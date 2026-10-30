#!/usr/bin/env python3
"""Isolated MediaServer + disposable MinIO checks for complete TS HLS archives.

Requires Docker, FFmpeg/ffprobe and an image containing the MinIO server.
Never uses existing storage credentials or modifies an existing bucket/service.
"""
import argparse
import configparser
from datetime import datetime, timezone
import hashlib
import hmac
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import importlib.util
import json
from pathlib import Path
import secrets
import socket
import subprocess
import sys
import threading
import time
from urllib.error import HTTPError
from urllib.parse import quote, unquote, urlencode, urlsplit
from urllib.request import Request, urlopen


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("gap_checks", Path(__file__).with_name("record_gap_fill.py"))
gap = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gap)


def free_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


class S3:
    def __init__(self, endpoint, access, secret):
        self.endpoint, self.access, self.secret = endpoint, access, secret

    def request(self, method, path, content=b"", query=""):
        now = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        day = now[:8]
        uri = quote(path, safe="/-_.~")
        payload = hashlib.sha256(content).hexdigest()
        host = urlsplit(self.endpoint).netloc
        names = "host;x-amz-content-sha256;x-amz-date"
        headers = "host:%s\nx-amz-content-sha256:%s\nx-amz-date:%s\n" % (host, payload, now)
        canonical = "%s\n%s\n%s\n%s\n%s\n%s" % (method, uri, query, headers, names, payload)
        scope = day + "/us-east-1/s3/aws4_request"
        message = "AWS4-HMAC-SHA256\n%s\n%s\n%s" % (now, scope, hashlib.sha256(canonical.encode()).hexdigest())
        key = ("AWS4" + self.secret).encode()
        for value in (day, "us-east-1", "s3", "aws4_request"):
            key = hmac.new(key, value.encode(), hashlib.sha256).digest()
        signature = hmac.new(key, message.encode(), hashlib.sha256).hexdigest()
        auth = "AWS4-HMAC-SHA256 Credential=%s/%s, SignedHeaders=%s, Signature=%s" % (self.access, scope, names, signature)
        request = Request(self.endpoint + uri + ("?" + query if query else ""),
                          data=content if method in ("PUT", "POST") else None, method=method,
                          headers={"Authorization": auth, "x-amz-date": now, "x-amz-content-sha256": payload})
        with urlopen(request, timeout=10) as response:
            return response.read(), dict(response.headers)


class Proxy:
    """Pass signed requests through unchanged and inject recoverable failures."""
    def __init__(self, endpoint):
        self.lock = threading.Lock()
        self.events, self.success = [], {}
        self.blocked = False
        self.fail_once = True
        self.order_errors = []
        self.diagnostics = []
        self.endpoint = endpoint
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, fmt, *values):
                with owner.lock:
                    owner.diagnostics.append(fmt % values)

            def do_PUT(self):
                body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
                key = unquote(urlsplit(self.path).path)
                with owner.lock:
                    fail = owner.blocked or (owner.fail_once and key.endswith(".ts"))
                    if key.endswith(".ts"):
                        owner.fail_once = False
                    owner.events.append({"key": key, "failed": fail, "type": self.headers.get("Content-Type")})
                    if not fail and key.endswith("hls.m3u8"):
                        prefix = key.rsplit("/", 1)[0] + "/"
                        for line in body.decode().splitlines():
                            if line and not line.startswith("#") and prefix + line not in owner.success:
                                owner.order_errors.append(prefix + line)
                if fail:
                    error = b"injected archive upload failure"
                    self.send_response(503)
                    self.send_header("Content-Length", str(len(error)))
                    self.end_headers()
                    self.wfile.write(error)
                    return
                headers = {name: value for name, value in self.headers.items()
                           if name.lower() not in ("connection", "accept-encoding", "content-length")}
                request = Request(owner.endpoint + self.path, data=body, method="PUT", headers=headers)
                try:
                    with urlopen(request, timeout=10) as response:
                        result, status = response.read(), response.status
                except HTTPError as error:
                    result, status = error.read(), error.code
                if 200 <= status < 300:
                    with owner.lock:
                        owner.success[key] = body
                self.send_response(status)
                self.send_header("Content-Length", str(len(result)))
                self.end_headers()
                self.wfile.write(result)

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.url = "http://127.0.0.1:%d" % self.server.server_port

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)


def task_states(spool):
    return [json.loads(path.read_text()) for path in spool.glob("*.json")]


