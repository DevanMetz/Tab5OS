"""Exercise the real pinned SDK client and HTTP app against loopback peers."""
import argparse
import base64
import contextlib
import csv
import importlib.util
import json
import pathlib
import subprocess
import threading
import time
from http.server import HTTPServer
from types import SimpleNamespace

root = pathlib.Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("http_fixture", root / "tools/http_test_server.py")
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)


class Handler(fixture.FixtureHandler):
    def reply(self, status, body, content_type="text/plain", extra_headers=()):
        self.server.replies.append((status, len(body)))
        if content_type == "application/json":
            self.server.received_bodies.append(base64.b64decode(json.loads(body)["body_base64"], validate=True))
        super().reply(status, body, content_type, extra_headers)

    def exchange(self):
        wire_mode = self.server.options.mode
        if wire_mode in ("informational", "informational-coalesced", "short-informational", "informational-wait",
                         "switch-protocol", "trailers", "boundary-trailers", "large-trailers", "short-trailers",
                         "invalid-trailers", "fragmented-trailers", "trailers-wait", "invalid-chunk",
                         "eof", "eof-silent", "no-content", "not-modified"):
            self.close_connection = True
            self.server.requests += 1
            with contextlib.suppress(OSError):
                informational = b"HTTP/1.1 103 Early Hints\r\nContent-Type: wrong/interim\r\nLocation: /wrong\r\n\r\n"
                final = b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 2\r\n\r\nOK"
                chunked = b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nTransfer-Encoding: chunked\r\n\r\n"
                terminal = chunked + b"2\r\nOK\r\n0\r\n"
                if wire_mode in ("informational", "informational-coalesced"):
                    packets = (informational, b"HTTP/1.1 100 Continue\r\n\r\n", final)
                    if wire_mode == "informational-coalesced":
                        self.connection.sendall(b"".join(packets))
                    else:
                        for packet in packets:
                            self.connection.sendall(packet)
                            time.sleep(0.05)
                elif wire_mode == "short-informational":
                    self.connection.sendall(informational)
                elif wire_mode == "switch-protocol":
                    self.connection.sendall(b"HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: fixture\r\n\r\n")
                elif wire_mode in ("informational-wait", "trailers-wait"):
                    prefix = informational + b"HTTP/1.1 200 OK\r\nX-Wait: value" if wire_mode == "informational-wait" else terminal + b"X-Wait: value"
                    self.connection.sendall(prefix)
                    until = time.monotonic() + self.server.options.seconds
                    while time.monotonic() < until:
                        self.connection.sendall(b"x")
                        time.sleep(self.server.options.interval)
                elif wire_mode in ("eof", "eof-silent"):
                    self.connection.sendall(b"HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nOK")
                    if wire_mode == "eof-silent":
                        time.sleep(self.server.options.seconds)
                elif wire_mode == "no-content":
                    self.connection.sendall(b"HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n")
                elif wire_mode == "not-modified":
                    self.connection.sendall(b"HTTP/1.1 304 Not Modified\r\nContent-Length: 999\r\nConnection: close\r\n\r\n")
                elif wire_mode == "invalid-chunk":
                    self.connection.sendall(chunked + b"2\r\nOK\r\nZ\r\n")
                else:
                    if wire_mode == "short-trailers":
                        trailer = b"X-Unfinished: value"
                    elif wire_mode == "invalid-trailers":
                        trailer = b"Bad Header: value\r\n\r\n"
                    elif wire_mode in ("large-trailers", "boundary-trailers"):
                        prefix, suffix = b"X-Large: ", b"\r\n\r\n"
                        padding = 9000 if wire_mode == "large-trailers" else 8192 - len(prefix) - len(suffix)
                        trailer = prefix + b"x" * padding + suffix
                    else:
                        trailer = b"X-Fixture: value\r\nX-Last: last\r\n\r\n"
                    response = terminal + trailer
                    if wire_mode == "fragmented-trailers":
                        for byte in response:
                            self.connection.sendall(bytes([byte]))
                            time.sleep(0.002)
                    else:
                        self.connection.sendall(response)
            return
        if self.server.options.mode in ("invalid-headers", "large-headers", "boundary-headers", "short-headers", "fragmented"):
            self.close_connection = True
            self.server.requests += 1
            prefix = b"HTTP/1.1 200 OK\r\nX-Large: "
            suffix = b"\r\nContent-Length: 2\r\nConnection: close\r\n\r\n"
            wire_mode = self.server.options.mode
            if wire_mode == "invalid-headers":
                response = b"HTTP/1.1 200 OK\r\nX-Good: value\r\nBad Header: value\r\n\r\n"
            elif wire_mode == "short-headers":
                response = b"HTTP/1.1 200 OK\r\nX-Unfinished: value"
            elif wire_mode == "fragmented":
                response = b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK"
            else:
                padding = 9000 if wire_mode == "large-headers" else 8192 - len(prefix) - len(suffix)
                response = prefix + b"x" * padding + suffix + b"OK"
            with contextlib.suppress(OSError):
                if wire_mode == "fragmented":
                    for byte in response:
                        self.connection.sendall(bytes([byte]))
                        time.sleep(0.002)
                else:
                    self.connection.sendall(response)
            return
        self.server.received.append((self.command, self.path, dict(self.headers)))
        super().exchange()

    do_GET = do_POST = do_PUT = do_DELETE = exchange


