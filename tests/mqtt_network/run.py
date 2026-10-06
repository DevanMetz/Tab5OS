"""Independent loopback broker for the pinned ESP-MQTT client, not a broker product.

Packet assertions follow MQTT 3.1.1's wire format (OASIS sections 2.2, 3.1,
3.3-3.7, 3.8-3.9 and 3.14). Python builds/parses bytes without SDK helpers.
Only fixture credentials and payloads enter the transcript. Never use real
broker credentials here. All peers bind/connect to 127.0.0.1.
"""
import argparse
import csv
import hashlib
import json
import socket
import subprocess
import threading
import time
from pathlib import Path

MODES = (
    "connect", "connect-rejected", "subscribe-rejected",
    "publish-qos0", "publish-qos1", "publish-qos2", "publish-empty",
    "receive-empty", "receive-qos0", "receive-qos1", "receive-qos2",
    "receive-zero-start", "receive-fragments", "invalid-header", "truncated-publish", "lifecycle",
)
APP_MODES = (
    "app-console", "app-qos0", "app-qos2", "app-zero-start", "app-fragments",
    "app-home", "app-abandoned", "app-refused", "app-subscribe-rejected", "app-soak",
    "app-early-subscribe", "app-early-subscribe-rejected", "app-early-publish-qos1", "app-early-publish-qos2",
    "app-log-sync-home", "app-log-close-home", "app-log-overflow", "app-log-open-error",
    "app-log-flush-error", "app-log-sync-error", "app-log-close-error", "app-log-active-error", "app-log-home-error",
    "app-copy-empty", "app-copy-binary", "app-copy-fragments", "app-copy-oversize",
    "app-paste-qos0", "app-paste-qos1", "app-paste-qos2", "app-paste-empty",
)
APP_ERROR_MODES = (
    "app-log-open-error", "app-log-flush-error", "app-log-sync-error", "app-log-close-error",
    "app-log-active-error", "app-log-home-error",
)


def remaining_length(length):
    assert 0 <= length <= 268435455
    encoded = bytearray()
    while True:
        byte, length = length % 128, length // 128
        encoded.append(byte | (0x80 if length else 0))
        if not length:
            return bytes(encoded)


def packet(header, body=b""):
    return bytes([header]) + remaining_length(len(body)) + body


def field(data):
    return len(data).to_bytes(2, "big") + data


def publish(topic, payload, qos=0, retain=False, dup=False, msg_id=0x1234):
    body = field(topic) + (msg_id.to_bytes(2, "big") if qos else b"") + payload
    return packet(0x30 | qos << 1 | int(retain) | int(dup) << 3, body)


def read_exact(peer, length):
    result = bytearray()
    while len(result) < length:
        part = peer.recv(length - len(result))
        if not part:
            raise EOFError(f"Peer closed with {length - len(result)} bytes missing")
        result.extend(part)
    return bytes(result)


def expect_closed(peer):
    # Closing with unread malformed input may reset TCP instead of sending FIN.
    try:
        assert peer.recv(1) == b"", "Unexpected packet after a rejected/incomplete message"
    except ConnectionResetError:
        pass


