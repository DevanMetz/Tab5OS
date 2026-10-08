"""Loopback image/redirect fixture for the actual pinned SDK OTA path and RAM sink."""
import argparse
import contextlib
import hashlib
import http.server
import json
import pathlib
import struct
import subprocess
import threading
import time

MODES = (
    "direct", "chunked", "close-delimited", "fragmented", "redirect-empty", "redirect-body",
    "redirect-chain", "redirect-fragmented", "wrong-hash", "wrong-size", "wrong-version",
    "corrupt-image", "long-image", "bad-description", "wrong-chip", "short-header", "truncated",
    "http-error", "missing-slot", "begin-fault", "write-fault", "late-write-fault", "end-fault", "boot-fault",
    "long-image-matching-hash", "short-image-matching-hash", "repeat",
)
ERRORS = {"wrong-hash": 0x109, "wrong-size": 0x104, "wrong-version": 0x10a,
          "corrupt-image": 0x109, "long-image": 0x109, "wrong-chip": 0x10a,
          "long-image-matching-hash": 0x109, "short-image-matching-hash": 0x109}
SUCCESSES = MODES[:8] + ("repeat",)


def image():
    data = bytearray((index * 37 + 11) & 255 for index in range(8192))
    data[:288] = bytes(288)
    data[0] = 0xe9
    data[1] = 1
    struct.pack_into("<H", data, 12, 0x12)
    struct.pack_into("<II", data, 24, 0x3f000000, len(data) - 32)
    struct.pack_into("<I", data, 32, 0xabcd5432)
    version = b"v0.6.0-fixture"
    data[48:48 + len(version)] = version
    return bytes(data)


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass

    def do_GET(self):
        server = self.server
        mode = server.mode
        server.requests.append(self.path)
        redirected = mode.startswith("redirect-") and self.path != "/image"
        if redirected:
            body = b"" if mode == "redirect-empty" else b"Redirect response is not image data.\x00\xff" * 7
            location = "/again" if mode == "redirect-chain" and self.path != "/again" else "/image"
            response = (f"HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:{server.server_port}{location}\r\n"
                        f"Content-Length: {len(body)}\r\nConnection: keep-alive\r\n\r\n").encode() + body
        else:
            body = bytearray(server.image)
            if mode == "wrong-version":
                body[48:80] = b"v0.7.0-fixture".ljust(32, b"\0")
            elif mode == "bad-description":
                body[32:36] = bytes(4)
            elif mode == "wrong-chip":
                struct.pack_into("<H", body, 12, 0xd)
            elif mode == "corrupt-image":
                body[-1] ^= 1
            elif mode == "short-header":
                body = body[:512]
            elif mode in ("long-image", "long-image-matching-hash"):
                body.append(0)
            elif mode == "short-image-matching-hash":
                body = body[:-1]
            if mode == "http-error":
                response = b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
            elif mode in ("chunked", "long-image", "long-image-matching-hash", "short-image-matching-hash"):
                response = b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
                for offset in range(0, len(body), 173):
                    chunk = body[offset:offset + 173]
                    response += f"{len(chunk):x}\r\n".encode() + chunk + b"\r\n"
                response += b"0\r\n\r\n"
            elif mode == "close-delimited":
                response = b"HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n" + body
            else:
                declared = len(body)
                if mode == "truncated":
                    body = body[:2049]
                response = (f"HTTP/1.1 200 OK\r\nContent-Length: {declared}\r\n"
                            "Connection: close\r\n\r\n").encode() + body
        server.responses.append({"path": self.path, "wireBytes": len(response),
                                 "sha256": hashlib.sha256(response).hexdigest(), "redirect": redirected})
        with contextlib.suppress(OSError):
            if mode in ("fragmented", "redirect-fragmented"):
                for offset in range(0, len(response), 37):
                    self.connection.sendall(response[offset:offset + 37])
                    time.sleep(0.001)
            else:
                self.connection.sendall(response)
        self.close_connection = not redirected


def run_case(executable, output, mode, data):
    source = output / "fixture.bin"
    digest = hashlib.sha256(data).hexdigest()
    digest_argument = hashlib.sha256(data + b"\0").hexdigest() if mode == "long-image-matching-hash" else (
        hashlib.sha256(data[:-1]).hexdigest() if mode == "short-image-matching-hash" else digest)
    with http.server.HTTPServer(("127.0.0.1", 0), Handler) as server:
        server.mode = mode
        server.image = data
        server.requests = []
        server.responses = []
        worker = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.02}, daemon=True)
        worker.start()
        expected = 0 if mode in SUCCESSES else ERRORS.get(mode, -1)
        try:
            completed = subprocess.run([str(executable), str(server.server_port), mode, str(source), digest_argument, str(expected)],
                capture_output=True, text=True, timeout=30, creationflags=subprocess.CREATE_NO_WINDOW)
        finally:
            server.shutdown()
            worker.join(3)
            assert not worker.is_alive()
        log = completed.stdout + completed.stderr
        (output / f"{mode}.log").write_text(log)
        evidence = {"mode": mode, "expectedError": expected, "exitCode": completed.returncode,
                    "imageBytes": len(data), "imageSha256": digest, "digestArgument": digest_argument,
                    "requests": server.requests, "responses": server.responses}
        (output / f"{mode}-wire.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(log, end="", flush=True)
        assert completed.returncode == 0, mode
        assert len(server.requests) == (26 if mode == "repeat" else 3 if mode == "redirect-chain" else 2 if mode.startswith("redirect-") else 1)
        return evidence


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("modes", nargs="*", default=list(MODES))
    args = parser.parse_args()
    if set(args.modes) - set(MODES):
        parser.error("Unknown OTA image mode")
    args.output.mkdir(parents=True, exist_ok=True)
    data = image()
    (args.output / "fixture.bin").write_bytes(data)
    report = [run_case(args.executable, args.output, mode, data) for mode in args.modes]
    (args.output / "image-checks.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"PASS {len(report)} real SDK OTA/HTTP/SHA image cases with RAM-only partitions")


if __name__ == "__main__":
    main()
