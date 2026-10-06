"""Independent wire and real-loopback checks for the controlled DNS fixture."""

from contextlib import contextmanager, redirect_stdout
import http.client
import importlib.util
import io
import json
from pathlib import Path
import socket
import struct
import subprocess
import sys
import threading
import time
from types import SimpleNamespace
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("dns_fixture", ROOT / "tools/dns_test_server.py")
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)
http_spec = importlib.util.spec_from_file_location("http_fixture", ROOT / "tools/http_test_server.py")
http_fixture = importlib.util.module_from_spec(http_spec)
http_spec.loader.exec_module(http_fixture)

A = bytes.fromhex("c00c 0001 0001 00000000 0004 7f000001")
AAAA = bytes.fromhex("c00c 001c 0001 00000000 0010 20010db8000000000000000000000005")
OPT = bytes.fromhex("00 0029 0200 00000000 0000")


def query(name="run.fixture.test", kind=1, *, ident=0x5A71, flags=0x0100, qclass=1, opt=b""):
    labels = name.split(".") if isinstance(name, str) and name else name or []
    labels = [part.encode("ascii") if isinstance(part, str) else part for part in labels]
    question = b"".join(bytes((len(part),)) + part for part in labels) + b"\0" + struct.pack("!HH", kind, qclass)
    return struct.pack("!6H", ident, flags, 1, 0, 0, int(bool(opt))) + question + opt, question


@contextmanager
def running(**options):
    logs, errors, stop = [], [], threading.Event()
    with fixture.DNSFixture(("127.0.0.1", 0), logger=logs.append, **options) as server:
        def work():
            try:
                server.serve_forever(stop)
            except BaseException as error:
                errors.append(error)
        worker = threading.Thread(target=work)
        worker.start()
        try:
            yield server, logs
        finally:
            stop.set()
            worker.join(2)
            assert not worker.is_alive(), "Fixture failed to stop promptly"
            assert not server.pending, "Delayed packets retained after stop"
            if errors:
                raise errors[0]
    assert server.socket.fileno() == -1, "Fixture socket was not closed"


def exchange(server, packet, timeout=1):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
        client.settimeout(timeout)
        client.sendto(packet, server.address)
        try:
            reply, peer = client.recvfrom(1024)
        except socket.timeout:
            return None
        assert peer == server.address, "Reply came from a different peer"
        return reply


def wait_for(predicate):
    end = time.monotonic() + 2
    while not predicate() and time.monotonic() < end:
        time.sleep(0.01)
    assert predicate(), "Fixture did not process the expected packets"


