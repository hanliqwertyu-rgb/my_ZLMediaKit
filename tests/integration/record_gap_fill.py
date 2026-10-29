#!/usr/bin/env python3
"""MP4/TS-HLS gap-fill integration checks using an isolated local server.

Requires a MediaServer build with ENABLE_FFMPEG, ENABLE_MP4 and ENABLE_HLS,
plus FFmpeg/ffprobe with libx264 and, for the HEVC case, libx265.
All generated configuration, media and logs remain in --output-directory.
"""
import argparse
import configparser
from contextlib import contextmanager
import json
from pathlib import Path
import re
import secrets
import signal
import socket
import subprocess
import time
from urllib.parse import urlencode
from urllib.request import urlopen


ROOT = Path(__file__).resolve().parents[2]


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def wait_until(check, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = check()
        if result:
            return result
        time.sleep(0.05)
    raise TimeoutError("condition not reached within %.1fs" % timeout)


def stop_process(process):
    if process.poll() is None:
        process.terminate()
    try:
        process.wait(timeout=8)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def probe(path):
    result = subprocess.run(
        ["ffprobe", "-v", "error", "-show_entries",
         "format=duration:stream=codec_name,codec_type,sample_rate,channels",
         "-of", "json", str(path)],
        capture_output=True, text=True, check=True, timeout=20,
    )
    return json.loads(result.stdout)


def analyze(path, audio, expect_gap):
    metadata = probe(path)
    video = [item for item in metadata["streams"] if item["codec_type"] == "video"]
    sound = [item for item in metadata["streams"] if item["codec_type"] == "audio"]
    require(video and video[0]["codec_name"] == "h264", "missing H.264 video: %s" % path)
    require(bool(sound) == audio, "unexpected audio track: %s" % path)
    if audio:
        require(sound[0]["codec_name"] == "aac", "missing AAC: %s" % path)
    decoded = subprocess.run(
        ["ffmpeg", "-hide_banner", "-nostats", "-xerror", "-i", str(path),
         "-vf", "blackdetect=d=0.001:pix_th=0.10", "-an", "-f", "null", "-"],
        capture_output=True, text=True, timeout=30,
    )
    require(decoded.returncode == 0, "video decoding failed: " + decoded.stderr)
    black = [tuple(map(float, item)) for item in re.findall(
        r"black_start:([\d.]+) black_end:([\d.]+) black_duration:([\d.]+)", decoded.stderr)]
    duration = float(metadata["format"]["duration"])
    if expect_gap:
        require(len(black) == 1, "expected one black interval: %s" % black)
        start, end, length = black[0]
        require(start > 0.5 and duration - end > 0.5, "real picture did not recover: %s" % black)
        require(2.0 <= length <= 3.8, "unexpected black duration: %s" % black)
    else:
        require(not black, "unexpected black interval: %s" % black)
    silence = []
    if audio:
        decoded_audio = subprocess.run(
            ["ffmpeg", "-hide_banner", "-nostats", "-xerror", "-i", str(path),
             "-vn", "-af", "silencedetect=noise=-45dB:d=0.3", "-f", "null", "-"],
            capture_output=True, text=True, timeout=30,
        )
        require(decoded_audio.returncode == 0, "audio decoding failed: " + decoded_audio.stderr)
        silence = [float(value) for value in re.findall(r"silence_duration: ([\d.]+)", decoded_audio.stderr)]
        if expect_gap:
            require(any(value >= 2 for value in silence), "missing silent audio in gap")
    return {"duration": duration, "black_intervals": black, "silence_durations": silence,
            "streams": metadata["streams"]}


def make_fixture(directory, codec):
    path = directory / (codec + ".mp4")
    options = (["-c:v", "libx264", "-preset", "veryfast", "-bf", "2", "-g", "240",
                "-x264-params", "b-adapt=0:scenecut=0:keyint=240:min-keyint=240"]
               if codec == "bframes" else
               ["-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency", "-g", "25"]
               if codec == "h264" else
               ["-c:v", "libx265", "-preset", "ultrafast", "-x265-params",
                "pools=1:frame-threads=1:bframes=0:keyint=25:min-keyint=25:scenecut=0:log-level=error"])
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin",
         "-f", "lavfi", "-i", "color=white:size=320x240:rate=25",
         "-f", "lavfi", "-i", "sine=frequency=1000:sample_rate=48000",
         "-t", "12", *options, "-pix_fmt", "yuv420p", "-c:a", "aac", "-ac", "2", str(path)],
        check=True, capture_output=True, timeout=45,
    )
    return path


