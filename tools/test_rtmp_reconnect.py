import argparse
from contextlib import contextmanager
from datetime import datetime
import json
import math
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
from urllib.parse import urlsplit, urlunsplit
from uuid import uuid4

def bounded_integer(minimum, maximum):
    def parse(value):
        try:
            number = int(value)
        except ValueError:
            raise argparse.ArgumentTypeError("请输入整数。") from None
        if not minimum <= number <= maximum:
            raise argparse.ArgumentTypeError(f"数值必须在 {minimum}～{maximum} 之间。")
        return number

    return parse

def parse_arguments():
    parser = argparse.ArgumentParser(description="自动模拟 RTMP 推流中断、重连并统计录像时长。")
    parser.add_argument("--input", type=Path, help="输入视频；省略时自动生成测试视频")
    parser.add_argument("--url", help="推流地址；省略时使用本机地址和唯一的测试流名")
    parser.add_argument("--mode", choices=("live", "resume"), default="live", help="live 按统一时钟跳过断流内容并估计补偿重连开销；resume 从断点继续发送")
    parser.add_argument("--push-seconds", type=bounded_integer(1, 3600), default=10, help="每次强制中断前的推流秒数，默认 10；最后一段默认推到文件结束")
    parser.add_argument("--final-seconds", type=bounded_integer(1, 3600), help="最后一段最多推流秒数；省略则推到源文件结束")
    parser.add_argument("--disconnect-seconds", type=bounded_integer(1, 3600), default=5, help="断流等待秒数，默认 5")
    parser.add_argument("--reconnect-count", type=bounded_integer(0, 100), default=2, help="重连次数，默认 2；设为 0 可测试连续推流")
    parser.add_argument("--finalize-seconds", type=bounded_integer(0, 3600), default=20, help="等待录像完成写入的秒数，默认 20")
    parser.add_argument("--progress-timeout-seconds", type=bounded_integer(5, 3600), default=20, help="输出进度停滞超时秒数，默认 20")
    parser.add_argument("--record-directory", type=Path, help="该测试流的本地录像目录")
    parser.add_argument("--output-directory", type=Path, help="测试结果目录，必须尚不存在")
    return parser.parse_args()

@contextmanager
def encoder_process(ffmpeg, arguments, prefix):
    with Path(str(prefix) + ".stdout.log").open("wb") as stdout_log, Path(
        str(prefix) + ".stderr.log"
    ).open("wb") as stderr_log:
        process = subprocess.Popen(
            [ffmpeg, *map(str, arguments)],
            stdin=subprocess.DEVNULL,
            stdout=stdout_log,
            stderr=stderr_log,
        )
        try:
            yield process
        finally:
            if process.poll() is None:
                process.kill()
            process.wait()

def read_progress_seconds(path):
    try:
        content = path.read_text(encoding="utf-8", errors="replace")
    except FileNotFoundError:
        return 0.0
    entries = re.findall(r"^out_time_us=(\d+)\r?$", content, re.MULTILINE)
    return int(entries[-1]) / 1_000_000 if entries else 0.0

