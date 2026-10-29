# Ubuntu 22.04 / x86_64 部署

本目录用于部署当前仓库的定制 MediaServer，包含 MP4/HLS 断流补帧及 MinIO 上传代码。
采用 Ubuntu 22.04 多阶段 Release 镜像和 Docker Compose，配置、录像、日志保存在宿主机。
本目录不会修改现有 `conf/config.ini` 或 `release/linux/Debug/config.ini`，也不会自动停止现有服务。

默认采用 API 按需录制：`enable_mp4=0`、`enable_hls=0`，通过录制接口分别开启。
运行账户为 UID/GID 10001，HTTP 8080、RTMP 1935、RTSP 8554；可在初始化时改端口。
默认保留本地录像，云上传关闭，待填写目标服务器的 MinIO 地址和凭据后开启。
SRT、RTP 代理已经编译但默认不监听；本镜像未启用 WebRTC、Python 插件或桌面播放器。

## 1. 准备源码和工具

请将**当前包含全部修改和新增文件的工作区**同步到服务器，或保存到自己的版本库后拉取。
直接克隆上游 ZLMediaKit 不包含这里新增的功能。无需携带 `build/`、`release/`、`out/`。

在服务器的项目根目录检查：

```bash
docker version
docker compose version
docker buildx version
```

需要 Compose 和 Buildx 插件。若已经配置 Docker 官方 APT 软件源但缺少插件：

```bash
sudo apt-get update
sudo apt-get install docker-compose-plugin docker-buildx-plugin
```

若无法找到包，应先配置适用于 Ubuntu 的 Docker 官方 APT 源。
后面的构建使用 BuildKit；`Dockerfile.dockerignore` 只向构建器发送所需源码和无凭据模板，
排除现有配置、录像、日志、测试产物及本地编译结果。
不要用缺少 Buildx 的旧构建器构建，以免发送不必要的完整工作区。

## 2. 初始化外置配置

```bash
sudo python3 deploy/ubuntu22/init_config.py --directory /opt/zlm
```

生成以下文件和目录，API 密钥随机生成，不会打印到终端，也不会使用开发机的密钥：

```text
/opt/zlm/
├── .env                   # 镜像名、UID 和挂载目录，无凭据
├── config/config.ini      # 唯一实际运行配置，含 API/MinIO 凭据
├── config/certs/          # 预留自有证书目录，默认空，未启用 TLS 端口
├── data/www/              # MP4、HLS、截图
└── log/                   # MediaServer、FFmpeg 日志
```

初始化程序拒绝覆盖已有配置；以后直接编辑该配置文件。
自定义目录时，父目录应允许 Docker 挂载，配置和数据目录应允许指定 UID/GID 访问。
程序只对本次新建目录设置所有者，不递归接管已有目录。

若希望调整端口，可在第一次初始化时指定：

```bash
sudo python3 deploy/ubuntu22/init_config.py --directory /opt/zlm \
  --http-port 8080 --rtmp-port 1935 --rtsp-port 8554
```

Compose 使用 host 网络，没有 `ports` 映射。同一台服务器现有 MediaServer 若占用 1935，
应在维护窗口停止旧服务，或将新配置的 RTMP 端口改为其他未占用端口。

## 3. 构建、启动

以下命令均在项目根目录执行。默认镜像名为 `my-zlm:2026-10-29`：

```bash
sudo docker compose --env-file /opt/zlm/.env -f deploy/ubuntu22/compose.yaml config --quiet
sudo docker compose --env-file /opt/zlm/.env -f deploy/ubuntu22/compose.yaml build
sudo docker compose --env-file /opt/zlm/.env -f deploy/ubuntu22/compose.yaml up -d
sudo docker compose --env-file /opt/zlm/.env -f deploy/ubuntu22/compose.yaml ps
```

构建时安装 FFmpeg 开发库，检查 ENABLE_FFMPEG/MP4/HLS/OPENSSL 实际生效，
编译 Release 并运行 `test_record_gap_fill`；任一步失败将中止构建。
`ENABLE_API=OFF` 关闭的是 C SDK 构建，HTTP `/index/api/` 接口仍可使用。
运行层仅包含运行依赖、程序、分离调试符号及测试 API 脚本，未打包开发配置或真实密钥。
`/opt/zlm/BUILD_INFO` 保存程序及关键修改源码的 SHA256，用于核对构建产物。

默认最多四个编译任务。内存较小时，在 `/opt/zlm/.env` 将 `ZLM_BUILD_JOBS` 改为 2。
这是编译并行度，不是录制路数或运行线程数。

默认基础镜像为 `ubuntu:22.04`。需要锁定基础镜像时，在 `.env` 增加
`ZLM_BASE_IMAGE=ubuntu:22.04@sha256:已验证的摘要`；最终发布镜像也应保留其镜像 ID/摘要。
记录源码版本时可将 `.env` 的 `ZLM_SOURCE_REVISION` 改为自己的版本号。

## 4. 验证服务和按需录制

查看启动日志：

```bash
sudo docker compose --env-file /opt/zlm/.env -f deploy/ubuntu22/compose.yaml logs --tail 100 media
sudo docker compose --env-file /opt/zlm/.env -f deploy/ubuntu22/compose.yaml exec media \
  python3 /opt/zlm/healthcheck.py
sudo docker compose --env-file /opt/zlm/.env -f deploy/ubuntu22/compose.yaml exec media \
  cat /opt/zlm/BUILD_INFO
```

先向 `rtmp://服务器IP:1935/live/test` 持续推流，再在服务器运行：