class Broker:
    def __init__(self, mode):
        self.mode = mode
        self.cycles = 25 if mode == "lifecycle" else 1
        self.error = None
        self.transcript = []
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.listener.settimeout(8)
        self.port = self.listener.getsockname()[1]
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.peer = None

    def record(self, direction, wire):
        self.transcript.append({"cycle": self.cycle, "direction": direction,
                                "wire_hex": wire.hex(), "sha256": hashlib.sha256(wire).hexdigest()})

    def read(self, peer):
        header = read_exact(peer, 1)
        encoded = bytearray()
        length = 0
        for shift in (0, 7, 14, 21):
            byte = read_exact(peer, 1)[0]
            encoded.append(byte)
            length |= (byte & 127) << shift
            if not byte & 128:
                break
        else:
            raise AssertionError("More than four remaining-length bytes")
        assert length <= 8192, f"Unexpected fixture packet size: {length}"
        body = read_exact(peer, length)
        self.record("client-to-broker", header + encoded + body)
        return header[0], body

    def send(self, peer, wire):
        self.record("broker-to-client", wire)
        peer.sendall(wire)

    def expect_ack(self, peer, header, msg_id):
        actual_header, body = self.read(peer)
        assert (actual_header, body) == (header, msg_id.to_bytes(2, "big")), (actual_header, body)

    def expect_connect(self, peer):
        header, body = self.read(peer)
        expected = b"\x00\x04MQTT\x04\xc2\x00\x3c" + field(b"tab5-native") + field(b"fixture-u") + field(b"fixture-p")
        assert header == 0x10 and body == expected, (header, body)

    def expect_subscribe(self, peer, reject):
        header, body = self.read(peer)
        assert header == 0x82 and len(body) >= 2 and body[2:] == field(b"site/+/state") + b"\x02", (header, body)
        msg_id = int.from_bytes(body[:2], "big")
        assert msg_id > 0
        self.send(peer, packet(0x90, body[:2] + (b"\x80" if reject else b"\x02")))

    def expect_publish(self, peer, qos, empty):
        header, body = self.read(peer)
        assert header == 0x31 | qos << 1, (header, qos)
        topic_length = int.from_bytes(body[:2], "big")
        assert topic_length == 10 and body[2:12] == b"site/state"
        msg_id = int.from_bytes(body[12:14], "big") if qos else 0
        payload = body[14:] if qos else body[12:]
        assert payload == (b"" if empty else "µZ".encode()), payload
        if qos:
            assert msg_id > 0
            self.send(peer, packet(0x40 if qos == 1 else 0x50, msg_id.to_bytes(2, "big")))
            if qos == 2:
                self.expect_ack(peer, 0x62, msg_id)
                self.send(peer, packet(0x70, msg_id.to_bytes(2, "big")))
        else:
            self.send(peer, publish(b"fixture/done", b"ok"))

    def send_publish(self, peer):
        large_topic = self.mode == "receive-zero-start"
        fragmented = self.mode in ("receive-fragments", "lifecycle")
        qos = 2 if large_topic or fragmented or self.mode == "receive-qos2" else 1 if self.mode == "receive-qos1" else 0
        topic = b"t" * 1017 if large_topic else b"site/state"
        payload = b"k" * 17 if large_topic else bytes(range(256)) * 4 if fragmented else b"" if self.mode == "receive-empty" else b"\x00\x80Z"
        self.send(peer, publish(topic, payload, qos, retain=True, dup=qos == 2))
        if qos:
            self.expect_ack(peer, 0x40 if qos == 1 else 0x50, 0x1234)
            if qos == 2:
                self.send(peer, packet(0x62, b"\x12\x34"))
                self.expect_ack(peer, 0x70, 0x1234)
        # This marker proves the SDK read and acknowledged the entire message.
        self.send(peer, publish(b"fixture/done", b"ok"))

    def session(self, peer):
        self.expect_connect(peer)
        self.send(peer, b"\x20\x02\x00" + (b"\x05" if self.mode == "connect-rejected" else b"\x00"))
        if self.mode == "connect-rejected":
            expect_closed(peer)
            return
        if self.mode in ("subscribe-rejected", "lifecycle"):
            self.expect_subscribe(peer, self.mode == "subscribe-rejected")
        if self.mode.startswith("publish-") or self.mode == "lifecycle":
            qos = 0 if self.mode in ("publish-qos0", "publish-empty") else 2 if self.mode == "publish-qos2" else 1
            self.expect_publish(peer, qos, self.mode == "publish-empty")
        if self.mode.startswith("receive-") or self.mode == "lifecycle":
            self.send_publish(peer)
        elif self.mode == "invalid-header":
            self.send(peer, b"\x36\x00")  # Reserved PUBLISH QoS=3.
            expect_closed(peer)
            return
        elif self.mode == "truncated-publish":
            wire = publish(b"site/state", bytes(range(256)) * 8, 2, retain=True, dup=True)
            self.send(peer, wire[:1024])
            peer.shutdown(socket.SHUT_WR)
            expect_closed(peer)  # Incomplete delivery must not be acknowledged.
            return
        header, body = self.read(peer)
        assert (header, body) == (0xE0, b""), (header, body)

    def run(self):
        try:
            for self.cycle in range(self.cycles):
                peer, address = self.listener.accept()
                assert address[0] == "127.0.0.1"
                self.peer = peer
                with peer:
                    peer.settimeout(6)
                    peer.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                    self.session(peer)
                self.peer = None
        except BaseException as error:
            self.error = error
        finally:
            self.listener.close()

    def finish(self):
        self.thread.join(10)
        if self.thread.is_alive():
            self.listener.close()
            if self.peer:
                self.peer.close()
            self.thread.join(1)
            raise AssertionError("Loopback broker did not finish")
        if self.error:
            raise self.error


