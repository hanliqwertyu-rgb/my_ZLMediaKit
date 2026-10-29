# MP4 / 普通 HLS 断流补帧实现笔记

本笔记对应本仓库的 `RecordGapFiller` 与普通 TS HLS 接入实现。MP4 与 HLS 共用编解码和补帧算法，HLS 额外经过 TS 适配。HLS-fMP4 和 DASH 没有接入补帧。

## 1. 目标与边界

目标是：录制已经启动、且同一媒体源仍被保留时，在没有有效画面的区间继续输出黑色视频；启用音频时，在缺少音频的区间输出静音。推流恢复并收到可解码的关键帧后，恢复真实画面。

这不是原始编码帧无损直存方案。正常画面也会解码后重新编码，因此画质、帧数和时间戳不保证与源文件完全一致。固定输出帧率和接收时间轴也不保证录像时长精确等于源文件的 `format.duration`。

一个录制任务停止后不再补帧。媒体源已经销毁后，补帧无法继续保留其直播上下文。默认关闭自动录制时，新建媒体源需要业务重新调用开始录制接口。

## 2. 文件与修改位置

| 文件 | 职责 |
| --- | --- |
| `src/Extension/RecordGapFiller.h` | 声明通用帧补齐接口和 `TsGapInput` 接口 |
| `src/Extension/RecordGapFiller.cpp` | FFmpeg 工作线程、时间轴、黑帧/静音生成、TS 适配及有界输出队列 |
| `src/Record/Recorder.cpp` | 在 MP4 工厂分支包装 `MP4Recorder` |
| `src/Record/HlsRecorder.h` | 在普通 `HlsRecorder` 内部创建 TS 补帧入口并转发数据 |
| `tests/test_record_gap_fill.cpp` | 禁用路径、无效参数、工作线程调度隔离、低帧率、空闲上限、轨道重置、绑定前缓存、TS 数据和重复停止回归 |
| `tests/CMakeLists.txt` | 将离线回归加入 CTest 白名单 |
| `tests/integration/record_gap_fill.py` | 独立服务的真实 FFmpeg/RTMP/RTSP、MP4/HLS 解码与黑帧/静音测试 |

既有的 `tools/test_rtmp_reconnect.py`、对应 Python 回归和 `tools/rtmp-reconnect.md` 是文件推流测试工具；其中源文件 EOF、重连耗时补偿等属于测试脚本行为，不属于服务端补帧算法。

本次没有改造 `MultiMediaSourceMuxer`、录制 API、MP4/HLS 封装器底层或 `CloudUploader`。HLS 的复杂适配代码放在补帧 `.cpp`，没有放到原有 HLS 头文件中。

## 3. 两条输出路径

```text
MP4：
原始轨道/帧
  → Recorder::createRecorder(type_mp4)
  → RecordGapFiller
  → MP4Recorder
  → MP4Muxer / MP4 文件

普通 HLS：
原始轨道/帧
  → HlsRecorder
  → TsGapInputImpl
  → RecordGapFiller
  → GapTsSink / MpegMuxer
  → TsDelivery
  → 已绑定 EventPoller 上的输出回调
  → 原有 HlsMakerImp
  → TS 切片 / m3u8
```

每种格式创建自己的补帧实例。两种格式共用同一份算法代码，不代表共用一个编码器；同时开启时有两份转码开销，也可能有少量调度时间差。

## 4. MP4 接入

原来工厂直接返回 `MP4Recorder`，现在增加一层包装：

```cpp
auto recorder = std::make_shared<MP4Recorder>(
    tuple, path, option.mp4_max_second);
return wrapRecordGapFiller(
    std::move(recorder), option.continue_push_ms, option.enable_audio);
```

MP4 调用方使用 `MediaSinkInterface::Ptr`，可以直接接受补帧包装对象。自动录制和 `/index/api/startRecord` 最终都经过这个工厂，因此共用同一个接入点。

`record_gap_fill.enable=0` 时，`wrapRecordGapFiller` 返回原始录制器，不启动补帧线程。