class Server:
    def __init__(self, binary, directory, automatic=True, enabled=True, audio=True, mute_audio=False,
                 threads=2, affinity=True, bitrate=2000000):
        self.binary, self.directory = binary, directory
        self.directory.mkdir()
        self.records = self.directory / "records"
        cfg = configparser.ConfigParser(interpolation=None, strict=False)
        cfg.optionxform = str
        cfg.read(ROOT / "conf/config.ini", encoding="utf-8-sig")
        self.secret = secrets.token_hex(16)
        with socket.socket() as rtmp, socket.socket() as http, socket.socket() as rtsp:
            for listener in (rtmp, http, rtsp):
                listener.bind(("127.0.0.1", 0))
            self.rtmp, self.http, self.rtsp = [item.getsockname()[1] for item in (rtmp, http, rtsp)]
        overrides = {
            "general": {"listen_ip": "127.0.0.1", "streamNoneReaderDelayMS": "60000"},
            "api": {"secret": self.secret},
            "http": {"port": str(self.http), "sslport": "0", "rootPath": str(self.directory)},
            "rtmp": {"port": str(self.rtmp), "sslport": "0"},
            "rtsp": {"port": str(self.rtsp), "sslport": "0"},
            "rtp_proxy": {"port": "0"}, "srt": {"port": "0"}, "shell": {"port": "0"},
            "rtc": {"port": "0", "tcpPort": "0"},
            "hook": {"enable": "0"},
            "cloud_upload": {"enable": "0", "minio_enable": "0", "url": "",
                             "minio_access_key": "", "minio_secret_key": ""},
            "record_gap_fill": {"enable": str(int(enabled)), "detect_ms": "200", "max_ms": "5000",
                                "fps": "25", "bitrate": str(bitrate)},
            "protocol": {"enable_mp4": str(int(automatic)), "enable_hls": str(int(automatic)),
                         "enable_hls_fmp4": "0", "enable_audio": str(int(audio)), "add_mute_audio": str(int(mute_audio)),
                         "hls_demand": "0", "enable_rtmp": "1", "enable_rtsp": "1", "enable_ts": "0",
                         "enable_fmp4": "0", "auto_close": "0", "continue_push_ms": "5000",
                         "mp4_max_second": "3600", "mp4_save_path": str(self.records),
                         "hls_save_path": str(self.records)},
            "hls": {"segDur": "0.5", "segNum": "0", "segKeep": "1", "segDelay": "0",
                    "broadcastRecordTs": "0"},
        }
        for section, values in overrides.items():
            if section not in cfg:
                cfg.add_section(section)
            cfg[section].update(values)
        config_path = directory / "config.ini"
        with config_path.open("w") as output:
            cfg.write(output)
        self.log = (directory / "server.log").open("w+")
        self.process = subprocess.Popen(
            [str(binary), "-c", str(config_path), "-t", str(threads), "--affinity", str(int(affinity)),
             "--log-dir", str(directory / "logs")],
            cwd=directory, stdout=self.log, stderr=subprocess.STDOUT,
        )

    def api(self, name, **parameters):
        query = urlencode({"secret": self.secret, **parameters})
        with urlopen("http://127.0.0.1:%d/index/api/%s?%s" % (self.http, name, query), timeout=2) as response:
            return json.load(response)

    def ready(self):
        def check():
            if self.process.poll() is not None:
                raise RuntimeError("MediaServer exited at startup; see " + str(self.directory / "server.log"))
            try:
                return self.api("getMediaList").get("code") == 0
            except OSError:
                return False
        wait_until(check)

    def media_ready(self, name):
        return any(item["stream"] == name for item in self.api("getMediaList").get("data", []))

    def record(self, operation, name, kind):
        result = self.api(operation, type=kind, vhost="__defaultVhost__", app="live", stream=name)
        require(result.get("code") == 0, "record API failed: %s" % result)
        return result

    def close(self):
        stop_process(self.process)
        self.log.close()


@contextmanager
def publisher(server, source, name, index, seconds, audio=True, transport="rtmp"):
    prefix = server.directory / (name + "-%d" % index)
    progress = prefix.with_suffix(".progress")
    log = prefix.with_suffix(".stderr").open("w")
    destination = "%s://127.0.0.1:%d/live/%s" % (
        transport, server.rtmp if transport == "rtmp" else server.rtsp, name)
    command = ["ffmpeg", "-hide_banner", "-loglevel", "warning", "-nostdin", "-nostats",
               "-stats_period", "0.1", "-progress", str(progress), "-re", "-i", str(source),
               "-t", str(seconds), "-map", "0:v:0"]
    command += ["-map", "0:a:0?"] if audio else ["-an"]
    command += ["-c", "copy"]
    command += ["-f", "flv"] if transport == "rtmp" else ["-f", "rtsp", "-rtsp_transport", "tcp"]
    process = subprocess.Popen(command + [destination], stdout=subprocess.DEVNULL, stderr=log)

    def sent():
        if not progress.exists():
            return 0
        values = re.findall(r"^out_time_us=(\d+)$", progress.read_text(), re.MULTILINE)
        return int(values[-1]) / 1_000_000 if values else 0

    def reach(seconds):
        def check():
            if process.poll() is not None:
                raise RuntimeError("publisher exited early; see " + str(prefix.with_suffix(".stderr")))
            return sent() >= seconds
        wait_until(check, seconds + 15)

    try:
        yield process, reach
    finally:
        stop_process(process)
        log.close()