def make_server(binary, directory, proxy, bucket, access, secret):
    original = gap.subprocess.Popen

    def launch(command, *args, **kwargs):
        if Path(command[0]).name == "MediaServer":
            path = Path(command[command.index("-c") + 1])
            cfg = configparser.ConfigParser(interpolation=None, strict=False)
            cfg.optionxform = str
            cfg.read(path)
            cfg["cloud_upload"].update({
                "enable": "1", "minio_enable": "1", "minio_endpoint": proxy.url,
                "minio_bucket": bucket, "minio_access_key": access, "minio_secret_key": secret,
                "minio_object": "mp4/{app}/{stream}/{file_name}", "delete_after_upload": "0", "timeout_sec": "3",
            })
            cfg["hls_archive"] = {
                "enable": "1", "scan_root": str(directory / "archive"), "spool_path": str(directory / "jobs"),
                "object_prefix": "hls", "scan_interval_sec": "0.1", "retry_delay_sec": "0.1", "retry_max": "100",
                "delete_local_after_archive": "0",
            }
            # Legacy TS broadcasting must not duplicate/delete archive-owned files.
            cfg["hls"]["broadcastRecordTs"] = "1"
            with path.open("w") as output:
                cfg.write(output)
        return original(command, *args, **kwargs)

    gap.subprocess.Popen = launch
    try:
        return gap.Server(binary, directory, automatic=False)
    finally:
        gap.subprocess.Popen = original


def restart_server(server):
    gap.stop_process(server.process)
    server.log.close()
    server.log = (server.directory / "server.log").open("a")
    server.process = subprocess.Popen([
        str(server.binary), "-c", str(server.directory / "config.ini"), "-t", "2", "--affinity", "1",
        "--log-dir", str(server.directory / "logs")], cwd=server.directory,
        stdout=server.log, stderr=subprocess.STDOUT)
    server.ready()


