#!/usr/bin/env python3
"""通过 HTTP GET 测试 ZLMediaKit 按需录制接口，仅使用 Python 标准库。"""

import argparse
import configparser
import json
import math
from pathlib import Path, PurePosixPath
import sys
import time
from uuid import uuid4
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode, urlsplit
from urllib.request import Request, urlopen


ROOT = Path(__file__).resolve().parents[1]
ENDPOINTS = {"start": "startRecord", "stop": "stopRecord", "status": "isRecording"}
RECORD_TYPES = {"hls": 0, "0": 0, "mp4": 1, "1": 1}


class ApiError(RuntimeError):
    pass


def positive_seconds(value):
    try:
        number = float(value)
    except ValueError:
        raise argparse.ArgumentTypeError("请输入大于 0 的秒数") from None
    if not math.isfinite(number) or number <= 0:
        raise argparse.ArgumentTypeError("请输入大于 0 的有限秒数")
    return number


def nonnegative_seconds(value):
    try:
        number = float(value)
    except ValueError:
        raise argparse.ArgumentTypeError("请输入大于或等于 0 的秒数") from None
    if not math.isfinite(number) or number < 0:
        raise argparse.ArgumentTypeError("请输入大于或等于 0 的有限秒数")
    return number


def positive_integer(value):
    try:
        number = int(value)
    except ValueError:
        raise argparse.ArgumentTypeError("请输入大于 0 的整数") from None
    if number <= 0:
        raise argparse.ArgumentTypeError("请输入大于 0 的整数")
    return number


def parse_arguments(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__,
        epilog="需先推流；type=1/mp4 表示 MP4，type=0/hls 表示普通 TS HLS。",
    )
    parser.add_argument("action", choices=("start", "stop", "status", "test"),
                        help="开始、停止、查询，或按指定次数与间隔重复录制")
    parser.add_argument("--server", default="http://127.0.0.1", help="服务根地址，可包含端口")
    parser.add_argument("--secret", help="API 密钥；省略时读取 --config 的 [api] secret")
    parser.add_argument("--config", type=Path, default=ROOT / "release/linux/Debug/config.ini",
                        help="运行服务的 INI 配置，默认 release/linux/Debug/config.ini")
    parser.add_argument("--type", choices=tuple(RECORD_TYPES), default="mp4", help="默认 mp4")
    parser.add_argument("--vhost", default="__defaultVhost__")
    parser.add_argument("--app", default="live")
    parser.add_argument("--stream", default="test")
    parser.add_argument("--record-root", help="服务器/容器内的保存根目录；每次开始录制自动创建独立任务子目录")
    parser.add_argument("--count", type=positive_integer, default=1,
                        help="test 模式录制次数，默认 1")
    parser.add_argument("--seconds", type=positive_seconds, default=10,
                        help="test 模式每轮录制秒数，默认 10")
    parser.add_argument("--interval", type=nonnegative_seconds, default=5,
                        help="test 模式两轮录制之间等待的秒数，默认 5，可设为 0")
    parser.add_argument("--timeout", type=positive_seconds, default=10, help="单次请求超时秒数")
    args = parser.parse_args(argv)
    try:
        parsed = urlsplit(args.server)
        valid = parsed.scheme in ("http", "https") and parsed.hostname and parsed.port != 0
    except ValueError:
        valid = False
    if not valid or parsed.query or parsed.fragment or parsed.username or "\\" in args.server:
        parser.error("--server 请填写服务根地址，例如 http://127.0.0.1:80，不带接口和查询参数")
    if args.record_root and (not PurePosixPath(args.record_root).is_absolute() or
                             ".." in PurePosixPath(args.record_root).parts or "\\" in args.record_root):
        parser.error("--record-root 请填写服务器/容器内的绝对目录，例如 /var/lib/zlm/www/hls-archive")
    return args


def load_secret(args):
    if args.secret is not None:
        secret = args.secret
    else:
        config = configparser.ConfigParser(interpolation=None, strict=False)
        try:
            with args.config.open(encoding="utf-8-sig") as source:
                config.read_file(source)
            secret = config.get("api", "secret")
        except (OSError, configparser.Error):
            raise ApiError("无法读取配置中的 [api] secret，请检查 --config 或显式传入 --secret") from None
    if not secret:
        raise ApiError("API secret 不能为空")
    return secret