def playlist_text(path):
    try:
        return path.read_text()
    except FileNotFoundError:
        return ""


def completed_recordings(server, name, minimum_mp4=1):
    playlist = server.records / "live" / name / "hls.m3u8"
    def check():
        files = sorted((server.records / "record" / "live" / name).rglob("*.mp4"))
        files = [path for path in files if not path.name.startswith(".")]
        return files if len(files) >= minimum_mp4 and "#EXT-X-ENDLIST" in playlist_text(playlist) else False
    return wait_until(check), playlist


def run_case(binary, directory, source, *, api=False, enabled=True, audio=True, transport="rtmp", server_options=None):
    server = Server(binary, directory, automatic=not api, enabled=enabled, audio=audio, **(server_options or {}))
    name = "fixture"
    try:
        server.ready()
        playlist = server.records / "live" / name / "hls.m3u8"
        with publisher(server, source, name, 1, 10, audio, transport) as (first, reach):
            wait_until(lambda: server.media_ready(name))
            if api:
                server.record("startRecord", name, 0)
                server.record("startRecord", name, 1)
            reach(3)
            first.kill()
            first.wait(timeout=5)
        before = playlist_text(playlist)
        time.sleep(2.4)
        after = playlist_text(playlist)
        updated_in_gap = after.count("#EXTINF:") > before.count("#EXTINF:")
        if enabled:
            require(updated_in_gap, "HLS playlist did not advance during disconnect")
        with publisher(server, source, name, 2, 6 if api else 3, audio, transport) as (second, reach):
            if api:
                reach(2)
                server.record("stopRecord", name, 0)
                server.record("stopRecord", name, 1)
                require(server.media_ready(name), "stopping recording also stopped source")
                mp4, playlist = completed_recordings(server, name)
                first_run = {"mp4": analyze(mp4[-1], audio, enabled),
                             "hls": analyze(playlist, audio, enabled)}
                # Restart only after the previous output has finalized.
                time.sleep(0.15)
                server.record("startRecord", name, 0)
                server.record("startRecord", name, 1)
                reach(4.5)
                server.record("stopRecord", name, 0)
                server.record("stopRecord", name, 1)
                restarted, playlist = completed_recordings(server, name, minimum_mp4=2)
                restart_run = {"mp4": analyze(restarted[-1], audio, False),
                               "hls": analyze(playlist, audio, False)}
            else:
                require(second.wait(timeout=15) == 0, "publisher failed")
        if not api:
            mp4, playlist = completed_recordings(server, name)
            first_run = {"mp4": analyze(mp4[-1], audio, enabled),
                         "hls": analyze(playlist, audio, enabled)}
            restart_run = None
        require(abs(first_run["mp4"]["duration"] - first_run["hls"]["duration"]) < 0.25,
                "MP4 and HLS durations diverged: %s" % first_run)
        server.log.flush()
        errors = re.findall(r".*(?:recording stopped|TS output failed|queue full|TS input rejected).*",
                            (server.directory / "server.log").read_text())
        require(not errors, "server recording errors: %s" % errors)
        return {"passed": True, "hls_advanced_in_gap": updated_in_gap,
                "first_run": first_run, "restart_run": restart_run}
    finally:
        server.close()


def run_continuous(binary, directory, source, *, jitter=False, audio=True, seconds=10, mute_audio=False,
                   server_options=None):
    server = Server(binary, directory, audio=audio, mute_audio=mute_audio, **(server_options or {}))
    try:
        server.ready()
        with publisher(server, source, "continuous", 1, seconds, audio) as (process, reach):
            if jitter:
                reach(3)
                process.send_signal(signal.SIGSTOP)
                try:
                    # Same connection, no packet loss or timestamp reset.
                    # Longer than detect_ms; decoding must retain references.
                    time.sleep(0.3)
                finally:
                    process.send_signal(signal.SIGCONT)
            require(process.wait(timeout=seconds + 20) == 0, "continuous publisher failed")
        mp4, playlist = completed_recordings(server, "continuous")
        result = {"mp4": analyze(mp4[-1], audio, False),
                  "hls": analyze(playlist, audio, False)}
        require(abs(result["mp4"]["duration"] - seconds) < 1,
                "continuous recording truncated: %s" % result)
        errors = re.findall(r".*(?:recording stopped|TS output failed|queue full|TS input rejected).*",
                            (server.directory / "server.log").read_text())
        require(not errors, "server recording errors: %s" % errors)
        return {"passed": True, "jitter": jitter, **result}
    finally:
        server.close()


