# 完整 TS HLS 归档到 MinIO：实现和使用

本功能在普通 TS HLS 录制结束后，将原始切片及最终播放列表保存到 MinIO。
不转码、不合并 TS、不改写切片顺序，适用于完整录像回放。
不是边录边从 MinIO 播放。功能默认关闭，需显式配置后重新启动进程。

## 1. 最小接入范围

业务 C++ 源码为“新增两个文件，修改一个已有文件”：

| 文件 | 内容 |
| --- | --- |
| 新增 `src/Extension/HlsArchive.h` | 存储无关的归档选项、对象数据与完成回调接口 |
| 新增 `src/Extension/HlsArchive.cpp` | 扫描完整列表、校验文件、保存任务、上传顺序、重试、恢复及可选清理 |
| 修改 `src/Extension/CloudUploader.cpp` | 复用签名和异步 PUT，安装/停止归档模块，保护归档目录内的切片 |

没有修改 RecordGapFiller、HlsRecorder、HlsMakerImp、Recorder、录制 API、main.cpp 或公共事件。
模块通过磁盘上的最终 `hls.m3u8` 发现任务，不需要增加录制完成事件或改动录制器析构顺序。
源码由现有 src/CMakeLists.txt 的 GLOB 收集；新增文件后重新执行 CMake 配置。

辅助修改：

- `tools/test_record_api.py`：新增 `--record-root`，每次开始录制生成独立子目录，传递现有 `customized_path` 参数。
- `tests/test_hls_archive.cpp` 和 `tests/CMakeLists.txt`：离线测试和 CTest 注册。
- `tests/integration/hls_archive.py`：独立 MediaServer、临时 MinIO、失败注入代理和真实远端回放验证。
- `deploy/ubuntu22/config.ini.example`：加入默认关闭的归档配置。
- `deploy/ubuntu22/Dockerfile`：构建归档测试并执行，将新增源码哈希写入 BUILD_INFO。
- 本文档及相关使用说明。

## 2. 数据处理流程

```text
独立任务目录内生成 hls.m3u8 和 TS
  → 两次扫描确认最终列表稳定、存在 ENDLIST、切片完整
  → 保存任务 JSON（含列表快照和切片校验信息）
  → 逐个上传 TS，每次成功后保存进度
  → 全部 TS 成功后上传原始 m3u8
  → 保存 ready 状态
  → 按选项清理已归档文件（默认不清理）
```

只有文件名恰好为 `hls.m3u8` 的列表会被发现；不扫描每小时的 `vod.m3u8`。
要求列表为普通、未加密 TS 媒体列表，具有 EVENT/VOD 类型、TARGETDURATION、完整 EXTINF/TS 项以及 ENDLIST。
滚动列表、主列表、fMP4、加密 KEY、BYTERANGE 和部分切片等扩展均不支持，遇到会拒绝归档并记录原因。
片段 URI 必须是无查询参数的相对 `.ts` 路径；拒绝父目录、绝对地址、编码路径及越界符号链接。

列表上限 8 MiB、单个 TS 上限 64 MiB、每任务最多 100000 个切片，扫描目录深度最多 16 层。
任务存储使用原子临时文件替换，POSIX 下先刷新任务文件；任务文件不包含 API 或 MinIO 密钥。
切片 MD5 用于识别本地内容意外变化，不作为安全签名。真正的上传签名仍使用现有 AWS Signature V4 / SHA256。
上传内容先在归档线程读取校验，之后签名与发送使用同一份字节快照，避免哈希与请求体不一致。

归档上传使用单个独立工作线程，逐对象等待异步 PUT 的结果；扫描、读取、哈希不在录制/网络线程执行。
已完成任务的完整清单保留在磁盘，内存只保留路径和指纹，避免长期运行一直持有所有历史切片清单。

## 3. 配置

部署服务器编辑 `/opt/zlm/config/config.ini`，将以下键合并到已有配置节，避免重复节名：

