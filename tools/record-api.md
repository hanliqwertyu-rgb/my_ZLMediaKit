# Python 按需录制接口测试

脚本 `tools/test_record_api.py` 使用 Python 3 标准库，通过 GET 查询参数调用
`startRecord`、`stopRecord` 和 `isRecording`，不需要安装 requests。

先启动 MediaServer 并推流，例如 `rtmp://127.0.0.1/live/test`。在仓库根目录执行：

```bash
# 开始 MP4 录制
python3 tools/test_record_api.py start --type mp4 --stream test

# 查看 MP4 录制状态
python3 tools/test_record_api.py status --type mp4 --stream test

# 停止 MP4 录制，对应 /index/api/stopRecord?...&type=1&...
python3 tools/test_record_api.py stop --type mp4 --stream test

# HLS 的 type=0，可以写 hls 或 0
python3 tools/test_record_api.py start --type hls --stream test
python3 tools/test_record_api.py stop --type hls --stream test

# 完整测试：确认未录制 → 开始 → 查询 → 等待 10 秒 → 停止 → 查询
python3 tools/test_record_api.py test --type mp4 --stream test --seconds 10

# 重复录制 5 次，每次录制 20 秒，两轮之间等待 3 秒
python3 tools/test_record_api.py test --type mp4 --stream test \
  --count 5 --seconds 20 --interval 3

# HLS 重复录制 3 次，每次录制 10 秒，轮间等待 5 秒
python3 tools/test_record_api.py test --type hls --stream test \
  --count 3 --seconds 10 --interval 5
```

`test` 模式的循环参数：

| 参数 | 含义 | 默认值 |
| --- | --- | --- |
| `--count` | 录制次数，大于 0 的整数 | 1 |
| `--seconds` | 每次录制的秒数，大于 0，支持小数 | 10 |
| `--interval` | 本轮停止并确认后，下一轮开始前等待的秒数，可为 0 | 5 |

录制 N 次只有 N−1 次间隔，最后一轮结束后直接退出。例如录制 5 次、每次 20 秒、
间隔 3 秒，计划等待总计 `5 × 20 + 4 × 3 = 112` 秒，另有接口调用和服务器处理耗时。
每轮计时从开始接口成功返回后计算，随后查询状态的耗时计入本轮时长；文件最终时长仍可能受
关键帧等待、编码缓冲、帧边界和收尾影响，不能保证恰好等于 `--seconds`。

默认访问 `http://127.0.0.1`，读取
`release/linux/Debug/config.ini` 中 `[api]` 的 `secret`。若服务使用其他配置、端口或流：

```bash
python3 tools/test_record_api.py status \
  --server http://127.0.0.1:8080 \
  --config /path/to/config.ini \
  --type mp4 --vhost __defaultVhost__ --app live --stream test
```

也支持 `--secret '实际密钥'` 显式指定。参数自动进行 URL 编码，输出的请求地址隐藏密钥。
`--config` 只读取密钥，服务器地址通过 `--server` 指定。

`test` 模式每轮开始前要求该格式当前未在录制，避免测试结束时关闭已有任务；
任意一轮失败即退出，不继续后续轮次，输出失败轮次和已完成次数。
Ctrl+C 会尝试停止本轮启动的录制；在轮间等待时中断，不再发起下一轮。
整个测试期间应保持推流。MP4 与 HLS 分别控制，录制停止只影响指定格式。

HTTP 返回 200 不代表成功；脚本还检查 JSON 的 `code`、`result` 或 `status`。
成功退出码为 0，错误为 1，Ctrl+C 为 130。`status` 查到“未录制”也是成功查询。
`test` 验证接口行为，不验证文件时长、画面或是否已上传。
尤其是开启 `record_gap_fill` 时，`isRecording=true` 只说明外层录制对象仍存在，
内部工作线程失败仍可能保持该状态。若接口全部成功却缺少文件，需要检查服务日志中的
`record_gap_fill: recording stopped`，不能仅凭接口成功判断成片正常。

完全按需录制时，将 `[protocol]` 的 `enable_mp4`、`enable_hls` 设为 0，
保持 `hls_demand=0`（当前 HLS 补帧实现的要求）。
HLS 文件是否保留仍由 `[hls]` 的 `segNum`、`segKeep` 决定。

完整 HLS MinIO 归档需要每次录制使用独立目录，可增加：

```bash
python3 tools/test_record_api.py test --type hls --stream test \
  --count 3 --seconds 20 --interval 3 \
  --record-root /var/lib/zlm/www/hls-archive
```

`--record-root` 是服务器/容器内的绝对路径。脚本每次 start 都在它下面生成独立任务子目录，
通过已有的 `customized_path` 参数传给服务器。未传入该选项时保持原有保存路径。
归档功能需要另行启用 `[hls_archive]`，完整配置和检查方式见 `docs/hls-minio-archive.md`。