## 5. HLS 为什么需要适配层

上层保存 `HlsRecorder::Ptr`，还会调用监听器、观看人数、启用状态等方法，并对工厂结果执行具体类型转换。直接从 HLS 工厂返回 `RecordGapFiller` 会导致类型转换失败。

因此外层对象仍然是 `HlsRecorder`，内部只增加 `_gap_input`：

- 构造：`createTsGapInput(retention_ms, audio_enabled)`。
- 轨道、帧、重置、刷新：开启补帧时转发到 `_gap_input`。
- 监听器就绪：调用 `bindOutput`，传入输出执行器和回调。
- 析构：先 `stop()`，再执行原有基类刷新。
- 补帧关闭：工厂返回空指针，继续走原有 HLS 方法。

`HlsRecorderBase` 和 `HlsFMP4Recorder` 没有改动。当前要求 `hls_demand=0`；不支持补帧实例随观看人数自动暂停、恢复。

### 5.1 TsGapInputImpl：启动和停止

自动启动和 API 启动的顺序不同。API 的 `makeRecorder()` 可能先添加轨道、通知完成、回放 GOP，之后才设置 HLS 监听器。

适配层会缓存绑定前的轨道和帧，等“轨道添加完成”和“输出已绑定”两个条件都满足后再创建内部补帧器。绑定前帧缓存限制为 2048 帧、32 MiB，用于容纳完整的音视频 GOP 启动重放；缓存 GOP 进入补帧器时仍按当时接收时钟处理，不承诺精确回溯历史录像时间。

前端操作由流所属线程串行调用。首次绑定确定本适配器的串行输出执行器；重复绑定不重建工作线程、不迁移输出队列。媒体源轨道重置通过有顺序的 reset 事件处理。

### 5.2 GapTsSink：音视频格式适配

现有补帧器输出 H.264 编码帧和裸 AAC 数据，不能不加处理地直接送入 MPEG-TS：

- 使用 `AACTrack` 为裸 AAC 添加 ADTS 头，满足 `MpegMuxer` 的要求。
- 保存输出视频轨道的 SPS/PPS，并在关键帧前按相同时间戳补入，保证 TS 端能取得视频参数。
- `MpegMuxer` 封装后，通过 `onWrite` 输出 TS 数据。

这个适配器仅在补帧工作线程运行，不访问 `HlsRecorder` 或 `HlsMakerImp`。

### 5.3 TsDelivery：线程和生命周期

补帧线程不直接修改 HLS 切片和播放列表，而是将 TS 数据按顺序投递到首次绑定的 EventPoller。投递明确使用 `async(..., false)`，避免同步执行打乱输出顺序。

输出队列限制为 256 个待处理 TS 数据任务、32 MiB。过载或写入异常会记录错误并将输出标记为失败；后续输入不能据此解释为录制成功。

输出任务持有独立状态，状态中的回调持有 `HlsMakerImp`，不捕获外层 HLS 对象的裸 `this`。因此停止后，剩余 TS 任务仍能使用有效的输出目标。

## 6. 补帧核心的工作过程

1. 网络/流线程将原始轨道、帧、重置和刷新放入输入事件队列；帧保留可缓存引用。
2. 每个补帧实例有一个独立工作线程，处理解码器和编码器，空闲时约每 10 ms 检查一次。线程启动时解除继承的单核亲和性；Linux 上将继承的调度策略改为普通 `SCHED_OTHER`，避免与网络线程争用同一核心及实时优先级。每轮先处理最多 64 个输入事件，再推进输出；仍有积压时，输出目标不会越过最早未处理事件的接收时刻减观察缓冲。
3. 输入视频解码后转换为 YUV420P。第一张成功解码画面对应的接收时刻作为该录制 epoch 的起点。
4. 按 `fps` 生成固定输出时间网格。正常情况下输出推进到“当前经过时间 − detect_ms”。
5. 当前位置有有效画面就输出真实画面；没有画面时输出黑色。最后一张真实画面至少保留 `detect_ms` 的媒体时间覆盖范围（若单帧间隔更长则取较大值，并有 2 ms 容差），容忍短暂抖动；更长的缺口再切换黑帧。因此黑帧起点不等于最后输入帧一结束就立即变黑。
6. 音频启用时，输出为 AAC、48 kHz、立体声。真实音频解码重采样后放到对应位置，空缺区间填零样本形成静音。
7. 输出视频重新编码为 H.264，禁用 B 帧，GOP 约为 `fps × 2`。当前编码使用 `libx264`、`veryfast`、`zerolatency`、单编码线程。

