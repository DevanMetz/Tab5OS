"""Exercise NTP/WoL/UDP Console workers against real UDP, exclusively on loopback.

SDK shims supply native locks, threads, clocks and socket calls. No datagram is
forwarded, no physical wake is attempted, and no tablet clock is changed.
"""
import argparse
import importlib.util
import json
import pathlib
import re
import socket
import struct
import subprocess
import threading
import time
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("fixture", ROOT / "tools/udp_device_fixture.py")
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)


def png_from_ppm(path):
    with path.open("rb") as image:
        assert image.readline() == b"P6\n"
        width, height = map(int, image.readline().split())
        assert image.readline() == b"255\n"
        pixels = image.read()
    assert len(pixels) == width * height * 3

    def chunk(kind, payload):
        return struct.pack("!I", len(payload)) + kind + payload + struct.pack("!I", zlib.crc32(kind + payload))

    scanlines = b"".join(b"\0" + pixels[row * width * 3:(row + 1) * width * 3] for row in range(height))
    path.with_suffix(".png").write_bytes(
        b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack("!IIBBBBB", width, height, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(scanlines)) + chunk(b"IEND", b""))


def run_case(executable, output, mode):
    peer = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    peer.bind(("127.0.0.1", 0))
    peer.settimeout(0.1)
    port = peer.getsockname()[1]
    stop = threading.Event()
    requests, times, sources, errors = [], [], [], []

    def serve():
        while not stop.is_set():
            try:
                request, source = peer.recvfrom(65535)
            except socket.timeout:
                continue
            except OSError:
                if not stop.is_set():
                    errors.append("UDP receive failed")
                break
            try:
                assert source[0] == "127.0.0.1", source
                requests.append(request)
                times.append(time.monotonic())
                sources.append(source)
                if mode.startswith("wol-"):
                    assert fixture.wol_mac(request) == "02:11:22:33:44:55"
                    continue
                if mode.startswith("udp-"):
                    expected_request = b"Hi\n\\n" if mode == "udp-ascii" else b"" if mode == "udp-empty" else (
                        bytes(range(128)) if mode == "udp-max" else bytes(range(4)))
                    if mode == "udp-clipboard":
                        expected_request = b"\x00\x00\xc0\x3f" if len(requests) == 1 else bytes(range(128))
                        if stop.wait(0.15):  # Leave time to inspect controls while busy.
                            break
                    assert request == expected_request, (request, expected_request)
                    if mode in ("udp-timeout", "udp-offline", "udp-queued-remaining") or (
                        mode in ("udp-stop", "udp-home") and len(requests) == 1
                    ):
                        continue
                    if mode in ("udp-wrong-peer", "udp-wrong-only"):
                        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as wrong:
                            wrong.bind(("127.0.0.1", 0))
                            wrong.sendto(b"wrong peer", source)
                        if mode == "udp-wrong-only":
                            continue
                        if stop.wait(0.08):
                            break
                        reply = b"good"
                    else:
                        count = {"udp-max": 512, "udp-over-limit": 513, "udp-truncated": 2048}.get(mode)
                        reply = fixture.udp_reply(request, count)
                    peer.sendto(reply, source)
                    continue
                assert len(request) == 48 and request[0] == 0x23
                assert request[1] == 0 and request[24:40] == bytes(16)
                assert request[40:48] != bytes(8)
                number = len(requests)
                if mode in ("ntp-timeout", "ntp-queued-remaining") or (mode in ("ntp-stop", "ntp-home") and number == 1):
                    continue
                received = time.time() + 0.1
                if mode == "ntp-clockstep" and stop.wait(0.4):
                    break
                outcome = "ok"
                if mode in ("ntp-kod", "ntp-queued-expired", "ntp-queued-retry") or (mode in ("ntp-stop", "ntp-home") and number == 2):
                    outcome = "kod"
                if mode == "ntp-invalid":
                    outcome = ["mismatch", "unsynced", "short", "ok"][number - 1]
                reply = fixture.ntp_reply(request, received, time.time() + 0.1, outcome)
                if mode == "ntp-invalid" and number == 4:
                    reply += b"x"  # Extra byte must not become an accepted 48-byte reply.
                if mode == "ntp-unmatched":
                    peer.sendto(fixture.ntp_reply(request, received, time.time() + 0.1, "mismatch"), source)
                for _ in range(16 if mode == "ntp-invalid" and number == 1 else 1):
                    peer.sendto(reply, source)
            except BaseException as error:
                errors.append(repr(error))

    worker = threading.Thread(target=serve, daemon=True)
    worker.start()
    try:
        result = subprocess.run([str(executable), str(port), mode, str(output)], capture_output=True,
                                text=True, timeout=35, creationflags=subprocess.CREATE_NO_WINDOW)
        # Catch accidental extra packets after the app believes it has finished.
        time.sleep(0.15)
    finally:
        stop.set()
        worker.join(1)
        peer.close()
        assert not worker.is_alive()
    evidence = {"scenario": mode, "exitCode": result.returncode, "requests": [request.hex() for request in requests],
                "requestGapsSeconds": [b - a for a, b in zip(times, times[1:])], "serverErrors": errors,
                "stdout": result.stdout, "stderr": result.stderr, "loopbackOnly": True,
                "workerStartAndMonotonicClockControlled": "-queued-" in mode,
                "actualSdkSchedulerOrPhysicalWifiVerified": False}
    (output / f"{mode}-wire.json").write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    print(result.stdout, end="", flush=True)
    if result.stderr:
        print(result.stderr, end="", flush=True)
    assert result.returncode == 0, (mode, result.returncode)
    assert not errors, (mode, errors)
    expected = 1
    if "-queued-" in mode:
        expected = 26 if mode.endswith("-retry") else (4 if mode.startswith("ntp-") else 1) if mode.endswith("-ready") else 1 if mode.endswith("-remaining") else 0
    elif mode.endswith("-ui") or mode in ("ntp-gates", "ntp-inputs", "wol-invalid", "udp-gates", "udp-invalid", "udp-bind"):
        expected = 0
    elif mode.endswith("-repeated"):
        expected = 25
    elif mode in ("ntp-valid", "ntp-unmatched", "ntp-invalid", "ntp-timeout"):
        expected = 4
    elif mode in ("ntp-stop", "ntp-home", "udp-stop", "udp-home", "udp-clipboard"):
        expected = 2
    assert len(requests) == expected, (mode, len(requests), expected)
    if mode == "udp-source":
        fixed_port = int(re.search(r"fixed_port=(\d+)", result.stdout).group(1))
        assert sources[0][1] == fixed_port, (sources, fixed_port)
    if mode in ("ntp-valid", "ntp-unmatched", "ntp-invalid", "ntp-timeout", "ntp-queued-ready"):
        gaps = [b - a for a, b in zip(times, times[1:])]
        assert all(gap >= 1.99 for gap in gaps), gaps  # 10ms scheduler/capture tolerance.
        assert len(set(sources)) == 1, "One socket should serve the four-sample burst"
    if mode.startswith("ntp-"):
        assert len({request[40:48] for request in requests}) == len(requests), "Repeated NTP origin token"
    message = f"fixture {mode}: {len(requests)} exact UDP requests; loopback peer closed"
    print(message, flush=True)
    return result.stdout + result.stderr + message + "\n"


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=pathlib.Path, default=ROOT / "build/device-network/out/device_test.exe")
    parser.add_argument("--output", type=pathlib.Path, default=ROOT / "build/device-network")
    parser.add_argument("modes", nargs="*")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    modes = args.modes or [
        "wol-ui", "wol-send", "wol-gates", "wol-offline", "wol-invalid", "wol-repeated",
        "ntp-ui", "ntp-inputs", "ntp-gates", "ntp-valid", "ntp-unmatched", "ntp-invalid", "ntp-kod",
        "ntp-timeout", "ntp-stop", "ntp-home", "ntp-clockstep", "ntp-clockgap", "ntp-repeated",
        "udp-ui", "udp-invalid", "udp-gates", "udp-echo", "udp-clipboard", "udp-ascii", "udp-empty", "udp-max",
        "udp-over-limit", "udp-truncated", "udp-wrong-peer", "udp-wrong-only", "udp-timeout",
        "udp-stop", "udp-home", "udp-offline", "udp-bind", "udp-source", "udp-task-failure", "udp-repeated"]
    if not args.modes:
        modes += [f"{app}-queued-{action}" for app in ("ntp", "wol", "udp")
                  for action in ("expired", "ready", "cancel", "home", "retry")]
        modes += ["ntp-queued-remaining", "udp-queued-remaining"]
    log = "".join(run_case(args.executable, args.output, mode) for mode in modes)
    for path in args.output.glob("*.ppm"):
        png_from_ppm(path)
    conclusion = f"All {len(modes)} real UDP/LVGL scenarios passed. Physical Wi-Fi/lwIP/wake behavior remains a hardware check.\n"
    (args.output / "results.txt").write_text(log + conclusion, encoding="utf-8")
    print(conclusion, end="", flush=True)
