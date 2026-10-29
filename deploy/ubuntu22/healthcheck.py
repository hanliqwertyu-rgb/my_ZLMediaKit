#!/usr/bin/env python3
"""Read-only API health check. Does not check the state of individual recorders."""

import argparse
import configparser
import json
import sys
from urllib.parse import urlencode
from urllib.request import ProxyHandler, build_opener


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", default="/etc/zlm/config.ini")
    args = parser.parse_args()
    try:
        cfg = configparser.ConfigParser(interpolation=None, strict=False)
        with open(args.config, encoding="utf-8-sig") as source:
            cfg.read_file(source)
        port = cfg.getint("http", "port")
        secret = cfg.get("api", "secret")
        if not secret or secret == "GENERATE_WITH_INIT_CONFIG" or port <= 0:
            raise ValueError("uninitialized configuration")
        url = "http://127.0.0.1:%d/index/api/getThreadsLoad?%s" % (
            port, urlencode({"secret": secret}))
        with build_opener(ProxyHandler({})).open(url, timeout=3) as response:
            body = json.load(response)
        if not isinstance(body, dict) or body.get("code") != 0:
            raise ValueError("API rejected health check")
    except Exception:
        # URL exceptions can contain query credentials: keep the output generic.
        print("MediaServer API health check failed", file=sys.stderr)
        return 1
    print("MediaServer API OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