### 6.1 “断流检测”与“黑帧输出”不是同一个开关

短暂接收间隔超过 `detect_ms` 不再单独触发解码器清空。媒体 DTS 回退、接收间隔与 DTS 均发生明显跳变，或长间隔后收到配置帧/关键帧等新序列起始证据时，才重建解码状态并等待关键帧。明显跳变阈值取至少 1000 ms 或四个估计输入帧间隔。这样，同一连接暂停 300 ms 后继续发送连续 DTS 的普通帧，不会因为参考帧被清空而一直黑到下一个 GOP。

输入解码包分别保留原 DTS 和 PTS，避免把带 B 帧的解码顺序与显示顺序合并。输入时间映射仍可能按接收时钟偏离情况重新对齐；重建时间映射和清空解码器是不同操作。

完全没有新输入时，输出线程仍独立推进时间轴，按画面是否覆盖当前位置来选择真实画面或黑帧，不需要等 RTMP 断开回调才能工作。

因此 `detect_ms=200` 是观察缓冲和媒体间隔阈值，不是从录像中扣掉 200 ms。正常停止时会尝试处理剩余观察区间。已经写出的黑帧不会由迟到画面回写替换，过晚的真实画面可能被跳过。

### 6.2 时间戳与重连

输入时间戳回退、重新从零开始或偏离接收时间过多时，创建新的输入映射 epoch；输出时间轴不会因为普通重连而重新归零。显式 `resetTracks()` 则允许开启新的录制 epoch。

输出时间按固定帧序号生成，再转换到毫秒交给封装器。源文件时间戳、上游 ZLM 时间戳修正、接收时钟、输出帧网格和封装时长定义共同影响最终长度。不能把某个时长差直接归因于“丢了同等长度的画面”。

## 7. 停止和限制

`TsGapInput::stop()` 会停止接收新数据，并通过现有补帧对象析构等待工作线程结束。工作线程排空输入、处理剩余时间区间并刷新编解码器。TS 工作线程只提交输出，不等待 EventPoller，因此不存在“所属线程 join，而工作线程等待所属线程”的互等。

不过 `stop()` 会阻塞等待编码线程，其耗时取决于积压量；它不是完全异步的停止实现。已投递的 TS 任务仍需 EventPoller 继续运行，最后释放 HLS maker 后由原有逻辑完成最终切片和索引。`flush()` 也不等于停止和完整文件落盘。

API 返回成功仅表示接受了录制控制操作，不代表文件已经封装或上传完成。当前 `isRecording` 仍使用原有对象存在判断，不能代替输出健康检查。

保护措施保留：

- 输入事件在启动时最多 2048 个；输出已经开始且待处理输入已排空后，恢复为最多 256 个。帧数据始终最多 32 MiB，超限停止录制。轨道重置时重新进入启动阶段。
- 编码输出落后推进目标超过 2 秒时停止录制；该保护属于新增补帧代码，不是原生 MP4 功能。
- 连续无有效视频达到上限后暂停该时间轴，不能再靠普通晚到帧恢复；需要轨道重置或新媒体源。
- `max_ms` 会受 `continue_push_ms` 限制，但两个计时起点不同，不能认为补帧时刻与连接保留截止时刻严格一致。
- 新恢复的真实画面必须可解码，等待关键帧会延长黑帧区间。

## 8. 配置与构建

```ini
[protocol]
enable_mp4=1
enable_hls=1
hls_demand=0
continue_push_ms=15000

[record_gap_fill]
enable=1
detect_ms=200
max_ms=15000
fps=25
bitrate=2000000
```