class AppBroker(Broker):
    """Peer for the actual console. File signals coordinate observable app phases."""
    def __init__(self, mode, output):
        super().__init__(mode)
        self.cycles = 25 if mode == "app-soak" else 2 if mode in APP_ERROR_MODES else 1
        self.output = output
        self.home_flag = output / f"{mode}-home.flag"
        self.home_flag.unlink(missing_ok=True)
        self.ack_flag = output / f"{mode}-ack.flag"
        self.ack_flag.unlink(missing_ok=True)
        self.copy_flag = output / f"{mode}-copy.flag"
        self.copy_flag.unlink(missing_ok=True)
        self.oversize_flag = output / f"{mode}-oversize.flag"
        self.oversize_flag.unlink(missing_ok=True)
        for phase in ("storage", "callback", "fault"):
            for cycle in range(self.cycles):
                (output / f"{mode}-{phase}-{cycle}.flag").unlink(missing_ok=True)

    @staticmethod
    def wait_phase(path):
        until = time.monotonic() + 5
        while not path.is_file():
            assert time.monotonic() < until, f"App phase did not complete: {path.name}"
            time.sleep(0.002)

    def session(self, peer):
        header, body = self.read(peer)
        expected = b"\x00\x04MQTT\x04\xc2\x00\x1e" + field(b"tab5os-010203") + field(b"FIXTURE_APP_USER_SECRET") + field(b"FIXTURE_APP_PASSWORD_SECRET")
        assert (header, body) == (0x10, expected), (header, body)
        self.send(peer, b"\x20\x02\x00" + (b"\x05" if self.mode == "app-refused" else b"\x00"))
        if self.mode == "app-refused":
            expect_closed(peer)
            return
        qos = 0 if self.mode in ("app-qos0", "app-paste-qos0") else 2 if "qos2" in self.mode else 1
        header, body = self.read(peer)
        assert header == 0x82 and body[2:] == field(b"site/+/state") + bytes([qos]), (header, body)
        assert int.from_bytes(body[:2], "big") > 0
        grant = 0x80 if "subscribe-rejected" in self.mode else qos
        self.send(peer, packet(0x90, body[:2] + bytes([grant])))
        header, body = self.read(peer)
        assert header == 0x31 | qos << 1 and body[2:12] == b"site/state" and body[:2] == b"\x00\x0a", (header, body)
        payload = body[14:] if qos else body[12:]
        pasted = self.mode.startswith("app-paste-")
        prepared = b"" if self.mode == "app-paste-empty" else bytes((index * 37) % 256 for index in range(128))
        assert payload == (prepared if pasted else "µFIXTURE_APP_PAYLOAD_SECRET".encode()), payload
        if qos:
            msg_id = int.from_bytes(body[12:14], "big")
            assert msg_id > 0
            self.send(peer, packet(0x40 if qos == 1 else 0x50, msg_id.to_bytes(2, "big")))
            if qos == 2:
                self.expect_ack(peer, 0x62, msg_id)
                self.send(peer, packet(0x70, msg_id.to_bytes(2, "big")))
        if self.mode.startswith("app-early-publish"):
            self.wait_phase(self.ack_flag)

        if self.mode.startswith("app-log-") and self.cycle == 0:
            phase = "storage" if self.mode in ("app-log-sync-home", "app-log-close-home", "app-log-overflow", "app-log-home-error") else "callback" if self.mode == "app-log-active-error" else "fault"
            self.wait_phase(self.output / f"{self.mode}-{phase}-0.flag")
            if self.mode == "app-log-overflow":
                for index in range(20):
                    # Each actual PUBACK confirms that the app received the whole message.
                    payload = f"FIXTURE_BURST_PAYLOAD_SECRET_{index:02}".encode()
                    self.send(peer, publish(f"burst/{index:02}".encode(), payload, 1, retain=True, msg_id=0x1200 + index))
                    self.expect_ack(peer, 0x40, 0x1200 + index)
                self.send(peer, publish(b"fixture/done", b"ok"))
                header, body = self.read(peer)
                assert (header, body) == (0xE0, b""), (header, body)  # The pending second publish must be cancelled.
                return

        if self.mode in ("app-home", "app-abandoned"):
            wire = publish(b"site/state", b"x" * 2048, retain=True)
            self.send(peer, wire[:1024])
            self.wait_phase(self.home_flag)
            if self.mode == "app-home":
                self.send(peer, wire[1024:])
                header, body = self.read(peer)
                assert (header, body) == (0xE0, b""), (header, body)
            else:
                peer.shutdown(socket.SHUT_WR)
                expect_closed(peer)
            return

        large_topic = self.mode in ("app-zero-start", "app-copy-fragments")
        fragmented = self.mode in ("app-fragments", "app-copy-fragments")
        receive_qos = qos if pasted else 0 if self.mode == "app-qos0" else 2 if large_topic or fragmented or qos == 2 else 1
        topic = b"t" * 1017 if large_topic else b"site/state"
        payload = (prepared if pasted else bytes((index * 37) % 256 for index in range(128)) if self.mode == "app-copy-fragments" else
                   b"" if self.mode == "app-copy-empty" else
                   b"k" * 17 if large_topic else b"z" * 1024 if fragmented else b"" if receive_qos == 0 else b"A\x00\r\n\t\x7f\xff\x80Z")
        self.send(peer, publish(topic, payload, receive_qos, retain=True, dup=receive_qos == 2))
        if receive_qos:
            self.expect_ack(peer, 0x40 if receive_qos == 1 else 0x50, 0x1234)
            if receive_qos == 2:
                self.send(peer, packet(0x62, b"\x12\x34"))
                self.expect_ack(peer, 0x70, 0x1234)
        if self.mode.startswith(("app-copy-", "app-paste-")):
            self.wait_phase(self.copy_flag)
            if self.mode == "app-copy-oversize":
                self.send(peer, publish(b"site/oversize", b"x" * 129, 1, retain=True, msg_id=0x1235))
                self.expect_ack(peer, 0x40, 0x1235)
                self.wait_phase(self.oversize_flag)
        self.send(peer, publish(b"fixture/done", b"ok"))
        header, body = self.read(peer)
        assert (header, body) == (0xE0, b""), (header, body)


