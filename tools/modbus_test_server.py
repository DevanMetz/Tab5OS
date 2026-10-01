#!/usr/bin/env python3
"""Small read-only Modbus TCP fixture for manually checking the Tab5 app."""

import argparse
import socket
import struct
import time


def response(request, exception=0):
    """Serve FC1..4; register N = N, bit N = N modulo 2."""
    if len(request) != 12:
        raise ValueError("expected one 12-byte read request")
    transaction, protocol, length, unit, function, address, quantity = struct.unpack(
        ">HHHBBHH", request
    )
    if protocol != 0 or length != 6:
        raise ValueError("invalid MBAP header")
    error = exception
    if function not in (1, 2, 3, 4):
        error = 1
    elif not 1 <= quantity <= 16:
        error = 3
    elif address + quantity > 65536:
        error = 2
    if error:
        body = bytes((function | 0x80, error))
    elif function in (1, 2):
        values = bytearray((quantity + 7) // 8)
        for index in range(quantity):
            values[index // 8] |= ((address + index) % 2) << (index % 8)
        body = bytes((function, len(values))) + values
    else:
        values = struct.pack(
            ">" + "H" * quantity, *range(address, address + quantity)
        )
        body = bytes((function, len(values))) + values
    return struct.pack(">HHHB", transaction, 0, len(body) + 1, unit) + body


def read_exact(connection, length):
    data = bytearray()
    while len(data) < length:
        block = connection.recv(length - len(data))
        if not block:
            raise EOFError("connection closed")
        data.extend(block)
    return bytes(data)


def self_test():
    def request(function, address, quantity):
        return struct.pack(">HHHBBHH", 0x1234, 0, 6, 1, function, address, quantity)

    assert response(request(3, 10, 2)) == bytes.fromhex("1234 0000 0007 01 03 04 000a 000b")
    assert response(request(4, 65535, 1))[-2:] == b"\xff\xff"
    assert response(request(1, 0, 9))[-2:] == b"\xaa\x00"
    assert response(request(2, 1, 9))[-2:] == b"\x55\x01"
    assert response(request(3, 65535, 2))[-2:] == b"\x83\x02"
    assert response(request(3, 0, 17))[-2:] == b"\x83\x03"
    assert response(request(6, 0, 1))[-2:] == b"\x86\x01"
    assert response(request(3, 0, 1), 6)[-2:] == b"\x83\x06"
    for invalid in (b"", request(3, 0, 1)[:-1], request(3, 0, 1) + b"x"):
        try:
            response(invalid)
        except ValueError:
            pass
        else:
            raise AssertionError("accepted malformed request")
    print("Modbus test server self-test passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", default="127.0.0.1", help="bind address (use your PC's LAN IPv4 for tablet tests)")
    parser.add_argument("--port", type=int, default=1502)
    parser.add_argument("--fragment", type=int, default=0, help="send replies in chunks of this many bytes")
    parser.add_argument("--delay", type=float, default=0, help="seconds to wait before replying (e.g. 6 for timeout testing)")
    parser.add_argument("--exception", type=int, choices=(1, 2, 3, 4, 6, 10, 11), default=0)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    if not 1 <= args.port <= 65535 or not 0 <= args.fragment <= 41 or not 0 <= args.delay <= 60:
        parser.error("port must be 1..65535, fragment 0..41, delay 0..60")
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
        server.bind((args.bind, args.port))
        server.listen(4)
        print(f"Modbus fixture on {args.bind}:{args.port}; registers equal their zero-based address, bits alternate 0/1.", flush=True)
        while True:
            connection, peer = server.accept()
            with connection:
                connection.settimeout(5)
                try:
                    header = read_exact(connection, 6)
                    length = struct.unpack_from(">H", header, 4)[0]
                    if length != 6:
                        raise ValueError("expected read request length 6")
                    request = header + read_exact(connection, length)
                    reply = response(request, args.exception)
                    time.sleep(args.delay)
                    chunk = args.fragment or len(reply)
                    for offset in range(0, len(reply), chunk):
                        connection.sendall(reply[offset:offset + chunk])
                        if args.fragment:
                            time.sleep(0.02)
                    print(f"{peer[0]}: FC{request[7]} replied with {len(reply)} bytes", flush=True)
                except EOFError:
                    print(f"{peer[0]}: disconnected (a TCP-only test sends no request)", flush=True)
                except (OSError, ValueError) as error:
                    print(f"{peer[0]}: {error}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