以上表示自动录制。按需录制时将对应 `enable_mp4` / `enable_hls` 改为 0，流上线后调用 `/index/api/startRecord`，`type=1` 是 MP4、`type=0` 是普通 HLS；停止使用 `/index/api/stopRecord`。

补帧代码默认开关为关闭，当前仓库示例 INI 显式启用了它。开关在创建相应包装时读取，不会自动替换已存在录制任务。不要在同一任务初始化期间动态切换配置。

当前保留原有编译条件，即使只开启 HLS 补帧也需要 `ENABLE_MP4=ON`，同时需要 `ENABLE_FFMPEG=ON`、`ENABLE_HLS=ON`。FFmpeg 必须提供输入解码器、libx264，启用音频时还需要 AAC 编码器。

参数限制：detect_ms 为 40～2000，fps 为 5～60，bitrate 为 64000～100000000；max_ms 必须大于 detect_ms 且不超过 600000；与连接保留时间取小后的 max_ms 仍需大于 detect_ms。视频宽、高均须为偶数，并分别在 16～4096 之间。最多一路视频和一路音频，纯音频补帧不支持。

输入视频接入 H.264、H.265；音频接入 AAC、G.711A、G.711U。输出统一为 H.264/AAC。本次真实测试覆盖 H.264/AAC、无音频和 H.265/AAC，未测试 G.711。

```bash
cmake -S . -B build -DENABLE_FFMPEG=ON -DENABLE_MP4=ON -DENABLE_HLS=ON
cmake --build build --target MediaServer test_record_gap_fill -j 4
```

项目原有 CMake 配置步骤会把 `conf/config.ini` 复制到可执行目录；运行目录内单独修改过的配置可能被覆盖。部署时应明确使用 `MediaServer -c /path/to/config.ini`，并管理好配置文件。

## 9. 云上传关系

MP4 文件完成、TS 切片完成仍通过原有事件通知上传模块。补帧不依赖 MinIO，不会直接发送上传请求。

普通 HLS 的切片通知受 `[hls] broadcastRecordTs` 控制。完整 HLS 云端播放还需要上传 m3u8，以及保证相对路径和资源保留策略正确；现有切片上传不能自动代表这项工作已经完成。正在被本地播放列表引用的切片不应因上传成功被立即删除。

## 10. 验证与结果

离线回归：

```bash
ctest --test-dir build --output-on-failure -R '^test_record_gap_fill$'
python3 -B -m unittest discover -s tests/integration -p test_rtmp_reconnect_script.py -v
```

真实媒体集成测试：

```bash
python3 -B tests/integration/record_gap_fill.py \
  --server release/linux/Debug/MediaServer \
  --output-directory out/gap-fill-manual-run
```

输出目录必须尚不存在。脚本使用独立端口、随机 API 密钥，关闭 Hook 和云上传，保留录像、播放列表、FFmpeg 日志和 JSON 报告。无需停止现有服务。

首次接入时五组实测通过（当时黑场检测下限为 300 ms，不能代表无单帧闪黑）：

| 测试 | 检查 |
| --- | --- |
| auto | 同时自动录制 MP4/HLS；黑帧、静音、恢复、断流期间索引更新 |
| api | 自动录制关闭后 API 开启；断流恢复；停止时流继续在线；前次封装完成后再次开启 |
| video-only | 音频关闭，MP4/HLS 只有视频轨道 |
| disabled | 补帧关闭，原有输出可解码、没有插入黑帧 |
| hevc | RTSP H.265/AAC 输入，MP4/HLS 转为 H.264/AAC，断流后恢复 |

验证不是仅检查“进程不报错”：ffprobe 检查轨道和时长，FFmpeg 严格解码，blackdetect 验证真实画面前后夹有黑帧，silencedetect 验证静音，断流等待中检查 HLS 列表确实增加切片。2026-10-27 起 blackdetect 检测下限改为 1 ms，用于发现一个输出帧长度的闪黑；该检测验证黑场，不等于逐帧画面保真或卡顿检测。

