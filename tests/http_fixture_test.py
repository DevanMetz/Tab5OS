"""Check the hardware fixture's HTTP framing with Python's independent client."""

import base64
import http.client
import importlib.util
import json
from pathlib import Path
import threading
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("http_fixture", ROOT / "tools/http_test_server.py")
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)


def exchange(mode):
    with fixture.HTTPServer(("127.0.0.1", 0), fixture.FixtureHandler) as server:
        server.options = SimpleNamespace(mode=mode, seconds=0.06, interval=0.02, reply_bytes=8192)
        server.requests = 0
        worker = threading.Thread(target=server.handle_request, daemon=True)
        worker.start()
        client = http.client.HTTPConnection("127.0.0.1", server.server_port, timeout=2)
        try:
            payload = b"\x00A\xff"
            client.request("PUT", "/?fixture=discarded#test", payload,
                           {"Content-Type": "application/octet-stream", "X-Tab5-Test": "fixture"})
            if mode == "informational":
                wire = bytearray()
                while part := client.sock.recv(4096):
                    wire.extend(part)
                first, rest = bytes(wire).split(b"\r\n\r\n", 1)
                assert first == b"HTTP/1.1 103 Early Hints\r\nLink: </fixture>"
                second, rest = rest.split(b"\r\n\r\n", 1)
                assert second == b"HTTP/1.1 100 Continue"
                final, body = rest.split(b"\r\n\r\n", 1)
                assert final.startswith(b"HTTP/1.1 200 OK\r\n") and b"Content-Length: 2\r\n" in final
                assert body == b"OK"
                return
            if mode == "silent":
                try:
                    client.getresponse()
                except http.client.RemoteDisconnected:
                    pass
                else:
                    raise AssertionError("Silent fixture sent an HTTP response")
                return
            response = client.getresponse()
            if mode == "short":
                assert response.status == 200 and response.getheader("Content-Length") == "32"
                try:
                    response.read()
                except http.client.IncompleteRead as error:
                    assert error.partial == b"short" and error.expected == 27
                else:
                    raise AssertionError("Truncated response was complete")
                return
            body = response.read()
            if mode == "echo":
                assert response.status == 200
                data = json.loads(body)
                assert data["method"] == "PUT"
                assert data["path"] == "/?fixture=discarded#test"
                assert data["headers"]["X-Tab5-Test"] == "fixture"
                assert base64.b64decode(data["body_base64"], validate=True) == payload
            elif mode == "large":
                assert len(body) == 8192 and body[:17] == b"0123456789ABCDEF\n"
                assert response.getheader("Content-Length") == "8192"
            elif mode == "redirect":
                assert response.status == 302
                assert response.getheader("Location") == "/redirect-target?fixture=1"
                assert b"REDIRECT FOLLOWED" not in body
            elif mode == "slow-headers":
                assert response.status == 200 and body == b"OK"
            elif mode == "stream":
                assert response.getheader("Transfer-Encoding") == "chunked"
                assert len(body) >= 5 and len(body) % 5 == 0
                assert body == b"drip\n" * (len(body) // 5)
            elif mode == "trailers":
                assert response.status == 200 and response.getheader("Transfer-Encoding") == "chunked"
                assert body == b"OK"
        finally:
            client.close()
            worker.join(timeout=3)
            assert not worker.is_alive()
            assert server.requests == 1


if __name__ == "__main__":
    for case in ("echo", "large", "redirect", "short", "silent", "slow-headers", "stream", "informational", "trailers"):
        exchange(case)
    print("HTTP fixture: nine real-socket framing checks passed")
