#!/usr/bin/env python3
"""Create external deployment directories and a unique API secret; never overwrite."""

import argparse
import configparser
import os
from pathlib import Path
import secrets
import sys
from uuid import uuid4


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, default=Path("/opt/zlm"))
    parser.add_argument("--uid", type=int, default=10001)
    parser.add_argument("--gid", type=int, default=10001)
    parser.add_argument("--image", default="my-zlm:2026-10-29")
    parser.add_argument("--http-port", type=int, default=8080)
    parser.add_argument("--rtmp-port", type=int, default=1935)
    parser.add_argument("--rtsp-port", type=int, default=8554)
    args = parser.parse_args()
    if args.uid <= 0 or args.gid <= 0:
        parser.error("UID/GID must identify a non-root account")
    ports = (args.http_port, args.rtmp_port, args.rtsp_port)
    if len(set(ports)) != 3 or any(not 1024 <= port <= 65535 for port in ports):
        parser.error("use three different ports within 1024..65535")
    base = args.directory.resolve()
    if any(char in str(base) + args.image for char in ("\n", "\r", "'")):
        parser.error("directory/image must not contain newlines or single quotes")
    config_path = base / "config/config.ini"
    env_path = base / ".env"
    if config_path.exists() or env_path.exists():
        parser.error("configuration or .env already exists; edit it directly rather than reinitializing")
    if os.geteuid() != 0 and (args.uid != os.geteuid() or args.gid != os.getegid()):
        parser.error("run with sudo, or use your own --uid and --gid")

    cfg = configparser.ConfigParser(interpolation=None)
    cfg.optionxform = str
    with Path(__file__).with_name("config.ini.example").open(encoding="utf-8") as source:
        cfg.read_file(source)
    cfg["api"]["secret"] = secrets.token_urlsafe(32)
    cfg["general"]["mediaServerId"] = "zlm-" + uuid4().hex[:12]
    for section, port in zip(("http", "rtmp", "rtsp"), ports):
        cfg[section]["port"] = str(port)

    if not base.exists():
        base.mkdir(parents=True, mode=0o755)
        base.chmod(0o755)
    # Only assign ownership of directories created by this invocation.
    for name in ("config", "config/certs", "data", "data/www", "log"):
        path = base / name
        if not path.exists():
            path.mkdir(mode=0o750)
            if os.geteuid() == 0:
                os.chown(path, args.uid, args.gid)
    fd = os.open(config_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o640)
    with os.fdopen(fd, "w", encoding="utf-8") as output:
        cfg.write(output)
    if os.geteuid() == 0:
        os.chown(config_path, args.uid, args.gid)

    # .env contains only paths/image/UID; credentials stay in the private INI.
    values = {
        "ZLM_IMAGE": args.image,
        "ZLM_UID": str(args.uid),
        "ZLM_GID": str(args.gid),
        "ZLM_CONFIG_DIR": str(config_path.parent),
        "ZLM_DATA_DIR": str(base / "data"),
        "ZLM_LOG_DIR": str(base / "log"),
        "ZLM_BUILD_JOBS": "4",
        "ZLM_SOURCE_REVISION": "workspace",
    }
    fd = os.open(env_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
    with os.fdopen(fd, "w", encoding="utf-8") as output:
        for key, value in values.items():
            output.write("%s='%s'\n" % (key, value))
    print("Created config: " + str(config_path))
    print("Created environment: " + str(env_path))
    print("API secret generated; its value is not printed. Cloud upload is initially disabled.")
    print("Ports: HTTP=%d, RTMP=%d, RTSP=%d" % ports)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except OSError as error:
        print("Deployment initialization failed: " + str(error), file=sys.stderr)
        sys.exit(1)