2026-10-26 的 auto 样例：MP4 为 8.639 秒、HLS 列表为 8.600 秒，黑帧均为 2.600 秒；API 首次录像分别为 7.594 / 7.560 秒，黑帧均为 2.520 秒。样例有主动断流、重连和帧边界差异，不是源文件等长性保证。

本次集成汇总位于 `out/gap-fill-verification/integration-report.json`，各测试日志和录像在对应 `out/gap-fill-integration-*` 目录。集成测试尚未覆盖高分辨率长时间压力、进程崩溃恢复、G.711、HLS 按观看人数暂停、HLS-fMP4 或完整云端 HLS 发布。

## 11. 可用于汇报的表述

在原有 ZLMediaKit 的 MP4 录制入口和普通 HLS 录制器内部接入通用补帧模块：按接收时间维持连续媒体时间轴，缺少有效内容时生成黑帧和静音，恢复后继续真实画面；HLS 通过独立 TS 适配和有界线程投递复用原切片与播放列表生成逻辑。自动录制和 API 按需录制均已验证。当前采用重新编码方案，支持范围和资源保护有明确限制，录像时长与原文件存在差异时需结合时间戳及媒体内容判断。

## 12. 2026-10-27 正常区间闪黑修复

复现：带 B 帧、长 GOP 的连续推流，同一连接暂停 300 ms 后恢复，不丢包、不重置媒体时间戳。旧代码生成约 5.24 秒黑场，直到下一关键帧才恢复。

修复集中在 `RecordGapFiller.cpp`：

- 接收抖动不再独立触发解码器清空；保留连续码流的参考帧。
- 解码包分别传入 DTS/PTS。
- 最后画面保留到观察容差结束，避免只保留一个输出帧后马上切黑。
- 批量处理输入，并限制输出推进目标，避免队列内仍有真实画面却先写黑帧。
- 停止排空时，输出推进目标不超过已经记录的停止时刻。
- 记录输入序列重建的 idle_ms 和 dts_step，便于后续区分码流跳变和接收抖动。

回归新增：固定输出 25 fps、输入实际 12.5 fps 且 SPS 帧率仍为 25 的低帧率测试；连续 B 帧、300 ms 暂停以及真实高分辨率文件的 MP4/HLS 集成验证。短暂缺口允许重复最后画面，真正断流仍需检测到持续黑场和恢复后的真实画面。

完整原始素材验证命令（输入文件本身不含音轨，按业务路径由服务器添加静音）：

```bash
python3 -B tests/integration/record_gap_fill.py \
  --server release/linux/Debug/MediaServer \
  --output-directory out/gap-fill-source-check \
  --case source --input /path/to/source.mp4 --mute-audio --seconds 74
```

这次验证用于修复误补黑帧，不保证转码后逐帧内容、输出时长或 CPU 使用与源文件完全相同；CPU 持续不足时输入队列和两秒编码落后保护仍会停止录制。

修复验证结果：

- 最终构建的七组真实媒体回归（bframes、jitter、auto、api、video-only、disabled、hevc）全部通过，所有 MP4/HLS 都实际解码并按单帧黑场阈值检查。
- 同连接 300 ms 暂停：旧版输出 5.24 秒黑场；修复版 MP4、HLS 都未检测到黑场。
- 完整 73.934 秒的 1440×1080、30 fps、带 B 帧原视频，按服务器添加静音轨道的路径验证：MP4 73.799 秒、HLS 73.760 秒，均未检测到黑场，音视频均可解码。该运行的时长差只作观测值，不作为新误差上限。
- CTest 五项门禁通过；文件推流脚本的 14 项 Python 回归通过。
- 汇总报告：`out/gap-fill-flicker-verification/report.json`。各组媒体、服务日志和原始检测结果保存在 `out/gap-fill-flicker-*`、`out/gap-fill-jitter-*` 和 `out/gap-fill-real-source-after`。