def request_api(args, secret, action):
    endpoint = args.server.rstrip("/") + "/index/api/" + ENDPOINTS[action]
    parameters = {
        "secret": secret,
        "type": RECORD_TYPES[args.type],
        "vhost": args.vhost,
        "app": args.app,
        "stream": args.stream,
    }
    if action == "start" and args.record_root:
        record_id = time.strftime("%Y%m%d_%H%M%S") + "_" + uuid4().hex[:12]
        parameters["customized_path"] = args.record_root.rstrip("/") + "/" + record_id
        print("本次录制根目录：" + parameters["customized_path"], flush=True)
    # urlencode 自动处理密钥、流名中的特殊字符，日志不输出实际密钥。
    visible = dict(parameters, secret="<hidden>")
    print("GET " + endpoint + "?" + urlencode(visible), flush=True)
    request = Request(endpoint + "?" + urlencode(parameters), method="GET")
    try:
        with urlopen(request, timeout=args.timeout) as response:
            result = json.load(response)
    except HTTPError as error:
        raise ApiError("HTTP 请求失败，状态码 %d" % error.code) from None
    except (URLError, OSError):
        raise ApiError("连接失败或请求超时，请检查服务地址、端口及服务是否启动") from None
    except (ValueError, UnicodeError):
        raise ApiError("服务未返回有效 JSON，请检查服务地址是否正确") from None
    if not isinstance(result, dict):
        raise ApiError("API 返回格式错误，应为 JSON 对象")
    print(json.dumps(result, ensure_ascii=False, indent=2), flush=True)
    if result.get("code") != 0:
        hint = ""
        if result.get("code") == -100:
            hint = "；请核对运行服务实际加载的配置与 API secret"
        raise ApiError("API 失败：code=%s，msg=%s%s" % (
            result.get("code"), result.get("msg", ""), hint))
    if action == "status":
        if not isinstance(result.get("status"), bool):
            raise ApiError("API 未返回有效的 status 布尔值")
        print("录制状态：" + ("正在录制" if result["status"] else "未录制"), flush=True)
    elif result.get("result") is not True:
        raise ApiError("API 未确认录制操作成功")
    return result


def test_recordings(args, secret):
    print("计划录制 %d 次，每次 %g 秒，轮间等待 %g 秒（共 %d 次间隔）。" % (
        args.count, args.seconds, args.interval, args.count - 1), flush=True)
    for index in range(1, args.count + 1):
        print("\n[%d/%d] 开始本轮测试。" % (index, args.count), flush=True)
        try:
            # 每轮重新检查，不接管已经在进行的录制。
            if request_api(args, secret, "status")["status"]:
                raise ApiError("该流已在录制，请选择其他测试流，或自行停止已有任务后再运行 test")
            request_api(args, secret, "start")
            started_at = time.monotonic()
            try:
                if not request_api(args, secret, "status")["status"]:
                    raise ApiError("开始接口返回成功，但查询状态仍为未录制")
                print("[%d/%d] 录制 %g 秒；请保持推流……" % (
                    index, args.count, args.seconds), flush=True)
                # 查询状态的耗时计入本轮等待，减少 HTTP 请求带来的额外计时偏差。
                time.sleep(max(0, started_at + args.seconds - time.monotonic()))
            finally:
                # Ctrl+C 或中途错误也会尝试结束本轮启动的录制。
                request_api(args, secret, "stop")
            if request_api(args, secret, "status")["status"]:
                raise ApiError("停止接口返回成功，但查询状态仍为正在录制")
        except ApiError as error:
            raise ApiError("第 %d/%d 轮失败，已完成 %d 轮；%s" % (
                index, args.count, index - 1, error)) from None
        print("[%d/%d] 本轮录制接口检查通过。" % (index, args.count), flush=True)
        if index < args.count:
            print("等待 %g 秒后开始下一轮……" % args.interval, flush=True)
            time.sleep(args.interval)
    print("%d 次按需录制接口测试全部通过。文件落盘与可播放性请另行检查。" % args.count)


def main(argv=None):
    args = parse_arguments(argv)
    try:
        secret = load_secret(args)
        if args.action != "test":
            request_api(args, secret, args.action)
            return 0

        test_recordings(args, secret)
        return 0
    except ApiError as error:
        print("错误：" + str(error), file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("测试已中断。", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
