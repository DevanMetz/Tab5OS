"""Local test peers for NTP Lab, Wake-on-LAN and UDP Console; no dependencies.

Defaults to loopback. Bind explicitly to a PC LAN address for tablet tests.
The NTP peer uses the PC wall clock and is a fixture, not a trusted time source.
The WoL peer only receives/inspects packets; it never forwards wake requests.
"""

import argparse
import math
import socket
import struct
import time

NTP_EPOCH = 2_208_988_800


def timestamp(unix_seconds):
    seconds = math.floor(unix_seconds)
    fraction = int((unix_seconds - seconds) * (1 << 32))
    return struct.pack("!II", (seconds + NTP_EPOCH) & 0xFFFFFFFF, fraction)


def ntp_reply(request, received, transmitted, outcome="ok"):
    if len(request) != 48 or request[0] & 7 != 3 or (request[0] >> 3) & 7 not in (3, 4):
        raise ValueError("Expected a 48-byte NTP v3/v4 client request")
    reply = bytearray(48)
    reply[0] = (4 << 3) | 4
    reply[1] = 2
    reply[2] = 4
    reply[3] = 0xEC  # precision -20
    reply[12:16] = b"TEST"
    reply[16:24] = timestamp(received - 1)
    reply[24:32] = request[40:48]
    reply[32:40] = timestamp(received)
    reply[40:48] = timestamp(transmitted)
    if outcome == "kod":
        reply[1] = 0
        reply[12:16] = b"RATE"
    elif outcome == "unsynced":
        reply[0] |= 3 << 6
    elif outcome == "mismatch":
        reply[24] ^= 1
    elif outcome == "short":
        return bytes(reply[:24])
    return bytes(reply)


def wol_mac(packet):
    if len(packet) != 102 or packet[:6] != b"\xff" * 6 or packet[6:] != packet[6:12] * 16:
        raise ValueError("Expected exactly 102 bytes: six FF bytes and sixteen identical MAC copies")
    return ":".join(f"{value:02X}" for value in packet[6:12])


def udp_reply(request, reply_bytes=None):
    if len(request) > 128:
        raise ValueError("UDP Console fixture accepts at most 128 request bytes")
    if reply_bytes is None:
        return bytes(request)
    if not 0 <= reply_bytes <= 4096:
        raise ValueError("Synthetic reply size must be 0 to 4096")
    return (bytes(range(256)) * 16)[:reply_bytes]


def self_test():
    request = bytearray(48)
    request[0] = 0x23
    request[40:48] = timestamp(1_800_000_000.125)
    reply = ntp_reply(request, 1_800_000_000.25, 1_800_000_000.375)
    assert len(reply) == 48 and reply[0] == 0x24 and reply[1] == 2
    assert reply[24:32] == request[40:48]
    assert reply[32:40] == timestamp(1_800_000_000.25)
    assert ntp_reply(request, 0, 0, "kod")[12:16] == b"RATE"
    assert ntp_reply(request, 0, 0, "unsynced")[0] >> 6 == 3
    assert len(ntp_reply(request, 0, 0, "short")) == 24
    assert timestamp(2_085_978_496) == bytes(8)  # NTP era rollover in 2036
    mac = bytes.fromhex("021122334455")
    packet = b"\xff" * 6 + mac * 16
    assert wol_mac(packet) == "02:11:22:33:44:55"
    for invalid in (b"", packet[:-1], packet + b"x", b"\0" + packet[1:]):
        try:
            wol_mac(invalid)
        except ValueError:
            pass
        else:
            raise AssertionError("Malformed magic packet accepted")
    assert udp_reply(b"Hi\0\xff") == b"Hi\0\xff"
    assert udp_reply(b"") == b""
    assert udp_reply(bytes(range(128)), 512) == bytes(range(256)) * 2
    assert len(udp_reply(b"x", 513)) == 513
    assert udp_reply(b"x", 0) == b""
    for request, count in ((bytes(129), None), (b"x", -1), (b"x", 4097)):
        try:
            udp_reply(request, count)
        except ValueError:
            pass
        else:
            raise AssertionError("Invalid UDP fixture input accepted")
    print("UDP device fixture self-test passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("ntp", "wol", "udp"), nargs="?")
    parser.add_argument("--bind", default="127.0.0.1", help="Explicit PC IPv4 address; default loopback")
    parser.add_argument("--port", type=int, help="Default 19123 for NTP, 19009 for WoL, 19007 for UDP")
    parser.add_argument("--offset-ms", type=float, default=100, help="Fixture NTP offset from PC clock")
    parser.add_argument("--delay", type=float, default=0, help="NTP/UDP reply delay in seconds, 0 to 10")
    parser.add_argument("--reply-bytes", type=int, help="UDP: return 0-4096 synthetic bytes instead of echoing")
    parser.add_argument("--outcome", choices=("ok", "kod", "unsynced", "mismatch", "short", "silent"), default="ok")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    if not args.mode:
        parser.error("Choose ntp, wol or udp, or --self-test")
    port = args.port if args.port is not None else {"ntp": 19123, "wol": 19009, "udp": 19007}[args.mode]
    if not 1 <= port <= 65535:
        parser.error("Port must be 1 to 65535")
    if not math.isfinite(args.offset_ms) or abs(args.offset_ms) > 86_400_000:
        parser.error("Offset must be finite and within one day")
    if not math.isfinite(args.delay) or not 0 <= args.delay <= 10:
        parser.error("Delay must be 0 to 10 seconds")
    if args.reply_bytes is not None and (args.mode != "udp" or not 0 <= args.reply_bytes <= 4096):
        parser.error("--reply-bytes requires UDP mode and a size from 0 to 4096")
    if args.mode == "udp" and args.outcome not in ("ok", "silent"):
        parser.error("UDP --outcome must be ok or silent")
    socket.inet_pton(socket.AF_INET, args.bind)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as peer:
        peer.bind((args.bind, port))
        peer.settimeout(0.5)
        print(f"{args.mode.upper()} fixture listening on {args.bind}:{port}; Ctrl+C stops", flush=True)
        while True:
            try:
                # Windows raises WSAEMSGSIZE for an undersized datagram buffer.
                # Bound by the maximum UDP size, then validate the full payload.
                packet, source = peer.recvfrom(65535)
            except socket.timeout:
                continue
            received = time.time() + args.offset_ms / 1000
            try:
                if args.mode == "wol":
                    print(f"Received valid magic packet for {wol_mac(packet)} from {source[0]}:{source[1]}; no packet forwarded", flush=True)
                else:
                    if args.outcome == "silent":
                        print("Request received; reply suppressed", flush=True)
                        continue
                    if args.delay:
                        time.sleep(args.delay)
                    reply = (udp_reply(packet, args.reply_bytes) if args.mode == "udp" else
                             ntp_reply(packet, received, time.time() + args.offset_ms / 1000, args.outcome))
                    peer.sendto(reply, source)
                    print(f"{args.mode.upper()} {args.outcome}: {len(packet)} RX / {len(reply)} TX bytes, peer {source[0]}:{source[1]}", flush=True)
            except ValueError as error:
                print(f"Rejected packet: {error}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nFixture stopped")