此前线上录像已测得 15 段、合计 62.4 秒黑场，原文件无黑场；这证实旧版存在误补黑帧，但不能据此认定每一段线上黑场都由同一个运行时条件触发。可确定复现并修复的是接收抖动误清空参考帧，以及过短画面覆盖和输入/输出推进顺序的问题。

## 13. 继续闪黑排查：补帧线程继承了网络线程的调度设置

### 13.1 本次确认的事实

2026-10-27 17:50 的线上 MP4 仍检测出 13 段黑场，单段 0.24～1.64 秒、合计 8.84 秒。推流命令为 `ffmpeg -re -i source.mp4 -c copy -f flv rtmp://127.0.0.1/live/test`。该录像期间没有输入序列重置或队列溢出的对应日志，因此不能直接沿用上一节的解码器重置结论。

继续检查发现，服务默认启用 `--affinity 1`，每个 EventPoller 绑定一个核心。补帧器在流所属 EventPoller 中创建 `std::thread`，Linux 子线程会继承创建线程的 CPU 亲和性与调度策略。因此，原实现的 MP4 编码线程、HLS 编码线程和该流的网络线程会挤在同一个核心。

现场服务使用 root 启动，读取线程状态确认其网络线程是 `SCHED_FIFO`、优先级 99。补帧线程继承这种实时调度也存在风险：耗 CPU 的转码工作可能影响收包调度。整个机器存在空闲核心，并不代表这个绑定核心没有过载。

此前集成测试固定使用 `--affinity 0`、400 kbps，遗漏了这一运行差异。对照测试改为 16 个 EventPoller、2 Mbps、同一完整原视频并同时开启 MP4/HLS、TS/fMP4 输出：

- 关闭绑核的旧实现：MP4、HLS 均无黑场，测试通过。
- 开启绑核的旧实现：实际读取到两个编码线程都只允许在网络线程所在的核心运行，出现输入队列溢出，测试失败。
- 开启绑核的修复实现：编码线程可用核心为 `0-15`、调度策略为 `SCHED_OTHER`；MP4 73.839 秒、HLS 73.720 秒，均无黑场、无队列溢出，完整测试通过。

**证据边界：**上述对照确认了线程调度继承导致的过载缺陷；隔离测试运行于普通用户，未直接复跑 root 的 `FIFO/99` 场景。旧实现的绑核测试复现的是过载停录，并未重现现场那 13 段黑场的完整模式，因此不能声称已逐段证明其唯一原因。当前 root 进程仍需要重启到新二进制，再用同一命令验证。

### 13.2 具体修改

`RecordGapFiller::run()` 开始处先设置工作线程属性，再打开 FFmpeg 编解码器：

```cpp
setThreadName("record gap fill");
setThreadAffinity(-1);
#if defined(__linux__) && !defined(ANDROID)
sched_param priority{};
int error = pthread_setschedparam(pthread_self(), SCHED_OTHER, &priority);
if (error) {
    throw std::runtime_error(std::string("reset encoding thread scheduler: ") + std::strerror(error));
}
#endif
```

不更改网络线程的绑核与优先级，不更改 `detect_ms` 或停录保护。这里的 `setThreadAffinity(-1)` 复用 ZLToolKit 的工具函数，允许编码线程在可用核心上调度；Linux 容器或 cpuset 的限制仍由内核约束。

补充三类 DEBUG 日志：`clock reanchor` 记录接收时钟与媒体时间重新对齐；`decoded picture too late/caught up` 记录迟到画面的丢弃与恢复；`black started/picture recovered` 记录实际补黑/恢复的输出时刻、最后画面时刻与最近视频接收时刻。每条含补帧实例地址，便于区分并行输出。日志用于定位后续复现，出现黑场本身并不等于已找到根因。

### 13.3 回归与复现

新增 C++ 回归从单核绑定、`SCHED_BATCH` 的父线程创建补帧器，再在下游实际调用中检查工作线程已使用普通调度与多核掩码。该用例不需要 root；修复前失败于“继承父线程调度策略”，修复后通过。它验证继承隔离机制，不替代 root 实时调度的线上复测。

