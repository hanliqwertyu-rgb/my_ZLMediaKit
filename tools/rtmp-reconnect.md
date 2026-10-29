# RTMP 断流重连测试

在项目根目录运行，需已启动启用 MP4 录制和断流补帧的 MediaServer，并安装 `ffmpeg`、`ffprobe` 和 Python 3。

```bash
python3 tools/test_rtmp_reconnect.py \
  --input /path/to/source.mp4 \
  --push-seconds 20 \
  --disconnect-seconds 5 \
  --reconnect-count 2
```

默认使用本机 RTMP 地址和唯一测试流名。前两段各推流约 20 秒后强制断开，等待约 5 秒重连；最后一段持续到源文件 EOF，不再限制为 20 秒。若需要指定地址，添加 `--url rtmp://127.0.0.1:1935/live/drone_test`。建议避免测试期间其他推流器使用相同流名。

- `--mode live`（默认）：以第一段输出进度对应的单调时钟为基准，后续源位置跟随实际经过时间推进，断流期间的源内容被跳过。每段的实际耗时减输出时长，用作下一段 FFmpeg 启动、握手等开销的估计并提前跳过相应源内容，减少多次重连造成的累计时长偏差。
- `--mode resume`：从断点继续发送，保留源内容；补黑帧后录像通常比源文件更长。
- `--final-seconds 20`：显式限制最后一段。此选项可能截断源文件尾部，报告会显示剩余时长。
- `--reconnect-count 0`：连续推流到 EOF，作为无中断基线。
- `--record-directory /path/to/stream-recordings`：明确指定该流的录像目录。默认检查 `release/linux/Debug/www/record`、`release/linux/Release/www/record` 和项目下的 `www/record`，并附加应用名和流名。
- `--finalize-seconds 20`：推流结束后等待服务器完成录像的时间。应覆盖服务器实际的断流保留、封装和磁盘写入时间。

报告和每段 FFmpeg 进度保存在 `out/rtmp-reconnect/<运行编号>/`。`report.json` 包含源文件时长、每段源起点、输出进度、实际耗时、断流等待、源文件是否到达 EOF、未覆盖尾部、录像总时长以及录像与源文件的时长差。`source_consumed_seconds` 包含 live 模式跳过的源内容，不表示实际发送的画面总时长。源文件进度依据 FFmpeg 输出时间估算，最后一段成功读到 EOF 时按源文件结束位置记录。

live 模式的 `source_timeline_policy` 为 `monotonic_with_startup_estimate`。每段的 `clock_source_offset_seconds` 记录统一时钟位置，`estimated_startup_seconds` 记录采用的启动耗时估计，`wall_minus_output_seconds` 记录该段实际耗时与输出进度的差。该差值也包含编码、调度和进度采样影响，并非精确的网络握手时长。补偿采用上一段的观测值，启动耗时突变时仍会有残差；不会通过裁剪生成的录像来强制凑齐时长。源视频较短而启动开销较大时，脚本可能在下一次重连前报告源文件耗尽，应减少中断次数或每段推流秒数。

报告中的 `publisher_test_completed` 只表示推流流程完成，不表示录像通过验收。`recording_status=not_found` 时无法比较录像时长：先检查录像目录和 MP4 开关；若配置了上传后删除本地录像，应在测试用服务配置中设置 `[cloud_upload] delete_after_upload=0`，或从云存储取回录像后单独分析。脚本不会更改服务器配置或自动访问云存储。

录像按接收时间生成。重连初始化、断流保留时间以及源文件和接收时间轴的差异，都可能影响最终时长。应分别比较连续推流、live 断流和 resume 断流的报告，结合服务日志与录像中的黑帧区间判断，不能仅凭录像比源文件短或长就判定补帧失败。

离线回归测试：

```bash
python3 -m unittest discover -s tests/integration -p test_rtmp_reconnect_script.py -v
```