class DNSFixtureTests(unittest.TestCase):
    def expect(self, server, packet, question, flags, records=(), opt=b""):
        # Expected records use fixed DNS wire bytes, independent of the fixture encoder.
        header = packet[:2] + struct.pack("!5H", flags, int(bool(question)), len(records), 0, int(bool(opt)))
        reply = exchange(server, packet)
        self.assertEqual(reply, header + question + b"".join(records) + opt)
        self.assertLessEqual(len(reply), 512)

    def test_a_names_flags_and_zone_boundaries(self):
        with running() as (server, _):
            for name in ("fixture.test", "run.fixture.test", "RUN.FIXTURE.TEST", "a.b.fixture.test"):
                packet, question = query(name)
                self.expect(server, packet, question, 0x8500, (A,))
            packet, question = query(flags=0x0010)  # No RD; CD copied, RA remains clear.
            self.expect(server, packet, question, 0x8410, (A,))
            for name in ("test", "other.test", "evilfixture.test", "fixture.test.example", ""):
                packet, question = query(name)
                self.expect(server, packet, question, 0x8105)

    def test_aaaa_any_and_nodata(self):
        with running(answer_v6="2001:db8::5") as (server, _):
            packet, question = query(kind=28)
            self.expect(server, packet, question, 0x8500, (AAAA,))
            packet, question = query(kind=255)
            self.expect(server, packet, question, 0x8500, (A, AAAA))
            for kind in (2, 15, 16, 65, 65535):
                packet, question = query(kind=kind)
                self.expect(server, packet, question, 0x8500)
        with running() as (server, _):
            packet, question = query(kind=28)
            self.expect(server, packet, question, 0x8500)
            packet, question = query(kind=255)
            self.expect(server, packet, question, 0x8500, (A,))

    def test_custom_ipv4_and_ttl(self):
        with running(answer="192.0.2.17", ttl=123) as (server, _):
            packet, question = query()
            expected = bytes.fromhex("c00c 0001 0001 0000007b 0004 c0000211")
            self.expect(server, packet, question, 0x8500, (expected,))

    def test_error_modes_and_unsupported_queries(self):
        for mode, flags in (("nxdomain", 0x8503), ("servfail", 0x8102)):
            with self.subTest(mode=mode), running(mode=mode) as (server, _):
                packet, question = query()
                self.expect(server, packet, question, flags)
                packet, question = query("other.test")
                self.expect(server, packet, question, 0x8105)
        with running() as (server, _):
            packet, question = query(qclass=3)
            self.expect(server, packet, question, 0x8105)
            packet, question = query(flags=0x0900)  # Unsupported opcode is copied in NOTIMP.
            self.expect(server, packet, question, 0x8904)

    def test_edns_options_and_badvers(self):
        # Unknown option 65001 is safely ignored; DO is returned, other EDNS flags clear.
        options = bytes.fromhex("fde9 0003 010203")
        request_opt = bytes.fromhex("00 0029 1000 00008001") + struct.pack("!H", len(options)) + options
        with running() as (server, _):
            packet, question = query(opt=request_opt)
            response_opt = bytes.fromhex("00 0029 0200 00008000 0000")
            self.expect(server, packet, question, 0x8500, (A,), response_opt)
            packet, question = query(opt=bytes.fromhex("00 0029 1000 00010000 0000"))
            response_opt = bytes.fromhex("00 0029 0200 01000000 0000")
            self.expect(server, packet, question, 0x8100, opt=response_opt)
            packet, question = query("other.test", opt=OPT)
            self.expect(server, packet, question, 0x8105, opt=OPT)

    def test_name_and_datagram_limits(self):
        labels = [b"a" * 63, b"b" * 63, b"c" * 63, b"d" * 48, b"fixture", b"test"]
        packet, question = query(labels, kind=255, opt=OPT)
        self.assertEqual(len(question) - 4, 255)
        with running(answer_v6="2001:db8::5") as (server, _):
            self.expect(server, packet, question, 0x8500, (A, AAAA), OPT)
            self.assertEqual(len(exchange(server, packet)), 326)
            labels[3] += b"d"
            packet, _ = query(labels)
            self.expect(server, packet, b"", 0x8101)
            packet, question = query()
            # A valid padding option makes the incoming datagram exactly 512 bytes.
            padding = bytes(512 - len(packet) - 15)
            request_opt = bytes.fromhex("00 0029 0200 00000000") + struct.pack("!H", len(padding) + 4)
            request_opt += struct.pack("!HH", 12, len(padding)) + padding
            packet, question = query(opt=request_opt)
            self.assertEqual(len(packet), 512)
            self.expect(server, packet, question, 0x8500, (A,), OPT)
            self.assertIsNone(exchange(server, packet + b"x", timeout=0.06))
            packet, question = query()
            self.expect(server, packet, question, 0x8500, (A,))

    def test_malformed_queries_are_bounded_and_server_recovers(self):
        packet, question = query()
        malformed = [
            packet[:12], packet[:-1], packet + b"x",
            struct.pack("!6H", 0x5A71, 0x100, 0, 0, 0, 0),
            struct.pack("!6H", 0x5A71, 0x100, 2, 0, 0, 0) + question * 2,
            struct.pack("!6H", 0x5A71, 0x100, 1, 1, 0, 0) + question,
            struct.pack("!6H", 0x5A71, 0x100, 1, 0, 1, 0) + question,
        ]
        for bad_name in (b"\xc0", b"\xc0\x0c", b"\xc0\xff", b"\xc0\x00",
                         b"\x01a\xc0\x0c", b"\x40" + b"a" * 64 + b"\0", b"\x80"):
            malformed.append(packet[:12] + bad_name + b"\x00\x01\x00\x01")
        for bad_opt in (b"\0", OPT[:-1], OPT + b"x", b"\x01x" + OPT,
                        bytes.fromhex("00 0001 0200 00000000 0000"),
                        bytes.fromhex("00 0029 0200 00000000 0003 000100"),
                        bytes.fromhex("00 0029 0200 00000000 0004 fde90001")):
            malformed.append(query(opt=bad_opt)[0])
        malformed.append(struct.pack("!6H", 0x5A71, 0x100, 1, 0, 0, 2) + question + OPT * 2)
        with running() as (server, _):
            for invalid in malformed:
                with self.subTest(packet=invalid.hex()):
                    self.expect(server, invalid, b"", 0x8101)
            for ignored in (b"", b"x" * 11, query(flags=0x8100)[0]):
                self.assertIsNone(exchange(server, ignored, timeout=0.06))
            self.expect(server, packet, question, 0x8500, (A,))

    def test_delay_keeps_receiving_and_preserves_ids(self):
        with running(mode="delay", seconds=0.15) as (server, _):
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
                client.settimeout(1)
                packet, question = query(ident=1)
                started = time.monotonic()
                client.sendto(packet, server.address)
                time.sleep(0.04)
                second, _ = query("next.fixture.test", ident=2)
                client.sendto(second, server.address)
                wait_for(lambda: server.requests == 2)
                self.assertEqual(server.replies, 0)
                first_reply = client.recvfrom(1024)[0]
                elapsed = time.monotonic() - started
                self.assertGreaterEqual(elapsed, 0.14)
                self.assertEqual(first_reply, bytes.fromhex("0001 8500 0001 0001 0000 0000") + question + A)
                self.assertEqual(client.recvfrom(1024)[0][:2], b"\0\x02")

    def test_drop_and_delay_only_apply_to_test_zone(self):
        for mode in ("drop", "delay"):
            with self.subTest(mode=mode), running(mode=mode, seconds=25) as (server, _):
                packet, _ = query()
                self.assertIsNone(exchange(server, packet, timeout=0.08))
                outside, question = query("other.test")
                self.expect(server, outside, question, 0x8105)
                if mode == "drop":
                    self.assertEqual(server.dropped, 1)
                    self.assertEqual(server.pending, [])

    def test_delay_queue_is_bounded_and_shutdown_discards_it(self):
        started = time.monotonic()
        with running(mode="delay", seconds=60) as (server, logs):
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
                for ident in range(67):
                    client.sendto(query(ident=ident)[0], server.address)
                wait_for(lambda: server.requests == 67)
                self.assertEqual(len(server.pending), 64)
                self.assertEqual(server.dropped, 3)
                self.assertEqual(server.replies, 0)
                self.assertEqual(sum("queue full" in line for line in logs), 3)
        self.assertLess(time.monotonic() - started, 3)

    def test_closed_client_does_not_stop_future_requests(self):
        with running(mode="delay", seconds=0.06) as (server, _):
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
                client.sendto(query()[0], server.address)
            wait_for(lambda: server.requests == 1)
            time.sleep(0.15)
            packet, question = query("next.fixture.test", ident=19)
            self.expect(server, packet, question, 0x8500, (A,))

    def test_logs_omit_query_names_and_payload(self):
        with running() as (server, logs):
            packet, question = query("DNS_QUERY_SECRET.fixture.test")
            self.expect(server, packet, question, 0x8500, (A,))
            self.assertIsNone(exchange(server, b"DNS_PAYLOAD_SECRET" * 32, timeout=0.06))
            wait_for(lambda: server.requests == 2)
            output = "\n".join(logs)
            for value in ("DNS_QUERY_SECRET", "DNS_PAYLOAD_SECRET", "fixture.test", "127.0.0.1"):
                self.assertNotIn(value, output)
            self.assertIn("type=1 in-zone=1 rcode=0", output)

    def test_dns_answer_reaches_http_peer_with_original_host(self):
        with running(mode="delay", seconds=0.04) as (server, _):
            packet, question = query("DNS_QUERY_SECRET.fixture.test")
            response = exchange(server, packet)
            self.assertEqual(response, bytes.fromhex("5a71 8500 0001 0001 0000 0000") + question + A)
            address = socket.inet_ntoa(response[-4:])
            output = io.StringIO()
            with redirect_stdout(output), http_fixture.HTTPServer(("127.0.0.1", 0), http_fixture.FixtureHandler) as peer:
                peer.options = SimpleNamespace(mode="echo", seconds=0.06, interval=0.02, reply_bytes=8192)
                peer.requests = 0
                peer.timeout = 1
                worker = threading.Thread(target=peer.handle_request)
                worker.start()
                client = http.client.HTTPConnection(address, peer.server_port, timeout=1)
                host = f"DNS_QUERY_SECRET.fixture.test:{peer.server_port}"
                try:
                    client.request("GET", "/?DNS_PATH_SECRET", headers={"Host": host})
                    result = client.getresponse()
                    self.assertEqual(result.status, 200)
                    body = json.loads(result.read())
                    self.assertEqual(body["headers"]["Host"], host)
                    self.assertEqual(body["path"], "/?DNS_PATH_SECRET")
                finally:
                    client.close()
                    worker.join(2)
                    self.assertFalse(worker.is_alive())
                self.assertEqual(peer.requests, 1)
            self.assertNotIn("DNS_QUERY_SECRET", output.getvalue())
            self.assertNotIn("DNS_PATH_SECRET", output.getvalue())

    def test_cli_rejects_invalid_configuration_before_binding(self):
        for option, value in (("--zone", "example.com"), ("--zone", "fixture.test.."),
                              ("--zone", "test"), ("--zone", "bad_name.test"),
                              ("--bind", "localhost"), ("--bind", "::1"),
                              ("--answer", "999.1.2.3"), ("--answer-v6", "127.0.0.1"),
                              ("--seconds", "nan"), ("--seconds", "inf"),
                              ("--seconds", "-1"), ("--seconds", "61"),
                              ("--ttl", "-1"), ("--ttl", "86401"),
                              ("--port", "-1"), ("--port", "65536")):
            with self.subTest(option=option, value=value):
                result = subprocess.run([sys.executable, str(ROOT / "tools/dns_test_server.py"), option, value],
                                        capture_output=True, text=True, timeout=3)
                self.assertEqual(result.returncode, 2)
                self.assertIn("error:", result.stderr)
                self.assertNotIn("Traceback", result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