def run_case(executable, output, mode):
    wire_mode = mode
    if mode.startswith("log-"):
        wire_mode = "stream" if mode in ("log-cancel", "log-cancel-fault", "log-deadline") else "invalid-headers" if mode == "log-invalid" else "short" if mode == "log-incomplete-sync" else "echo"
    elif mode.startswith("dns-") or mode in ("methods", "repeat", "gates", "allocation", "limits", "utf8-body", "source-edits", "pending-clear", "pending-send", "ipv6-literal"):
        wire_mode = "echo"
    elif mode == "home":
        wire_mode = "stream"
    elif mode.startswith(("cancel-", "deadline-")):
        wire_mode = mode.split("-", 1)[1]
        if wire_mode == "headers":
            wire_mode = "slow-headers"
        elif wire_mode in ("informational", "trailers"):
            wire_mode += "-wait"
    seconds = 25 if mode.startswith("deadline-") or mode == "log-deadline" else 1.5 if mode == "cancel-silent" else 12 if mode in ("silent", "eof-silent") else 2 if mode.startswith("cancel-") or mode in ("home", "log-cancel", "log-cancel-fault") else 0.3
    with HTTPServer(("127.0.0.1", 0), Handler) as server:
        server.options = SimpleNamespace(mode=wire_mode, seconds=seconds, interval=0.05, reply_bytes=8192)
        server.requests = 0
        server.received = []
        server.received_bodies = []
        server.replies = []
        worker = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True)
        worker.start()
        try:
            result = subprocess.run([str(executable), str(server.server_port), mode, str(output)],
                                    capture_output=True, text=True, timeout=40,
                                    creationflags=subprocess.CREATE_NO_WINDOW)
        finally:
            server.shutdown()
            worker.join(3)
            assert not worker.is_alive()
        print(result.stdout, end="", flush=True)
        if result.returncode:
            print(result.stderr, end="", flush=True)
            raise AssertionError(f"{mode}: exit {result.returncode}")
        no_request = ("gates", "tls-fail-closed", "limits", "dns-failure", "dns-memory", "dns-post-failure", "dns-timeout", "dns-budget", "dns-tls", "dns-ipv6", "ipv6-literal")
        expected = 0 if mode in no_request else 4 if mode in ("methods", "log-success") else 26 if mode == "repeat" else 3 if mode in ("log-optin", "log-write") else 2 if mode in ("log-repair", "log-pending-error", "source-edits", "pending-send") else 1
        assert server.requests == expected, (mode, server.requests, expected)
        if mode.startswith("log-"):
            path = output / f"storage-{mode}" / "HTTP" / "HTTPLOG.CSV"
            if mode in ("log-mkdir", "log-open"):
                assert not path.exists()
                return
            raw = path.read_text(encoding="utf-8")
            assert "SECRET" not in raw and "Authorization" not in raw and "Content-Type" not in raw
            with path.open(encoding="utf-8", newline="") as stream:
                reader = csv.DictReader(stream)
                assert reader.fieldnames == ["unix_time", "method", "url", "status", "response_bytes", "duration_ms", "outcome"]
                rows = list(reader)
            count = 4 if mode == "log-success" else 2 if mode in ("log-repair", "log-write") else 1
            assert len(rows) == count, (mode, rows)
            if mode == "log-repair-error":
                assert rows == [dict(zip(reader.fieldnames, ["1", "GET", "http://example.invalid/", "200", "2", "1", "complete"]))]
                return
            safe_url = f'http://127.0.0.1:{server.server_port}/evidence,"quoted"'
            outcomes = {"log-cancel": "cancelled", "log-cancel-fault": "cancelled", "log-deadline": "request_deadline",
                        "log-invalid": "invalid_response_headers", "log-incomplete-sync": "incomplete_response"}
            for i, row in enumerate(rows):
                assert row["url"] == safe_url and row["outcome"] == outcomes.get(mode, "complete")
                assert row["method"] == (["GET", "POST", "PUT", "DELETE"][i] if mode == "log-success" else "GET")
                assert int(row["unix_time"]) > 0 and 0 <= int(row["duration_ms"]) < 16000
                assert int(row["status"]) == (-1 if mode == "log-invalid" else 200)
                if mode == "log-invalid":
                    assert row["response_bytes"] == "0"
                elif mode == "log-incomplete-sync":
                    assert row["response_bytes"] == "5"
                elif mode in ("log-cancel", "log-cancel-fault", "log-deadline"):
                    assert int(row["response_bytes"]) > 0 and int(row["response_bytes"]) % 5 == 0
                else:
                    reply_index = 1 if mode == "log-optin" else (0, 2)[i] if mode == "log-write" else i
                    assert int(row["response_bytes"]) == server.replies[reply_index][1]
        if mode == "methods":
            assert [request[0] for request in server.received] == ["GET", "POST", "PUT", "DELETE"]
            assert [request[2].get("Content-Length", "0") for request in server.received] == ["0", "13", "13", "0"]
            for _, path, headers in server.received:
                assert path == "/?test=discarded"  # Fragments never reach the peer.
                assert headers["Authorization"] == "Bearer disposable"
                assert headers["X-Tab5-Test"] == "fixture"
            assert server.received_bodies == [b"", b'{"test":true}', b'{"test":true}', b""]
        elif mode == "utf8-body":
            assert server.received[0][0] == "POST"
            assert server.received[0][2]["Content-Length"] == "1020"
            assert server.received_bodies == ["\U0001f600".encode("utf-8") * 255]
        elif mode == "source-edits":
            assert [(request[0], request[1]) for request in server.received] == [
                ("GET", "/?test=discarded"), ("POST", "/second?token=SOURCE_QUERY_SECRET")]
            assert "X-Changed" not in server.received[0][2] and server.received[1][2]["X-Changed"] == "captured"
            assert server.received_bodies == [b"", b'{"changed":true}']
        elif mode in ("dns-cached", "dns-delayed", "dns-late", "dns-queued-next"):
            hostname = "cache.fixture.test" if mode == "dns-cached" else "slow.fixture.test" if mode == "dns-delayed" else "next.fixture.test"
            assert server.received[0][2]["Host"] == f"{hostname}:{server.server_port}"
            assert server.received[0][1] == "/dns?token=DNS_QUERY_SECRET"


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("modes", nargs="*", default=["echo", "methods", "large", "redirect", "short", "stream",
                        "slow-headers", "silent", "gates", "limits", "utf8-body", "source-edits", "pending-clear", "pending-send", "allocation", "tls-fail-closed", "invalid-headers", "large-headers",
                        "boundary-headers", "short-headers", "fragmented", "informational", "trailers",
                        "informational-coalesced", "short-informational", "switch-protocol", "boundary-trailers",
                        "large-trailers", "short-trailers", "invalid-trailers", "fragmented-trailers", "invalid-chunk",
                        "eof", "eof-silent", "no-content", "not-modified", "cancel-informational", "cancel-trailers",
                        "deadline-informational", "deadline-trailers",
                        "cancel-silent", "cancel-headers", "cancel-stream", "home", "deadline-headers", "deadline-stream", "repeat",
                        "log-optin", "log-success", "log-repair", "log-write", "log-mkdir", "log-repair-error", "log-open",
                        "log-flush", "log-sync", "log-close", "log-incomplete-sync", "log-cancel", "log-cancel-fault",
                        "log-deadline", "log-invalid", "log-busy", "log-pending-error", "log-pending-clear",
                        "dns-cached", "dns-delayed", "dns-failure", "dns-memory", "dns-post-failure",
                        "dns-cancel", "dns-queued-cancel", "dns-queued-next", "dns-late", "dns-timeout", "dns-budget", "dns-tls", "dns-ipv6", "ipv6-literal"])
    args = parser.parse_args()
    for mode in args.modes:
        run_case(args.executable, args.output, mode)
    print(f"HTTP Console: {len(args.modes)} real SDK/socket/UI scenarios passed")