集成脚本默认改为 `--affinity 1`、`--bitrate 2000000`，提供 `--threads`、`--affinity` 和 `--bitrate` 参数。完整素材可按以下命令检查：

```bash
python3 -B tests/integration/record_gap_fill.py \
  --server release/linux/Debug/MediaServer \
  --output-directory out/gap-fill-source-affinity-check \
  --case source --input /path/to/source.mp4 --mute-audio --seconds 74 \
  --threads 16 --affinity 1 --bitrate 2000000
```

本轮修复后七组 MP4/HLS 真实媒体测试、五项 CTest、十四项推流脚本测试全部通过。另测同连接暂停 900 ms，旧版只出现该暂停对应的一个黑场，没有复现恢复后的反复闪黑，因此本轮未据此额外修改时间映射算法。

结果与原始文件：`out/gap-fill-affinity-before`、`out/gap-fill-affinity-after`、`out/gap-fill-scheduling-final-all`、`out/gap-fill-scheduling-verification/report.json`。构建时间横幅可能沿用未重编译的 `main.cpp` 对象，不能单凭该横幅判断是否加载了新补帧代码。

## 14. 多次按需录制只有首轮成片：缓存 GOP 导致解码画面队列溢出

2026-10-28 三轮 MP4 按需录制，每轮 20 秒、间隔 3 秒，服务日志记录三次开始、停止接口均成功，
但第二轮 17:33:23.133、第三轮 17:33:46.148 的补帧线程报
`record_gap_fill: recording stopped: decoded video queue overflow`，只有第一轮生成文件。
这次不是测试脚本少调用两轮，也不是云上传删除文件。

### 14.1 原因

`MultiMediaSourceMuxer::makeRecorder()` 创建录制器后，会通过 `flushGop()` 将现有 GOP 缓存送入新对象。
长 GOP 可能包含数秒、上百张画面，但重放到补帧器时，所有帧的接收时刻只相差几毫秒。
因此源 PTS 很快增长数秒，接收时钟几乎不动，`mapTime()` 会多次重新建立源时间与接收时间的映射。

原实现只更新映射锚点，没有清理按旧映射生成、尚未输出的 `_pictures`。
旧画面与新画面混在同一个输出时间范围内，逐次累积到 120 张上限，工作线程抛异常退出。
音频 `_audio_chunks` 存在相同的跨映射缓存问题。

外层 `_mp4` 指针仍存在，所以 `isRecording` 仍可能返回 true，`stopRecord` 也能成功删除对象。
这说明录制接口状态与内部编码工作线程健康状态不同；当前接口未新增线程健康状态字段。

### 14.2 修改

修改集中在 `RecordGapFiller::mapTime()`：每次开始新的输入时间映射时，清理对应旧映射下尚未输出的缓存。

```cpp
if (video) { _pictures.clear(); }
else { _audio_chunks.clear(); }
valid = true;
source = pts == AV_NOPTS_VALUE ? 0 : pts;
target = video && _video_frame_index == 0 ? 0 : now;
```

已经编码写出的样本不回写，解码器参考帧也不因此清空；保留解码画面 120 张和两秒编码落后保护。
输入队列的正常录制上限仍为 256 事件；随后第 15 节为启动重放增加了单独的有界容量。
`clock reanchor` 日志增加 `discarded` 数量，表示这次清理的旧映射缓存条数。
仅扩大队列不能解决旧映射画面继续累积或覆盖新画面的问题，因此本次没有提高队列容量。

启动阶段尚未写出视频时，上述代码将新视频映射的目标锚点放在输出 0 时刻。
否则缓存重放耗费几毫秒后清理旧画面，会让新队列的第一张画面从几毫秒处才生效，
输出 0 时刻被误填黑。该问题在 API 重启回归中实际出现过 1 ms 首帧黑场，已加入启动覆盖回归。

### 14.3 验证方式

- C++ 新增 `testCachedGopRemapping`：毫秒内送入 180 张带连续媒体时间戳的可解码画面，再继续实时送帧。
  修复前复现队列溢出并拒绝后续实时输入；修复后要求持续输出且无黑帧。