```ini
[protocol]
enable_hls=0
hls_demand=0

[hls]
segNum=0
segKeep=0
broadcastRecordTs=0

[cloud_upload]
enable=1
minio_enable=1
minio_endpoint=http://MinIO的S3地址:9000
minio_bucket=video
minio_access_key=实际访问密钥
minio_secret_key=实际私钥
minio_region=us-east-1
delete_after_upload=0

[hls_archive]
enable=1
scan_root=/var/lib/zlm/www/hls-archive
spool_path=/var/lib/zlm/archive-jobs
object_prefix=hls
scan_interval_sec=3
retry_delay_sec=3
retry_max=10
delete_local_after_archive=0
```

`scan_root`、`spool_path` 是容器内路径，现有 `/var/lib/zlm` 数据挂载已覆盖二者。
默认实际宿主机路径分别是 `/opt/zlm/data/www/hls-archive`、`/opt/zlm/data/archive-jobs`。
目录由归档模块创建，运行 UID 需对父目录具有写权限。

`segNum=0` 使主列表包含本轮所有切片，`segKeep=0` 在此模式下不会删除切片。
`broadcastRecordTs=0` 避免旧逐片上传。即使设为 1，CloudUploader 也会跳过归档扫描根目录内的 TS，
防止重复上传或触发旧 `delete_after_upload` 删除；其它目录的 TS 和原 MP4 上传行为保留。

归档目的地和凭据在启动模块时固定；修改 MinIO 配置后应重启，避免一个任务中途切换目的地。
切换 endpoint/bucket/object_prefix 后，扫描会将这些录像作为新目的地的任务处理。
桶需要提前存在；程序不会创建桶、修改访问策略或配置 CORS。

## 4. 每轮录制使用独立目录

从本地操作远端服务，例如 HTTP 为 8083，使用当前测试脚本：

```bash
python3 tools/test_record_api.py test \
  --server http://10.1.224.37:8083 \
  --secret '服务器完整API密钥' \
  --type hls --app live --stream test \
  --count 3 --seconds 20 --interval 3 \
  --record-root /var/lib/zlm/www/hls-archive
```

`--record-root` 填服务器/容器内的路径，不是执行脚本的电脑路径。
脚本每次 start 自动生成“时间 + 随机 ID”的根目录，并打印本次路径；原 API 追加 app/stream。
`status`、`stop` 不需要该参数。没有传入时保持原脚本行为，但不能自动确保每轮 HLS 目录独立。

直接调用 API 时，也可以自行生成唯一任务 ID 并传入：

```text
type=0
customized_path=/var/lib/zlm/www/hls-archive/任务ID
```

本地和远端目录一一对应：

```text
本地：scan_root/任务ID/live/test/hls.m3u8
      scan_root/任务ID/live/test/日期/小时/分-秒_序号.ts

MinIO 桶内：hls/任务ID/live/test/hls.m3u8
            hls/任务ID/live/test/日期/小时/分-秒_序号.ts
```

对象路径保留扫描根目录下的相对层次，没有使用 `RecordInfo.file_name` 推测切片位置。
归档目录禁止复用；已知路径的最终列表发生变化时记录错误，不覆盖原有任务。
支持源路径包含空格、加号及中文，网络请求按同一对象路径进行编码和签名。

## 5. 结果、重试、重启

`stopRecord` 成功表示结束录制请求完成，不代表已经上传。
查看日志中的 `hls_archive: ready`，或检查 spool_path 下任务 JSON：

| 字段 | 含义 |
| --- | --- |
| `status` | pending、retry_wait、ready、failed |
| `playlist_object` | 桶内最终播放列表对象路径 |
| `next` | 已确认并保存进度的对象数；列表是最后一个对象 |
| `attempts` / `retry_at` | 当前对象连续失败次数及下一次重试时间 |
| `error` | 最近失败原因 |

HTTP 非 2xx、连接异常或本地文件变化均不会被记录为成功。
重试等待逐步加倍，上限 60 秒；同一对象成功后失败次数清零，达到 retry_max 后任务保持 failed。
失败时保留本地数据，不能发布未满足依赖的播放列表。
修正问题后，停止服务、保留录像并移除对应 failed 任务 JSON，再启动，可重新发现并重试同一路径。
不要删除所有 ready 记录，否则重启后会重新上传历史任务。

