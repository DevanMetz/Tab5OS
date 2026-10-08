"""Loopback manifest/image fixture for the actual pinned SDK OTA path and RAM sink."""
import argparse
import contextlib
import hashlib
import http.server
import json
import pathlib
import re
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
    "informational", "trailer", "partial-header", "header-overflow", "trailer-truncated",
    "header-trickle", "description-trickle", "body-trickle", "transport-allocation", "transport-init", "metadata-repeat",
    "large-length", "aliased-length", "narrow-length", "max-length", "zero-padded-length",
    "oversized-stream", "oversized-close-delimited", "oversized-stream-matching-hash",
)
FETCH_MODES = (
    "manifest-direct", "manifest-fragmented", "manifest-chunked", "manifest-close-delimited",
    "manifest-redirect-body", "manifest-redirect-chain", "manifest-incomplete-length", "manifest-incomplete-chunked",
    "manifest-truncated-json", "manifest-exact-cap", "manifest-overflow", "manifest-nul-suffix",
    "manifest-http-error", "manifest-repeat",
    "manifest-large-length", "manifest-max-length",
    "manifest-informational", "manifest-trailer", "manifest-partial-header", "manifest-header-overflow",
    "manifest-trailer-truncated", "manifest-header-deadline", "manifest-body-deadline", "manifest-metadata-repeat",
    "manifest-header-trickle", "manifest-transport-allocation", "manifest-transport-init",
)
ERRORS = {"wrong-hash": 0x109, "wrong-size": 0x104, "wrong-version": 0x10a, "short-header": 0x104,
          "corrupt-image": 0x109, "long-image": 0x109, "wrong-chip": 0x10a,
          "long-image-matching-hash": 0x109, "short-image-matching-hash": 0x109,
          "header-overflow": 0x108,
          "header-trickle": 0x107, "description-trickle": 0x107, "body-trickle": 0x107,
          "transport-allocation": 0x101, "transport-init": 0x101,
          "large-length": 0x104, "aliased-length": 0x104, "narrow-length": 0x104, "max-length": 0x108,
          "oversized-stream": 0x109, "oversized-close-delimited": 0x109, "oversized-stream-matching-hash": 0x109,
          "manifest-incomplete-length": 0x104, "manifest-incomplete-chunked": 0x104,
          "manifest-truncated-json": 0x108, "manifest-overflow": 0x104, "manifest-nul-suffix": 0x108,
          "manifest-large-length": 0x104, "manifest-max-length": 0x108,
          "manifest-partial-header": 0x7004, "manifest-header-overflow": 0x108,
          "manifest-trailer-truncated": 0x104,
          "manifest-header-deadline": 0x107, "manifest-body-deadline": 0x107,
          "manifest-header-trickle": 0x107, "manifest-transport-allocation": 0x101, "manifest-transport-init": 0x101}
SUCCESSES = MODES[:8] + ("repeat", "informational", "trailer", "metadata-repeat", "zero-padded-length") + FETCH_MODES[:6] + (
    "manifest-exact-cap", "manifest-repeat", "manifest-informational", "manifest-trailer", "manifest-metadata-repeat")


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