def verify_archive(job, server, storage, bucket, endpoint):
    playlist = job["playlist"]
    key = job["playlist_object"]
    raw, headers = storage.request("GET", "/" + bucket + "/" + key)
    gap.require(raw == playlist.encode(), "remote manifest differs from original snapshot")
    gap.require(headers.get("Content-Type", "").startswith("application/vnd.apple.mpegurl"), "playlist MIME type missing")
    prefix = key.rsplit("/", 1)[0] + "/"
    local_root = Path(job["scan_root"]) / Path(job["playlist_path"]).parent
    checksums = []
    for segment in job["segments"]:
        content, headers = storage.request("GET", "/" + bucket + "/" + prefix + segment["path"])
        local = (local_root / segment["path"]).read_bytes()
        gap.require(content == local, "TS content changed during archive")
        gap.require(headers.get("Content-Type", "").startswith("video/mp2t"), "TS MIME type missing")
        checksums.append(hashlib.sha256(content).hexdigest())
    remote_url = endpoint + quote("/" + bucket + "/" + key, safe="/-_.~")
    metadata = gap.probe(remote_url)
    decoded = subprocess.run([
        "ffmpeg", "-hide_banner", "-nostats", "-xerror", "-i", remote_url,
        "-vf", "blackdetect=d=0.001:pix_th=0.10", "-an", "-f", "null", "-"],
        capture_output=True, text=True, timeout=20)
    gap.require(decoded.returncode == 0 and "black_start:" not in decoded.stderr, "remote HLS decode/black-frame check failed")
    return {"object": key, "segments": len(checksums), "duration": metadata["format"]["duration"],
            "sha256": checksums, "remote_decode_ok": True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--output-directory", type=Path, required=True)
    parser.add_argument("--minio-image", default="minio/minio:latest")
    args = parser.parse_args()
    output = args.output_directory.resolve()
    output.mkdir(parents=True, exist_ok=False)
    access, secret = "archive-test-" + secrets.token_hex(6), secrets.token_urlsafe(24)
    bucket = "hls-archive-test-" + secrets.token_hex(5)
    name = "zlm-hls-archive-test-" + secrets.token_hex(5)
    port = free_port()
    endpoint = "http://127.0.0.1:%d" % port
    server, proxy, publisher = None, None, None
    report = {}
    publisher_log = (output / "publisher.log").open("w")
    try:
        subprocess.run([
            "docker", "run", "--rm", "-d", "--name", name, "-p", "127.0.0.1:%d:9000" % port,
            "-e", "MINIO_ROOT_USER=" + access, "-e", "MINIO_ROOT_PASSWORD=" + secret,
            args.minio_image, "server", "/data", "--address", ":9000", "--console-address", ":9001"],
            check=True, capture_output=True, text=True, timeout=30)

        def healthy():
            try:
                with urlopen(endpoint + "/minio/health/live", timeout=1) as response:
                    return response.status == 200
            except OSError:
                return False
        gap.wait_until(healthy, 20)
        storage = S3(endpoint, access, secret)
        storage.request("PUT", "/" + bucket)
        # Only the disposable bucket is readable anonymously, for real player GETs.
        policy = {"Version": "2012-10-17", "Statement": [{"Effect": "Allow", "Principal": {"AWS": ["*"]},
                  "Action": ["s3:GetObject"], "Resource": ["arn:aws:s3:::" + bucket + "/*"]}]}
        storage.request("PUT", "/" + bucket, json.dumps(policy).encode(), "policy=")
        proxy = Proxy(endpoint)
        server = make_server(args.server.resolve(), output / "server", proxy, bucket, access, secret)
        server.ready()
        fixture = gap.make_fixture(output, "h264")
        publisher = subprocess.Popen([
            "ffmpeg", "-hide_banner", "-loglevel", "warning", "-nostdin", "-stream_loop", "-1", "-re", "-i", str(fixture),
            "-c", "copy", "-f", "flv", "rtmp://127.0.0.1:%d/live/test" % server.rtmp],
            stdout=subprocess.DEVNULL, stderr=publisher_log)
        gap.wait_until(lambda: server.media_ready("test"))
        command = [sys.executable, "-B", str(ROOT / "tools/test_record_api.py"), "test",
                   "--server", "http://127.0.0.1:%d" % server.http, "--config", str(server.directory / "config.ini"),
                   "--type", "hls", "--stream", "test", "--count", "3", "--seconds", "2", "--interval", "0.3",
                   "--record-root", str(server.directory / "archive")]
        tested = subprocess.run(command, capture_output=True, text=True, timeout=30)
        (output / "repeat.stdout.log").write_text(tested.stdout)
        (output / "repeat.stderr.log").write_text(tested.stderr)
        gap.require(tested.returncode == 0, "recording test script failed: " + tested.stderr)
        spool = server.directory / "jobs"
        gap.wait_until(lambda: len(task_states(spool)) == 3 and all(j["status"] == "ready" for j in task_states(spool)), 20)
        first = task_states(spool)
        checked = [verify_archive(job, server, storage, bucket, endpoint) for job in first]
        print("PASS: three independent HLS recordings, MinIO byte identity, MIME types and remote playback", flush=True)
        gap.require(any(e["failed"] for e in proxy.events), "failure retry was not exercised")
        gap.require(not proxy.order_errors, "playlist was published before its TS segments")

        # In-flight failures must survive process restart, including special URL characters.
        with proxy.lock:
            proxy.blocked = True
        custom = server.directory / "archive" / "restart + 中文"
        started = server.api("startRecord", type=0, vhost="__defaultVhost__", app="live", stream="test", customized_path=str(custom))
        gap.require(started.get("code") == 0, "restart fixture start failed")
        time.sleep(2)
        server.record("stopRecord", "test", 0)
        gap.wait_until(lambda: len(task_states(spool)) == 4 and any(j["status"] == "retry_wait" for j in task_states(spool)))
        gap.stop_process(publisher)
        publisher = None
        restart_server(server)
        with proxy.lock:
            proxy.blocked = False
        gap.wait_until(lambda: all(j["status"] == "ready" for j in task_states(spool)), 20)
        final = task_states(spool)
        recovered = next(j for j in final if "restart + 中文" in j["playlist_path"])
        checked.append(verify_archive(recovered, server, storage, bucket, endpoint))
        with proxy.lock:
            already_uploaded = len(proxy.events)
        time.sleep(0.4)
        with proxy.lock:
            gap.require(len(proxy.events) == already_uploaded, "ready archive uploaded repeatedly")
        print("PASS: interrupted upload recovery after restart and encoded object paths", flush=True)

        # Existing MP4 event uploads continue to use the configured old object template.
        publisher = subprocess.Popen([
            "ffmpeg", "-hide_banner", "-loglevel", "warning", "-nostdin", "-stream_loop", "-1", "-re", "-i", str(fixture),
            "-c", "copy", "-f", "flv", "rtmp://127.0.0.1:%d/live/mp4_test" % server.rtmp],
            stdout=subprocess.DEVNULL, stderr=publisher_log)
        gap.wait_until(lambda: server.media_ready("mp4_test"))
        server.record("startRecord", "mp4_test", 1)
        time.sleep(2)
        server.record("stopRecord", "mp4_test", 1)

        def mp4_uploaded():
            with proxy.lock:
                return next((key for key in proxy.success if key.endswith(".mp4")), False)
        key = gap.wait_until(mp4_uploaded)
        remote, _ = storage.request("GET", key)
        local = next(p for p in server.records.rglob("*.mp4") if not p.name.startswith("."))
        gap.require(remote == local.read_bytes(), "legacy MP4 upload changed bytes")
        with proxy.lock:
            gap.require(all("/hls/" in e["key"] or "/mp4/" in e["key"] for e in proxy.events), "legacy TS uploader duplicated archive-owned segments")
            gap.require(not proxy.order_errors, "publication order violated")
            events = list(proxy.events)
        report = {"passed": True, "archives": checked, "retry_exercised": True, "restart_recovered": True,
                  "playlist_last": True, "legacy_mp4_ok": True, "legacy_ts_duplicate": False, "upload_events": events}
        print("PASS: existing MP4 uploads and archive-owned TS deduplication", flush=True)
    except Exception as error:
        report = {"passed": False, "error": str(error)}
        raise
    finally:
        if publisher:
            gap.stop_process(publisher)
        publisher_log.close()
        if server:
            server.close()
        if proxy:
            (output / "proxy.json").write_text(json.dumps({"events": proxy.events, "diagnostics": proxy.diagnostics,
                                                          "order_errors": proxy.order_errors}, ensure_ascii=False, indent=2))
            proxy.close()
        subprocess.run(["docker", "rm", "-f", name], capture_output=True, timeout=20)
        (output / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")


if __name__ == "__main__":
    main()