def probe_duration(ffprobe, path):
    result = subprocess.run(
        [ffprobe, "-v", "error", "-show_entries", "format=duration",
         "-of", "default=noprint_wrappers=1:nokey=1", str(path)],
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, timeout=15, check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(f"无法读取输入视频时长：{path}")
    try:
        duration = float(result.stdout.strip())
    except ValueError:
        raise RuntimeError(f"无法解析输入视频时长：{path}") from None
    if not math.isfinite(duration) or duration <= 0:
        raise RuntimeError(f"输入视频时长无效：{path}")
    return duration

def generate_video(ffmpeg, input_path, duration, output_directory):
    print(f"正在生成 {duration} 秒测试视频……", flush=True)
    with encoder_process(
        ffmpeg,
        [
            "-hide_banner", "-loglevel", "warning", "-nostdin",
            "-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25",
            "-f", "lavfi", "-i", "sine=frequency=1000:sample_rate=48000",
            "-t", duration, "-c:v", "libx264", "-preset", "ultrafast",
            "-pix_fmt", "yuv420p", "-g", "25", "-c:a", "aac", input_path,
        ],
        output_directory / "generate",
    ) as process:
        try:
            return_code = process.wait(timeout=max(30, duration * 2))
        except subprocess.TimeoutExpired:
            raise RuntimeError("测试视频生成超时，请查看 generate.stderr.log。") from None
        if return_code != 0:
            raise RuntimeError("测试视频生成失败，请查看 generate.stderr.log。")

def publish_segment(ffmpeg, args, source_offset, is_last, prefix):
    progress_path = Path(str(prefix) + ".progress.log")
    limit_seconds = args.final_seconds if is_last and args.final_seconds else None
    if not is_last:
        limit_seconds = args.push_seconds + 10
    command = [
        "-hide_banner", "-loglevel", "warning", "-nostdin", "-nostats",
        "-stats_period", "0.25", "-progress", progress_path,
        "-ss", f"{source_offset:.6f}", "-re", "-i", args.input,
    ]
    if limit_seconds is not None:
        command.extend(["-t", limit_seconds])
    command.extend([
        "-map", "0:v:0", "-map", "0:a:0?",
        "-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency",
        "-r", "25", "-g", "25", "-sc_threshold", "0", "-pix_fmt", "yuv420p",
        "-c:a", "aac", "-f", "flv", args.url,
    ])
    with encoder_process(
        ffmpeg,
        command,
        prefix,
    ) as process:
        last_progress_time = time.monotonic()
        sent_seconds = 0.0
        while True:
            return_code = process.poll()
            current_seconds = read_progress_seconds(progress_path)
            if current_seconds > sent_seconds:
                sent_seconds = current_seconds
                last_progress_time = time.monotonic()
            if return_code is not None:
                if return_code != 0 or sent_seconds <= 0:
                    raise RuntimeError(f"推流失败或未输出有效数据，请查看 {prefix}.stderr.log。")
                if not is_last:
                    raise RuntimeError(f"推流进程在模拟中断前提前退出，请查看 {prefix}.stderr.log。")
                break
            if not is_last and sent_seconds >= args.push_seconds:
                process.kill()
                process.wait()
                sent_seconds = max(sent_seconds, read_progress_seconds(progress_path))
                break
            if time.monotonic() - last_progress_time > args.progress_timeout_seconds:
                raise RuntimeError(f"推流输出进度停滞，请查看 {prefix}.stderr.log。")
            time.sleep(0.1)
        return sent_seconds

def probe_recordings(ffprobe, directory, started_at, output_directory, index_offset=0):
    recordings = []
    for recording_path in sorted(directory.rglob("*.mp4")):
        if recording_path.name.startswith("."):
            continue
        try:
            if not recording_path.is_file() or recording_path.stat().st_mtime < started_at:
                continue
        except FileNotFoundError:
            continue
        error_path = output_directory / f"recording_{index_offset + len(recordings) + 1:02d}.probe-error.log"
        duration = None
        with error_path.open("wb") as error_log:
            try:
                result = subprocess.run(
                    [
                        ffprobe, "-v", "error", "-show_entries", "format=duration",
                        "-of", "default=noprint_wrappers=1:nokey=1", str(recording_path),
                    ],
                    stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                    stderr=error_log, timeout=15, check=False,
                )
                if result.returncode == 0:
                    candidate = float(result.stdout.strip())
                    if math.isfinite(candidate) and candidate >= 0:
                        duration = round(candidate, 3)
            except (OSError, ValueError, subprocess.TimeoutExpired) as error:
                error_log.write(str(error).encode("utf-8"))
        recordings.append({
            "path": str(recording_path),
            "duration_seconds": duration,
            "status": "readable" if duration is not None else "probe_failed",
        })
    return recordings

def main():
    args = parse_arguments()
    repository_root = Path(__file__).resolve().parent.parent
    run_id = datetime.now().strftime("%Y%m%d_%H%M%S") + "_" + uuid4().hex[:8]
    args.url = args.url or f"rtmp://127.0.0.1:1935/live/reconnect_test_{run_id}"
    try:
        target = urlsplit(args.url)
        path_parts = target.path.strip("/").split("/")
        if target.scheme not in ("rtmp", "rtmps") or not target.hostname or target.fragment:
            raise ValueError("请提供完整的 RTMP 或 RTMPS 推流地址。")
        if target.port is not None and target.port == 0:
            raise ValueError("端口必须在 1～65535 之间。")
        if len(path_parts) < 2 or any(part in ("", ".", "..") or "\\" in part for part in path_parts):
            raise ValueError("推流地址必须包含应用名和流名，例如 /live/drone001。")
        ffmpeg = shutil.which("ffmpeg")
        ffprobe = shutil.which("ffprobe")
        if not ffmpeg or not ffprobe:
            raise ValueError("找不到 ffmpeg 或 ffprobe，请安装后将它们加入 PATH。")
        if args.input:
            args.input = args.input.resolve(strict=True)
            if not args.input.is_file():
                raise ValueError("输入路径必须指向视频文件。")
        output_directory = (args.output_directory or repository_root / "out" / "rtmp-reconnect" / run_id).resolve()
        output_directory.mkdir(parents=True, exist_ok=False)
    except (OSError, ValueError) as error:
        print(f"无法开始测试：{error}", file=sys.stderr)
        return 2

    if args.record_directory:
        record_directories = [args.record_directory.resolve()]
    else:
        record_roots = [
            repository_root / "release" / "linux" / "Debug" / "www" / "record",
            repository_root / "release" / "linux" / "Release" / "www" / "record",
            repository_root / "www" / "record",
        ]
        record_directories = list(dict.fromkeys((root / Path(*path_parts)).resolve() for root in record_roots))
    safe_authority = target.netloc.rsplit("@", 1)[-1]
    report = {
        "status": "failed", "error": None, "mode": args.mode,
        "target": urlunsplit((target.scheme, safe_authority, target.path, "", "")),
        "input_path": None, "record_directory": str(record_directories[0]),
        "record_directories": [str(path) for path in record_directories],
        "source_duration_seconds": None, "source_consumed_seconds": 0.0,
        "unpublished_source_seconds": None, "disconnect_wait_seconds": 0.0,
        "source_end_reached": False,
        "source_timeline_policy": "monotonic_with_startup_estimate" if args.mode == "live" else "resume_from_output_progress",
        "last_segment_policy": "duration_limit" if args.final_seconds else "source_eof",
        "recording_minus_source_seconds": None,
        "duration_comparison_note": "live 模式按统一时钟并用上一段耗时估计补偿重连开销，启动耗时变化、帧边界和服务器收尾仍可能产生偏差；resume 模式保留全部源内容，补黑帧后通常更长。",
        "push_wall_seconds": 0.0, "segments": [], "recording_status": "not_checked",
        "readable_recording_duration_sum_seconds": None, "recordings": [],
    }
    started_at = time.time()
    push_started_at = None
    try:
        if args.input is None:
            args.input = output_directory / "source.mp4"
            source_seconds = (args.reconnect_count + 1) * (args.push_seconds + 2) + args.reconnect_count * args.disconnect_seconds + 10
            generate_video(ffmpeg, args.input, source_seconds, output_directory)
        report["input_path"] = str(args.input)
        source_duration = probe_duration(ffprobe, args.input)
        report["source_duration_seconds"] = round(source_duration, 3)
        minimum_duration = args.reconnect_count * (
            args.push_seconds + (args.disconnect_seconds if args.mode == "live" else 0)
        )
        if source_duration <= minimum_duration:
            raise ValueError(f"源视频只有 {source_duration:.3f} 秒，不足以完成指定的中断重连次数；请减少 --push-seconds 或 --reconnect-count。")
        print(f"推流地址：{report['target']}\n模式：{args.mode}\n源视频：{source_duration:.3f} 秒\n最后一段：{report['last_segment_policy']}\n日志目录：{output_directory}", flush=True)
        push_started_at = time.monotonic()
        source_offset = 0.0
        live_origin = None
        startup_estimate = 0.0
        for segment_index in range(args.reconnect_count + 1):
            is_last = segment_index == args.reconnect_count
            segment_started_at = time.monotonic()
            clock_source_offset = source_offset
            if args.mode == "live" and live_origin is not None:
                clock_source_offset = max(0.0, segment_started_at - live_origin)
                source_offset = max(source_offset, clock_source_offset + startup_estimate)
            if source_offset >= source_duration:
                raise RuntimeError("计入断流和重连开销后，源视频已耗尽；请减少中断次数或每段推流时长。")
            prefix = output_directory / f"segment_{segment_index + 1:02d}"
            print(f"第 {segment_index + 1}/{args.reconnect_count + 1} 次连接，源视频起点 {source_offset:.2f} 秒", flush=True)
            sent_seconds = publish_segment(ffmpeg, args, source_offset, is_last, prefix)
            segment_finished_at = time.monotonic()
            wall_seconds = segment_finished_at - segment_started_at
            segment = {
                "connection": segment_index + 1,
                "source_offset_seconds": round(source_offset, 3),
                "output_progress_seconds": round(sent_seconds, 3),
                "wall_seconds": round(wall_seconds, 3),
                "clock_source_offset_seconds": round(clock_source_offset, 3),
                "estimated_startup_seconds": round(startup_estimate, 3) if args.mode == "live" else 0.0,
                "wall_minus_output_seconds": round(wall_seconds - sent_seconds, 3),
                "forced_disconnect": not is_last,
            }
            report["segments"].append(segment)
            if args.mode == "live":
                if live_origin is None:
                    live_origin = segment_finished_at - sent_seconds
                startup_estimate = max(0.0, wall_seconds - sent_seconds)
            source_offset += sent_seconds
            if is_last and (args.final_seconds is None or args.final_seconds >= source_duration - segment["source_offset_seconds"]):
                source_offset = source_duration
                report["source_end_reached"] = True
            report["source_consumed_seconds"] = round(min(source_duration, source_offset), 3)
            report["unpublished_source_seconds"] = round(max(0.0, source_duration - source_offset), 3)
            if not is_last:
                print(f"已强制中断推流，等待 {args.disconnect_seconds} 秒后重连……", flush=True)
                gap_started_at = time.monotonic()
                time.sleep(args.disconnect_seconds)
                gap_seconds = time.monotonic() - gap_started_at
                segment["disconnect_wait_seconds"] = round(gap_seconds, 3)
                report["disconnect_wait_seconds"] += gap_seconds
                if args.mode == "live":
                    source_offset += gap_seconds
        report["push_wall_seconds"] = round(time.monotonic() - push_started_at, 3)
        push_started_at = None
        print(f"等待 {args.finalize_seconds} 秒，供服务器完成录像写入……", flush=True)
        time.sleep(args.finalize_seconds)
        recordings = []
        for record_directory in record_directories:
            recordings.extend(probe_recordings(ffprobe, record_directory, started_at, output_directory, len(recordings)))
        report["recordings"] = recordings
        report["recording_status"] = (
            "not_found" if not recordings else
            "probe_failed" if any(item["status"] != "readable" for item in recordings) else "readable"
        )
        durations = [item["duration_seconds"] for item in recordings if item["status"] == "readable"]
        report["readable_recording_duration_sum_seconds"] = round(sum(durations), 3) if durations else None
        if recordings and all(item["status"] == "readable" for item in recordings):
            report["recording_minus_source_seconds"] = round(sum(durations) - source_duration, 3)
        if not recordings:
            report["recording_note"] = "未找到本地录像，无法判断录像时长；请核对 --record-directory、是否开启 MP4 录制以及云上传后是否删除了本地文件。"
        report["status"] = "publisher_test_completed"
    except KeyboardInterrupt:
        report["status"] = "interrupted"
        report["error"] = "用户中断测试。"
    except Exception as error:
        report["error"] = str(error)
    finally:
        if push_started_at is not None:
            report["push_wall_seconds"] = round(time.monotonic() - push_started_at, 3)
        report_path = output_directory / "report.json"
        report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(f"报告：{report_path}", flush=True)
    if report["error"]:
        print(f"测试未完成：{report['error']}", file=sys.stderr)
        return 130 if report["status"] == "interrupted" else 1
    print(f"推流测试完成，录像检查状态：{report['recording_status']}")
    print(f"源视频：{report['source_duration_seconds']:.3f} 秒；未覆盖的源视频尾部：{report['unpublished_source_seconds']:.3f} 秒")
    print(report["duration_comparison_note"])
    if report["readable_recording_duration_sum_seconds"] is not None:
        print(f"可读取录像的总时长：{report['readable_recording_duration_sum_seconds']:.3f} 秒")
    if report.get("recording_note"):
        print(report["recording_note"])
    return 0

if __name__ == "__main__":
    sys.exit(main())


# python tools/test_rtmp_reconnect.py --input "/home/hanli/test_video/2026-05-25-15-58-30-117.mp4" --url "rtmp://127.0.0.1:1935/live/drone_test" --push-seconds 20 --disconnect-seconds 5 --reconnect-count 2
