"""Controlled UDP DNS peer for tablet HTTP tests; Python standard library only.

Defaults to loopback and answers only a configured .test zone and its children.
No recursion, forwarding, TCP, files, or query-name logging. Explicitly bind to a
PC LAN IPv4 address on port 53 for an isolated test LAN's DHCP DNS setting.
"""

import argparse
import errno
import heapq
import ipaddress
import math
import re
import socket
import struct
import time

MAX_PACKET = 512
MAX_PENDING = 64
MODES = ("answer", "delay", "drop", "nxdomain", "servfail")


def zone_labels(value):
    labels = value.removesuffix(".").lower().split(".")
    if len(labels) < 2 or labels[-1] != "test":
        raise ValueError("Zone must be a name under .test, such as fixture.test")
    if any(not re.fullmatch(r"[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?", part) for part in labels):
        raise ValueError("Zone labels must be 1-63 ASCII letters, digits or interior hyphens")
    if sum(len(part) + 1 for part in labels) + 1 > 255:
        raise ValueError("Encoded zone must be at most 255 bytes")
    return tuple(part.encode("ascii") for part in labels)


def read_name(packet, offset):
    labels, seen, end, size = [], set(), None, 1
    while True:
        if offset >= len(packet) or offset in seen:
            raise ValueError("Truncated or cyclic name")
        seen.add(offset)
        length = packet[offset]
        if length & 0xC0 == 0xC0:
            if offset + 1 >= len(packet):
                raise ValueError("Truncated pointer")
            target = ((length & 0x3F) << 8) | packet[offset + 1]
            if not 12 <= target < offset:
                raise ValueError("Pointer must refer to a prior name")
            if end is None:
                end = offset + 2
            offset = target
        elif length == 0:
            return tuple(labels), end if end is not None else offset + 1
        else:
            if length > 63 or offset + 1 + length > len(packet):
                raise ValueError("Invalid label")
            size += length + 1
            if size > 255:
                raise ValueError("Name exceeds 255 bytes")
            labels.append(packet[offset + 1:offset + 1 + length])
            offset += length + 1


def dns_reply(packet, zone, ipv4, ipv6, mode, ttl):
    """Return (reply, redacted description, apply_mode) for one bounded query."""
    if not 12 <= len(packet) <= MAX_PACKET:
        return None, "ignored-size", False
    ident, flags, questions, answers, authority, additional = struct.unpack_from("!6H", packet)
    if flags & 0x8000:
        return None, "ignored-response", False
    question, records, opt, count = b"", b"", b"", 0
    qtype, in_zone, controlled, aa, code = 0, False, False, False, 1
    try:
        if questions != 1 or answers or authority or additional > 1:
            raise ValueError("Expected one question and at most one OPT")
        labels, end = read_name(packet, 12)
        if end + 4 > len(packet):
            raise ValueError("Truncated question")
        qtype, qclass = struct.unpack_from("!HH", packet, end)
        # Rebuild the name so no incoming pointer can refer to omitted records.
        question = b"".join(bytes((len(part),)) + part for part in labels) + b"\0" + packet[end:end + 4]
        end += 4
        version, opt_flags = 0, 0
        if additional:
            if end + 11 > len(packet) or packet[end] != 0:
                raise ValueError("OPT requires a root name and complete header")
            record_type, _, opt_ttl, data_length = struct.unpack_from("!HHIH", packet, end + 1)
            if record_type != 41 or end + 11 + data_length != len(packet):
                raise ValueError("Invalid OPT record")
            pos, limit = end + 11, len(packet)
            while pos < limit:
                if pos + 4 > limit:
                    raise ValueError("Truncated EDNS option")
                option_length = struct.unpack_from("!H", packet, pos + 2)[0]
                pos += 4 + option_length
                if pos > limit:
                    raise ValueError("Truncated EDNS option data")
            version, opt_flags = (opt_ttl >> 16) & 0xFF, opt_ttl & 0x8000
            end = limit
        if end != len(packet):
            raise ValueError("Unexpected trailing bytes")
        in_zone = len(labels) >= len(zone) and tuple(part.lower() for part in labels[-len(zone):]) == zone
        if version:
            code = 16  # BADVERS uses the extended RCODE in the response OPT.
        elif flags & 0x7800:
            code = 4  # NOTIMP
        elif qclass != 1 or not in_zone:
            code = 5  # REFUSED; the fixture never resolves other namespaces.
        else:
            controlled = True
            code = {"nxdomain": 3, "servfail": 2}.get(mode, 0)
            aa = code in (0, 3)
            if code == 0:
                for kind, address in ((1, ipv4), (28, ipv6)):
                    if address is not None and qtype in (kind, 255):
                        records += b"\xc0\x0c" + struct.pack("!HHIH", kind, 1, ttl, len(address)) + address
                        count += 1
        if additional:
            opt = b"\0" + struct.pack("!HHIH", 41, MAX_PACKET, ((code >> 4) << 24) | opt_flags, 0)
    except ValueError:
        question, records, opt = b"", b"", b""
        code, controlled, aa, count = 1, False, False, 0  # FORMERR, no unvalidated bytes echoed.
    response_flags = 0x8000 | (flags & 0x7910) | (0x0400 if aa else 0) | (code & 15)
    reply = struct.pack("!6H", ident, response_flags, int(bool(question)), count, 0, int(bool(opt)))
    reply += question + records + opt
    return reply, f"type={qtype} in-zone={int(in_zone)} rcode={code}", controlled