- 集成脚本新增 `preroll`，并加入 `all`：使用 B 帧和 9.6 秒 GOP 的输入，先推流 7 秒，
  再通过 API 同时开启 MP4/HLS，录制约 2 秒后停止，检查两份输出都存在、可解码、无黑场且时长合理。
- 使用原始 1440×1080、30 fps 视频及服务器静音轨道，调用实际 Python API 测试脚本，
  完整验证 `--count 3 --seconds 20 --interval 3`，逐份检查生成文件，而不只检查接口返回值。

修复前复现记录：`out/record-api-preroll-before`、`out/record-api-preroll-before-unit.log`。
启动覆盖修复前的回归失败记录为 `out/record-api-preroll-startup-before.log`。
最终修复后记录：`out/record-api-preroll-final-all`、`out/record-api-original-repeat-final`、
`out/record-api-preroll-final-ctest.log`。
原视频完整三轮复测生成三份 MP4，本轮实测均为 20.010 秒、可严格解码且未检测到黑场；
这个时长只是本次运行结果，不是以后每轮都达到或超过 20 秒的保证。新二进制需重启服务后生效。

## 15. 启动输入队列溢出与运行进程确认

后续 17:56 的三轮测试仍只生成首轮文件。现场 PID 2301818 的启动时间是
2026-10-28 17:19:17，早于第 14 节修复版二进制的 17:45:52，说明旧进程尚未加载修复。
同时，该次第二轮在 17:56:48.952 报 `input queue overflow`，第三轮报 `decoded video queue overflow`。
这揭示了两处不同的容量问题；第 14 节只修复了解码后画面跨映射累积。

在上一轮新二进制上，使用长 GOP、25 fps 视频和 48 kHz AAC，先推流 7 秒再开启录制，
独立复现了输入队列溢出：七秒历史数据约有 175 个视频帧和三百多个 AAC 帧，
集中重放明显超过原有 256 个事件上限。这不能仅解释为编码器持续跟不上。

处理仍集中在 `RecordGapFiller.cpp`：

- 为录制器初始化、GOP 重放阶段设置 2048 个输入事件的上限。
- 当视频输出已开始且共享输入队列排空后，在互斥锁内恢复正常的 256 事件上限。
- 接收 `Reset` 时重新进入启动阶段；全过程保留 32 MiB 输入字节限制与两秒编码落后保护。
- HLS 的绑定前缓存同样使用 2048 个帧上限、32 MiB，避免尚未绑定输出时就拒绝同一份 GOP。
- 溢出日志增加事件数、实际事件上限、已有字节数、输入帧字节数和是否在启动阶段，方便区分容量和持续积压。

新增 C++ `testStartupInputBurst`：暂时阻塞输出初始化，让调用方集中提交约 600 个历史帧，
再释放工作线程、继续实时送帧并解码检查输出。该测试要求启动重放被接受且无黑场，
用于覆盖编码器初始化期间生产者暂时快于消费者的情况。

新增集成 `preroll-av`，保留原纯视频 `preroll`：两种情况都检查 MP4/HLS 成片，
前者同时包含音频，以覆盖超过 256 个缓存事件的实际场景。两个用例均加入 `--case all`。

输入溢出修复前证据在 `out/record-api-input-burst-before/report.json`：
`input_overflow=true`、`decoded_overflow=false`、没有生成文件。
最终回归保存在 `out/record-api-both-queues-final-all`、
`out/record-api-both-queues-original-repeat`、`out/record-api-input-burst-final-ctest.log`。
本轮九组媒体回归与五项 CTest 全部通过；原视频三轮录制均生成可解码、无黑场的 MP4，
实测时长均为 20.010 秒。汇总为 `out/record-api-both-queues-verification/report.json`。

重新编译只替换磁盘上的可执行文件；已经启动的进程继续使用原有程序。
不要仅通过 `main.cpp` 的编译日期横幅判断是否更新，应核对进程启动时间、实际二进制以及新增日志。
