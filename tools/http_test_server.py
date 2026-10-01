"""Read-only HTTP peer for Tab5 HTTP Console tests; Python standard library only.

Defaults to loopback. Use --bind <PC-LAN-IPv4> --port 18080 for tablet tests.
All methods operate on synthetic data; no files are read or changed. Logs contain
request counts and lengths, never request bodies, headers or query strings.
"""

import argparse
import base64
import json
import math
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import urlsplit


class FixtureHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def setup(self):
        super().setup()
        self.connection.settimeout(5)

    def reply(self, status, body, content_type="text/plain", extra_headers=()):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        for name, value in extra_headers:
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(body)

    def exchange(self):
        self.close_connection = True
        started = time.monotonic()
        options = self.server.options
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 <= length <= 1023 or self.headers.get("Transfer-Encoding"):
                self.reply(400, b"Expected a fixed request body of 0-1023 bytes.\n")
                return
            body = self.rfile.read(length)
            if len(body) != length:
                return
            target = urlsplit(self.path).path == "/redirect-target"
            self.server.requests += 1
            print(f"Request {self.server.requests}: {self.command}, {length} body bytes, "
                  f"mode={options.mode}, redirect_target={target}", flush=True)
            if target:
                self.reply(200, b"REDIRECT FOLLOWED\n")
            elif options.mode == "echo":
                result = json.dumps({"method": self.command, "path": self.path,
                                     "headers": dict(self.headers),
                                     "body_base64": base64.b64encode(body).decode("ascii")},
                                    indent=2).encode("utf-8") + b"\n"
                self.reply(200, result, "application/json")
            elif options.mode == "large":
                pattern = b"0123456789ABCDEF\n"
                result = (pattern * (options.reply_bytes // len(pattern) + 1))[:options.reply_bytes]
                self.reply(200, result)
            elif options.mode == "redirect":
                self.reply(302, b"Redirect is intentionally not followed.\n",
                           extra_headers=(("Location", "/redirect-target?fixture=1"),))
            elif options.mode == "short":
                self.send_response(200)
                self.send_header("Content-Length", "32")
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(b"short")
            elif options.mode == "silent":
                time.sleep(options.seconds)
            elif options.mode == "slow-headers":
                self.connection.sendall(b"HTTP/1.1 200 OK\r\nX-Drip: ")
                while time.monotonic() - started < options.seconds:
                    self.connection.sendall(b"x")
                    time.sleep(options.interval)
                self.connection.sendall(b"\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK")
            elif options.mode == "stream":
                self.send_response(200)
                self.send_header("Content-Type", "text/plain")
                self.send_header("Transfer-Encoding", "chunked")
                self.send_header("Connection", "close")
                self.end_headers()
                while time.monotonic() - started < options.seconds:
                    self.wfile.write(b"5\r\ndrip\n\r\n")
                    self.wfile.flush()
                    time.sleep(options.interval)
                self.wfile.write(b"0\r\n\r\n")
            print(f"Response finished after {round((time.monotonic() - started) * 1000)} ms", flush=True)
        except (OSError, ValueError) as error:
            print(f"Connection ended after {round((time.monotonic() - started) * 1000)} ms "
                  f"({type(error).__name__})", flush=True)

    do_GET = exchange
    do_POST = exchange
    do_PUT = exchange
    do_DELETE = exchange


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=18080)
    parser.add_argument("--mode", choices=("echo", "large", "redirect", "short", "silent",
                                          "slow-headers", "stream"), default="echo")
    parser.add_argument("--reply-bytes", type=int, default=8192)
    parser.add_argument("--seconds", type=float, default=15)
    parser.add_argument("--interval", type=float, default=0.2)
    args = parser.parse_args()
    if not 1 <= args.port <= 65535 or not 0 <= args.reply_bytes <= 65536:
        parser.error("Port must be 1-65535; reply size must be 0-65536 bytes")
    if not math.isfinite(args.seconds) or not 0 <= args.seconds <= 60:
        parser.error("Duration must be finite and between 0 and 60 seconds")
    if not math.isfinite(args.interval) or not 0.02 <= args.interval <= 10:
        parser.error("Interval must be finite and between 0.02 and 10 seconds")
    with HTTPServer((args.bind, args.port), FixtureHandler) as server:
        server.options = args
        server.requests = 0
        print(f"HTTP fixture on {args.bind}:{args.port}, mode={args.mode}; Ctrl+C stops", flush=True)
        server.serve_forever(poll_interval=0.2)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("Fixture stopped")