def manifest(data):
    return json.dumps({"schema": 1, "version": "v0.6.0-fixture", "hardware": "m5stack-tab5",
        "size": len(data), "sha256": hashlib.sha256(data).hexdigest(),
        "url": "https://github.com/DevanMetz/Tab5OS/releases/download/v0.6.0/tab5_os.bin",
        "channel": "stable", "minimum_predecessor": "v0.5.1"}, separators=(",", ":")).encode()


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass

    def do_GET(self):
        server = self.server
        mode = server.mode
        fetching = mode.startswith("manifest-")
        kind = mode.removeprefix("manifest-")
        server.requests.append(self.path)
        final_path = "/manifest.json" if fetching else "/image"
        redirected = kind.startswith("redirect-") and self.path != final_path
        if redirected:
            body = b"" if kind == "redirect-empty" else b"Redirect response is not image data.\x00\xff" * (57 if fetching else 7)
            location = "/again" if kind == "redirect-chain" and self.path != "/again" else final_path
            response = (f"HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:{server.server_port}{location}\r\n"
                        f"Content-Length: {len(body)}\r\nConnection: keep-alive\r\n\r\n").encode() + body
        else:
            body = bytearray(manifest(server.image) if fetching else server.image)
            if fetching:
                if kind == "truncated-json":
                    body = body[:-1]
                elif kind in ("exact-cap", "overflow"):
                    body.extend(b" " * ((1536 if kind == "exact-cap" else 1537) - len(body)))
                elif kind == "nul-suffix":
                    body.extend(b"\0hidden")
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
            elif kind in ("oversized-stream", "oversized-close-delimited", "oversized-stream-matching-hash"):
                body *= 4
            if kind in ("partial-header", "header-deadline"):
                response = b"HTTP/1.1 200 OK\r\nX-Probe: unfinished-value"
            elif kind == "header-overflow":
                response = (b"HTTP/1.1 200 OK\r\nX-Probe: " + b"x" * 9000 +
                            f"\r\nContent-Length: {len(body)}\r\nConnection: close\r\n\r\n".encode() + body)
            elif kind == "http-error":
                response = b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
            elif kind in ("chunked", "incomplete-chunked", "long-image", "long-image-matching-hash", "short-image-matching-hash",
                          "trailer", "trailer-truncated", "metadata-repeat", "oversized-stream", "oversized-stream-matching-hash"):
                transfer_name = b"tRaNsFeR-EnCoDiNg" if kind == "incomplete-chunked" else b"Transfer-Encoding"
                response = b"HTTP/1.1 200 OK\r\n" + transfer_name + b": chunked\r\nConnection: close\r\n\r\n"
                for offset in range(0, len(body), 173):
                    chunk = body[offset:offset + 173]
                    response += f"{len(chunk):x}\r\n".encode() + chunk + b"\r\n"
                if kind in ("trailer", "metadata-repeat"):
                    response += b"0\r\nX-Probe: trailing-value\r\n\r\n"
                elif kind == "trailer-truncated":
                    response += b"0\r\nX-Probe: unfinished-value"
                elif kind != "incomplete-chunked":
                    response += b"0\r\n\r\n"
            elif kind in ("close-delimited", "oversized-close-delimited"):
                response = b"HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n" + body
            else:
                declared = len(body)
                if kind == "incomplete-length":
                    declared += 20
                elif kind == "large-length":
                    declared = 1 << 63
                elif kind == "aliased-length":
                    declared = (1 << 63) + len(body)
                elif kind == "narrow-length":
                    declared = (1 << 32) + len(body)
                elif kind == "max-length":
                    declared = (1 << 64) - 1
                elif kind == "zero-padded-length":
                    declared = f"000{len(body)}"
                if mode == "truncated":
                    body = body[:2049]
                response = (f"HTTP/1.1 200 OK\r\nContent-Length: {declared}\r\n"
                            "Connection: close\r\n\r\n").encode() + (body[:10] if kind == "body-deadline" else body)
            if kind in ("informational", "metadata-repeat"):
                response = b"HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 103 Early Hints\r\nLink: </probe>\r\n\r\n" + response
        server.responses.append({"path": self.path, "wireBytes": len(response),
                                 "sha256": hashlib.sha256(response).hexdigest(), "redirect": redirected})
        with contextlib.suppress(OSError):
            if kind == "header-trickle":
                for byte in response:
                    self.connection.sendall(bytes((byte,)))
                    time.sleep(0.3)
            elif kind in ("description-trickle", "body-trickle"):
                header_end = response.index(b"\r\n\r\n") + 4
                prefix = 512 if kind == "description-trickle" else 2048
                self.connection.sendall(response[:header_end + prefix])
                for byte in response[header_end + prefix:]:
                    self.connection.sendall(bytes((byte,)))
                    time.sleep(0.03)
            elif kind in ("informational", "metadata-repeat"):
                final = response.index(b"HTTP/1.1 200 OK")
                self.connection.sendall(response[:final])
                time.sleep(0.025)
                self.connection.sendall(response[final:])
            elif kind in ("fragmented", "redirect-fragmented"):
                for offset in range(0, len(response), 37):
                    self.connection.sendall(response[offset:offset + 37])
                    time.sleep(0.001)
            else:
                self.connection.sendall(response)
            if kind in ("header-deadline", "body-deadline"):
                time.sleep(16)
        self.close_connection = not redirected


def run_case(executable, output, mode, data):
    source = output / "fixture.bin"
    digest = hashlib.sha256(data).hexdigest()
    digest_data = data
    if mode == "oversized-stream-matching-hash":
        digest_data = data * 4
    elif mode == "long-image-matching-hash":
        digest_data = data + b"\0"
    elif mode == "short-image-matching-hash":
        digest_data = data[:-1]
    digest_argument = hashlib.sha256(digest_data).hexdigest()
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
        evidence = {"mode": mode, "operation": "manifest" if mode in FETCH_MODES else "image",
                    "expectedError": expected, "exitCode": completed.returncode,
                    "imageBytes": len(data), "imageSha256": digest, "digestArgument": digest_argument,
                    "requests": server.requests, "responses": server.responses}
        framing = re.search(r"^HTTP_RESPONSE [\w-]+ content_length=(-?\d+) complete=([01])$", log, re.M)
        if framing:
            evidence["responseContentLength"] = int(framing[1])
            evidence["responseComplete"] = framing[2] == "1"
        deadline = re.search(r"^IMAGE_DEADLINE [\w-]+ elapsed_ms=(\d+) wall_ms=(\d+) clock_scale=(\d+)$", log, re.M)
        if deadline:
            evidence["deadlineElapsedMs"] = int(deadline[1])
            evidence["deadlineWallMs"] = int(deadline[2])
            evidence["clockScale"] = int(deadline[3])
        (output / f"{mode}-wire.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(log, end="", flush=True)
        assert completed.returncode == 0, mode
        kind = mode.removeprefix("manifest-")
        assert len(server.requests) == (0 if kind in ("transport-allocation", "transport-init") else
            26 if kind in ("repeat", "metadata-repeat") else 3 if kind == "redirect-chain" else 2 if kind.startswith("redirect-") else 1)
        return evidence


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("modes", nargs="*", default=list(MODES + FETCH_MODES))
    args = parser.parse_args()
    if set(args.modes) - set(MODES + FETCH_MODES):
        parser.error("Unknown OTA image mode")
    args.output.mkdir(parents=True, exist_ok=True)
    data = image()
    (args.output / "fixture.bin").write_bytes(data)
    report = [run_case(args.executable, args.output, mode, data) for mode in args.modes]
    (args.output / "image-checks.json").write_text(json.dumps(report, indent=2) + "\n")
    images = sum(row["operation"] == "image" for row in report)
    print(f"PASS {len(report)} real SDK OTA/HTTP/SHA cases ({images} images, {len(report) - images} manifest fetches) with RAM-only partitions")


if __name__ == "__main__":
    main()