def run_preroll(binary, directory, source, *, audio=False, server_options=None):
    """Start API recording after a long GOP has accumulated in the live cache."""
    server = Server(binary, directory, automatic=False, audio=audio, **(server_options or {}))
    try:
        server.ready()
        with publisher(server, source, "preroll", 1, 11, audio=audio) as (_, reach):
            reach(7)  # More than 120 decoded frames before the next 9.6 s IDR.
            for kind in (0, 1):
                server.record("startRecord", "preroll", kind)
            reach(9)
            for kind in (0, 1):
                server.record("stopRecord", "preroll", kind)
            mp4, playlist = completed_recordings(server, "preroll")
            result = {"mp4": analyze(mp4[-1], audio, False),
                      "hls": analyze(playlist, audio, False)}
            for output in result.values():
                require(1.5 < output["duration"] < 3.0,
                        "cached GOP replay changed recording duration: %s" % result)
        errors = re.findall(r".*(?:recording stopped|TS output failed|queue full|TS input rejected).*",
                            (server.directory / "server.log").read_text())
        require(not errors, "server recording errors: %s" % errors)
        return {"passed": True, **result}
    finally:
        server.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--output-directory", type=Path, required=True)
    parser.add_argument("--case", choices=("auto", "api", "video-only", "disabled", "hevc",
                                          "bframes", "jitter", "preroll", "preroll-av", "source", "all"), default="all")
    parser.add_argument("--input", type=Path, help="Optional real source for --case source")
    parser.add_argument("--seconds", type=int, default=10, help="Continuous source test duration")
    parser.add_argument("--mute-audio", action="store_true", help="Generate a server-side mute track for a silent source")
    parser.add_argument("--threads", type=int, default=2, help="Server event poller count")
    parser.add_argument("--affinity", type=int, choices=(0, 1), default=1,
                        help="Poller CPU affinity; defaults to the production setting")
    parser.add_argument("--bitrate", type=int, default=2000000, help="Gap-fill output video bitrate")
    args = parser.parse_args()
    if args.seconds <= 0:
        parser.error("--seconds must be positive")
    if args.threads <= 0:
        parser.error("--threads must be positive")
    if args.case == "source" and not args.input:
        parser.error("--case source requires --input")
    directory = args.output_directory.resolve()
    directory.mkdir(parents=True, exist_ok=False)
    cases = {"auto": {}, "api": {"api": True}, "video-only": {"audio": False},
             "disabled": {"enabled": False}, "hevc": {"transport": "rtsp"}}
    results = {}
    server_options = {"threads": args.threads, "affinity": bool(args.affinity), "bitrate": args.bitrate}
    try:
        h264 = make_fixture(directory, "h264")
        hevc = make_fixture(directory, "hevc") if args.case in ("hevc", "all") else None
        if args.case in ("bframes", "jitter", "preroll", "preroll-av", "all"):
            bframes = make_fixture(directory, "bframes")
        for name in ("bframes", "jitter", "source"):
            if args.case not in (name, "all") or (name == "source" and not args.input):
                continue
            source = args.input.resolve(strict=True) if name == "source" else bframes
            print("Running " + name, flush=True)
            try:
                results[name] = run_continuous(
                    args.server.resolve(), directory / name, source,
                    jitter=name == "jitter", audio=name != "source" or args.mute_audio,
                    mute_audio=args.mute_audio, seconds=args.seconds, server_options=server_options)
            except Exception as error:
                results[name] = {"passed": False, "error": str(error)}
                raise
            print("PASS " + name, flush=True)
        for name in ("preroll", "preroll-av"):
            if args.case not in (name, "all"):
                continue
            print("Running " + name, flush=True)
            try:
                results[name] = run_preroll(
                    args.server.resolve(), directory / name, bframes,
                    audio=name == "preroll-av", server_options=server_options)
            except Exception as error:
                results[name] = {"passed": False, "error": str(error)}
                raise
            print("PASS " + name, flush=True)
        for name, options in cases.items():
            if args.case not in (name, "all"):
                continue
            print("Running " + name, flush=True)
            try:
                results[name] = run_case(args.server.resolve(), directory / name,
                                         hevc if name == "hevc" else h264, server_options=server_options, **options)
            except Exception as error:
                results[name] = {"passed": False, "error": str(error)}
                raise
            print("PASS " + name, flush=True)
    finally:
        (directory / "report.json").write_text(json.dumps(results, indent=2) + "\n")


if __name__ == "__main__":
    main()