```bash
sudo python3 tools/test_record_api.py test \
  --server http://127.0.0.1:8080 --config /opt/zlm/config/config.ini \
  --type mp4 --stream test --count 3 --seconds 20 --interval 3
```

整个三轮测试至少需要约 66 秒再加接口和处理耗时；原素材不足时需循环推流。
客户端上 `127.0.0.1` 指向客户端自身，远程调用时应替换为服务器地址，并使用同一个 API 密钥。

MP4 文件位于：

```text
/opt/zlm/data/www/record/live/test/日期/*.mp4
```

HLS 测试将 `--type mp4` 改为 `--type hls`，文件位于：

```text
/opt/zlm/data/www/live/test/hls.m3u8
/opt/zlm/data/www/live/test/日期/小时/*.ts
```

模板设置 `segNum=0`、`segKeep=0`，属于保留全部切片的录像模式；它不是滚动直播列表。
同一流多次 HLS 录制会复用默认播放列表路径，旧切片不等于仍在当前列表内。
若要分任务归档 HLS，应通过开始接口的 `customized_path` 给每次任务使用独立目录。

HTTP 健康检查只验证 API 响应，不代表每个录制工作线程正常，也不检查 MinIO。
Docker 的 `restart` 会处理主进程退出，不会仅因 `unhealthy` 自动重启，
也不能恢复“进程仍存活但某个录制线程已失败”的状态。
验收应检查实际文件数量、解码播放，以及日志中是否出现
`record_gap_fill: recording stopped`、`queue overflow` 或上传失败。

## 5. MinIO 和公网访问

编辑唯一部署配置：

```bash
sudo editor /opt/zlm/config/config.ini
```

使用现有 MinIO 时填写 `[cloud_upload]`：

```ini
enable=1
minio_enable=1
minio_endpoint=http://可访问的MinIO地址:9000
minio_bucket=video
minio_access_key=实际访问密钥
minio_secret_key=实际私钥
delete_after_upload=0
```

该模板使用 host 网络：若 MinIO 也在同一宿主机且发布了 9000 端口，可使用
`http://127.0.0.1:9000`；若 MinIO 在其他机器或未向宿主机发布端口，应使用可达地址。
桶需提前存在。HLS 逐切片上传还需设置 `[hls] broadcastRecordTs=1`；
当前上传事件只处理切片，不自动构建完整的远端 m3u8 归档。
需要本地 HLS 回放时，应保持 `delete_after_upload=0`，防止上传后删除播放列表引用的切片。

API 默认允许本机和私网地址；远程调用受 `[http] allow_ip_range` 限制，
应只添加实际管理端/代理地址。API 调试日志默认关闭，以免记录带密钥的查询参数。
模板不启用 HTTPS；面向公网部署时应配置信任证书的 HTTPS 入口及业务推流鉴权，
并在服务器安全组/防火墙中按实际用途开放端口。不要将 API 密钥或 MinIO 凭据写进镜像。

修改 INI 后重建容器使配置重新加载：

```bash
sudo docker compose --env-file /opt/zlm/.env -f deploy/ubuntu22/compose.yaml up -d --force-recreate
```

配置目录只读挂载，运行时配置修改应编辑宿主机文件；录制开始/停止接口正常可用。

## 6. 停止、更新与回退

```bash
sudo docker compose --env-file /opt/zlm/.env -f deploy/ubuntu22/compose.yaml stop
```

停止会发送 SIGINT 并等待最多 60 秒。维护时先停止录制、确认文件和上传结果，
再停止服务；需要保持直播连续时应另行设计多节点切换。

每次发布在 `.env` 中使用新的 `ZLM_IMAGE` 版本号，避免覆盖上一份已验证的镜像：

```bash
sudo docker compose --env-file /opt/zlm/.env -f deploy/ubuntu22/compose.yaml build
sudo docker compose --env-file /opt/zlm/.env -f deploy/ubuntu22/compose.yaml up -d --force-recreate
```

回退时将 `.env` 的镜像名恢复为旧版本，使用已有镜像启动：

```bash
sudo docker compose --env-file /opt/zlm/.env -f deploy/ubuntu22/compose.yaml up -d --no-build --force-recreate
```

挂载目录保留，不会因更换容器而自动删除录像。主程序以普通用户运行，未授予实时调度或特权模式。
发布后检查容器创建时间、镜像 ID 和 `BUILD_INFO`，不要仅凭日志中的 main.cpp 编译横幅判断更新是否生效。

## 7. 本次验证记录

2026-10-29 已在本机完成 Ubuntu 22.04 / amd64 的实际镜像构建与隔离容器测试：

- Release 镜像构建成功，构建内的补帧 C++ 回归通过。
- 容器以 `10001:10001` 和移除全部 capabilities 的配置运行，配置目录只读挂载。
- 三轮 MP4 按需录制均生成可解码文件，每轮设置 2 秒，本次实测均为 1.999 秒。
- HLS 按需录制生成可解码的 m3u8/TS，本次实测 3.000 秒。
- 所有测试输出未检测到黑场；API 健康检查与 Docker `healthy` 状态通过。
- Compose 停止时主进程退出码为 0。
- 镜像 `BUILD_INFO` 中关键源码哈希与当前工作区一致。

验证结果在 `out/deploy-ubuntu22-check/report.json`，构建日志在
`out/deploy-ubuntu22-buildkit.log` 和 `out/deploy-ubuntu22-final-build.log`。
测试使用独立随机端口及合成素材，没有验证目标服务器网络、真实 MinIO 上传或公网 TLS，
也没有替换现有服务。容器验收不代表已经完成目标服务器部署。