停止模块会中断工作线程的等待，异步网络回调只访问独立状态，不持有已销毁的归档对象。
下次启动从持久化检查点恢复；上次请求已送达而未保存确认的对象可能再次 PUT，但对象路径和内容不变。
归档模块关闭期间完成的录像，下次扫描也能发现；进程突然崩溃且未生成 ENDLIST 的录像不会冒充完整归档。

默认保留本地文件。开启 `delete_local_after_archive=1` 后，仅在 ready 后校验并删除本任务引用的 TS 和主列表；
不会递归删除目录或其它文件。每小时 `vod.m3u8` 等辅助文件可能保留，但其引用的本地 TS 已不存在，不能继续本地回放。
有本地回放或其它消费者时应保持该选项为 0。

## 6. 播放

回放入口是 MinIO S3 API 或播放网关上的完整对象地址，例如：

```text
http://MinIO-S3地址:9000/video/hls/任务ID/live/test/hls.m3u8
```

播放器需要同时读取列表和所有 TS。私有桶建议通过同一路径的业务播放网关授权；
仅签发一个 m3u8 的预签名 URL 不会自动授权其相对 TS 地址。
浏览器跨域播放还需要正确 CORS 和支持 HLS 的播放器。
此模块只归档原有切片和列表，不实现播放器、公开桶策略或播放鉴权。

## 7. 构建和验证

```bash
cmake -S . -B build
cmake --build build --target MediaServer test_hls_archive -j 4
ctest --test-dir build --output-on-failure

python3 -B tests/integration/hls_archive.py \
  --server release/linux/Debug/MediaServer \
  --output-directory out/hls-archive-check
```

集成测试需要 Docker、FFmpeg/ffprobe 和含 MinIO 服务端的镜像，可通过 `--minio-image` 指定。
测试新建临时 MinIO 容器、随机凭据和桶；只允许临时测试桶匿名读取，以便真实播放器 GET 验证，
测试结束删除该容器，不使用或修改现有 MinIO 数据。

部署更新需重新构建镜像再重建容器。配置默认关闭，不会自动接管已有录像目录。

## 8. 本次验证结果（2026-10-30）

- 本机 Debug 程序与 Ubuntu 22.04 Release 镜像均构建成功。
- 六项 CTest 通过，包含新增归档回归；原推流脚本十四项 Python 回归通过。
- 原有九组媒体回归全部通过：bframes、jitter、preroll、preroll-av、auto、api、video-only、disabled、hevc。
- 本机及 UID 10001、移除 capabilities 的 Release 容器各自连接独立临时 MinIO 完成验证：
  三轮录制分别归档，一轮在上传失败后重启恢复，共四份 HLS 均可直接从 MinIO 连续解码。
- 本轮四份 HLS 均约 2 秒，未检测到黑场；每个远端 TS 与本地字节一致，列表与最终快照一致。
- 检查 Content-Type、播放列表最后上传、503 失败重试、空格/加号/中文对象路径。
- 已完成任务不重复上传，原 MP4 上传保持字节一致，启用 TS 广播也未重复上传归档目录里的切片。
- 镜像 BUILD_INFO 中 HlsArchive.cpp、CloudUploader.cpp 的 SHA256 与当前工作区一致。

汇总：`out/hls-archive-verification/report.json`。
原始记录：`out/hls-archive-minio-final`、`out/hls-archive-container-minio`、
`out/hls-archive-gap-regression`、`out/hls-archive-final-ctest.log`、`out/hls-archive-docker-build.log`。

测试使用合成 H.264/AAC 素材、HTTP MinIO 和随机端口；未验证目标服务器网络、公网 TLS、
浏览器 CORS 或业务播放鉴权，也未进行高并发容量测试。现有 MediaServer 和已有 MinIO 未被替换或重启。