def check_app_csv(output, mode, cycles):
    path = output / f"storage-{mode}" / "MQTTLOG.CSV"
    if mode == "app-refused":
        assert not path.exists(), path
        return 0
    text = path.read_text(encoding="utf-8")
    for secret in ("FIXTURE_APP_USER_SECRET", "FIXTURE_APP_PASSWORD_SECRET", "FIXTURE_APP_PAYLOAD_SECRET",
                   "FIXTURE_BURST_PAYLOAD_SECRET", "FIXTURE_CANCELLED_PAYLOAD_SECRET", "kkkkkkkkkkkkkkkkk", "zzzzzzzzzzzzzzzz"):
        assert secret not in text, (mode, secret)
    rows = list(csv.DictReader(text.splitlines()))
    qos = "0" if mode in ("app-qos0", "app-paste-qos0") else "2" if "qos2" in mode else "1"
    tx = [row for row in rows if row["direction"] == "TX"]
    rx = [row for row in rows if row["direction"] == "RX"]
    assert len(tx) == (cycles - 1 if mode == "app-log-open-error" else cycles), (mode, tx)
    assert all(row["topic"] == "site/state" and row["qos"] == qos and row["retained"] == "1" and
               row["payload_bytes"] == ("0" if mode == "app-paste-empty" else "128" if mode.startswith("app-paste-") else "28") and
               row["outcome"] == "queued" for row in tx), (mode, tx)
    if mode == "app-log-overflow":
        assert [row["topic"] for row in rows] == ["site/state"] + [f"burst/{index:02}" for index in range(13, 20)] + ["fixture/done"], rows
        assert all(row["qos"] == "1" and row["retained"] == "1" and row["payload_bytes"] == "31" and
                   row["outcome"] == "received" for row in rx[:-1]), rx
        assert (rx[-1]["qos"], rx[-1]["retained"], rx[-1]["payload_bytes"], rx[-1]["outcome"]) == ("0", "0", "2", "received"), rx
    elif mode == "app-abandoned":
        assert not rx, rx
    elif mode == "app-home":
        assert len(rx) == 1 and rx[0]["topic"] == "site/state" and rx[0]["qos"] == "0" and rx[0]["retained"] == "1" and rx[0]["payload_bytes"] == "2048" and rx[0]["outcome"] == "preview_truncated", rx
    else:
        # Flush/sync/close failures deliberately leave a TX row on the native file,
        # but the UI must report unconfirmed persistence and discard later metadata.
        receive_cycles = 1 if mode in APP_ERROR_MODES else cycles
        assert len(rx) == (3 if mode == "app-copy-oversize" else receive_cycles * 2), (mode, rx)
        markers = [row for row in rx if row["topic"] == "fixture/done"]
        messages = [row for row in rx if row["topic"] not in ("fixture/done", "site/oversize")]
        assert len(markers) == len(messages) == receive_cycles, (mode, rx)
        assert all(row["qos"] == "0" and row["retained"] == "0" and row["payload_bytes"] == "2" and row["outcome"] == "received" for row in markers)
        topic = "t" * 127 if mode in ("app-zero-start", "app-copy-fragments") else "site/state"
        length = "0" if mode == "app-paste-empty" else "128" if mode.startswith("app-paste-") or mode == "app-copy-fragments" else "0" if mode == "app-copy-empty" else "17" if mode == "app-zero-start" else "1024" if mode == "app-fragments" else "0" if mode == "app-qos0" else "9"
        receive_qos = "2" if mode in ("app-zero-start", "app-fragments", "app-qos2", "app-copy-fragments") else qos
        outcome = "preview_truncated" if mode in ("app-zero-start", "app-fragments", "app-copy-fragments") else "received"
        assert all(row["topic"] == topic and row["payload_bytes"] == length and row["qos"] == receive_qos and
                   row["retained"] == "1" and row["outcome"] == outcome for row in messages), (mode, messages)
        if mode == "app-copy-oversize":
            assert [(row["topic"], row["payload_bytes"], row["qos"], row["retained"], row["outcome"]) for row in rx] == [
                ("site/state", "9", "1", "1", "received"), ("site/oversize", "129", "1", "1", "received"),
                ("fixture/done", "2", "0", "0", "received")], rx
        if mode in APP_ERROR_MODES:
            expected = ["TX", "RX", "RX"] if mode == "app-log-open-error" else ["TX", "TX", "RX", "RX"]
            assert [row["direction"] for row in rows] == expected, (mode, rows)
    assert len(rows) == len(tx) + len(rx), (mode, rows)
    return len(rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--app-executable", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("modes", nargs="*", metavar="MODE")
    args = parser.parse_args()
    if any(mode not in MODES + APP_MODES for mode in args.modes):
        parser.error(f"Choose modes from: {', '.join(MODES + APP_MODES)}")
    if any(mode in APP_MODES for mode in args.modes) and not args.app_executable:
        parser.error("App modes require --app-executable")
    args.output.mkdir(parents=True, exist_ok=True)
    report = []
    for mode in args.modes or (MODES + APP_MODES if args.app_executable else MODES):
        app = mode in APP_MODES
        broker = AppBroker(mode, args.output) if app else Broker(mode)
        broker.thread.start()
        try:
            command = [str((args.app_executable if app else args.executable).resolve()), str(broker.port), mode]
            if app:
                command.append(str(args.output.resolve()))
            completed = subprocess.run(command,
                                       capture_output=True, text=True, timeout=150 if broker.cycles > 1 else 40)
            log = completed.stdout + completed.stderr
            (args.output / f"{mode}.log").write_text(log, encoding="utf-8")
            (args.output / f"{mode}-wire.json").write_text(json.dumps(broker.transcript, indent=2) + "\n", encoding="utf-8")
            assert completed.returncode == 0, f"{mode}: client exit {completed.returncode}\n{log}"
            assert f"PASS {mode} cycles={broker.cycles}" in log, log
            broker.finish()
            csv_rows = check_app_csv(args.output, mode, broker.cycles) if app else None
        except BaseException as error:
            broker.listener.close()
            if broker.peer:
                broker.peer.close()
            broker.thread.join(1)
            if isinstance(error, subprocess.TimeoutExpired):
                log = "".join(part.decode(errors="replace") if isinstance(part, bytes) else part or ""
                              for part in (error.stdout, error.stderr))
                (args.output / f"{mode}.log").write_text(log, encoding="utf-8")
            (args.output / f"{mode}-wire.json").write_text(json.dumps(broker.transcript, indent=2) + "\n", encoding="utf-8")
            if broker.error:
                print(f"{mode}: broker failure: {broker.error!r}", flush=True)
            raise
        report.append({"mode": mode, "cycles": broker.cycles, "packets": len(broker.transcript), "csvRows": csv_rows, "result": "PASS"})
        print(next(line for line in log.splitlines() if line.startswith(f"PASS {mode} ")), flush=True)
    (args.output / "client-checks.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"PASS {len(report)} pinned MQTT client/broker cases", flush=True)


if __name__ == "__main__":
    main()