class DNSFixture:
    def __init__(self, bind=("127.0.0.1", 15353), *, mode="answer", zone="fixture.test",
                 answer="127.0.0.1", answer_v6=None, seconds=25, ttl=0, logger=None):
        if mode not in MODES:
            raise ValueError("Unsupported mode")
        if not math.isfinite(seconds) or not 0 <= seconds <= 60 or not 0 <= ttl <= 86400:
            raise ValueError("Delay must be finite and 0-60 seconds; TTL must be 0-86400")
        if not 0 <= bind[1] <= 65535:
            raise ValueError("Port must be 0-65535 (0 selects a temporary port)")
        self.zone = zone_labels(zone)
        self.ipv4 = ipaddress.IPv4Address(answer).packed
        self.ipv6 = ipaddress.IPv6Address(answer_v6).packed if answer_v6 is not None else None
        bind_address = str(ipaddress.IPv4Address(bind[0]))
        self.mode, self.seconds, self.ttl = mode, seconds, ttl
        self.logger = logger if logger is not None else lambda line: print(line, flush=True)
        self.requests = self.replies = self.dropped = self.send_errors = 0
        self.pending = []
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            self.socket.bind((bind_address, bind[1]))
        except OSError:
            self.socket.close()
            raise
        self.address = self.socket.getsockname()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.socket.close()

    def send(self, packet, client, request):
        try:
            self.socket.sendto(packet, client)
        except OSError as error:
            self.send_errors += 1
            self.logger(f"Reply to query #{request} failed ({type(error).__name__})")
        else:
            self.replies += 1
            self.logger(f"Reply #{self.replies}: query={request} bytes={len(packet)} pending={len(self.pending)}")

    def serve_forever(self, stop=None):
        try:
            while stop is None or not stop.is_set():
                now = time.monotonic()
                while self.pending and self.pending[0][0] <= now:
                    _, request, packet, client = heapq.heappop(self.pending)
                    self.send(packet, client, request)
                wait = min(0.05, max(0, self.pending[0][0] - now)) if self.pending else 0.05
                self.socket.settimeout(wait)
                try:
                    packet, client = self.socket.recvfrom(MAX_PACKET + 1)
                except socket.timeout:
                    continue
                except ConnectionResetError:
                    # Windows can report ICMP from a client that closed before a delayed reply.
                    self.logger("Closed UDP peer; continuing")
                    continue
                except OSError as error:
                    # Winsock reports a too-large datagram instead of returning a truncated buffer.
                    if error.errno not in (errno.EMSGSIZE, 10040) and getattr(error, "winerror", None) != 10040:
                        raise
                    self.requests += 1
                    self.dropped += 1
                    self.logger(f"Query #{self.requests}: oversized-datagram mode={self.mode}")
                    continue
                self.requests += 1
                reply, detail, controlled = dns_reply(packet, self.zone, self.ipv4, self.ipv6, self.mode, self.ttl)
                self.logger(f"Query #{self.requests}: bytes={len(packet)} {detail} mode={self.mode}")
                if reply is None or (controlled and self.mode == "drop"):
                    self.dropped += 1
                elif controlled and self.mode == "delay":
                    if len(self.pending) == MAX_PENDING:
                        self.dropped += 1
                        self.logger(f"Delay queue full ({MAX_PENDING}); query dropped")
                    else:
                        heapq.heappush(self.pending, (time.monotonic() + self.seconds, self.requests, reply, client))
                else:
                    self.send(reply, client, self.requests)
        finally:
            self.pending.clear()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", default="127.0.0.1", help="IPv4 address to listen on; loopback by default")
    parser.add_argument("--port", type=int, default=15353, help="Use 53 for tablet DNS; 0 selects a temporary port")
    parser.add_argument("--zone", default="fixture.test")
    parser.add_argument("--answer", default="127.0.0.1", help="IPv4 returned for every name in the test zone")
    parser.add_argument("--answer-v6", help="Optional IPv6 AAAA answer; listener still uses IPv4 UDP")
    parser.add_argument("--mode", choices=MODES, default="answer")
    parser.add_argument("--seconds", type=float, default=25, help="Delay before each reply in delay mode (0-60)")
    parser.add_argument("--ttl", type=int, default=0, help="Answer TTL in seconds (0-86400)")
    args = parser.parse_args()
    try:
        server = DNSFixture((args.bind, args.port), mode=args.mode, zone=args.zone, answer=args.answer,
                            answer_v6=args.answer_v6, seconds=args.seconds, ttl=args.ttl)
    except ValueError as error:
        parser.error(str(error))
    with server:
        print(f"DNS fixture on {server.address[0]}:{server.address[1]}, zone={args.zone}, "
              f"answer={args.answer}, mode={args.mode}, delay={args.seconds}s, TTL={args.ttl}; Ctrl+C stops", flush=True)
        server.serve_forever()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("Fixture stopped")
