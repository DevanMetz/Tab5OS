#!/usr/bin/env python3
"""Inspect or update a COLMI R12 directly over Bluetooth LE on Windows."""

import argparse
import asyncio
import hashlib
import struct
from pathlib import Path

from bleak import BleakClient, BleakScanner


ADDRESS = "30:31:48:31:A1:04"
NAME = "COLMI R12_A104"
WRITE_UUID = "de5bf72a-d711-4e47-af26-65e3012a5dc7"
NOTIFY_UUID = "de5bf729-d711-4e47-af26-65e3012a5dc7"
HARDWARE_UUID = "00002a27-0000-1000-8000-00805f9b34fb"
FIRMWARE_UUID = "00002a26-0000-1000-8000-00805f9b34fb"


def crc16(data: bytes) -> int:
    value = 0xFFFF
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = (value >> 1) ^ (0xA001 if value & 1 else 0)
    return value & 0xFFFF


def frame(command: int, payload: bytes = b"") -> bytes:
    checksum = crc16(payload) if payload else 0xFFFF
    return b"\xbc" + bytes([command]) + struct.pack("<HH", len(payload), checksum) + payload


def parse_response(data: bytes) -> tuple[int, int]:
    if len(data) < 7 or data[0] != 0xBC:
        raise RuntimeError(f"invalid OTA response: {data.hex()}")
    size, expected_crc = struct.unpack_from("<HH", data, 2)
    payload = data[6:]
    if size != len(payload) or crc16(payload) != expected_crc:
        raise RuntimeError(f"corrupt OTA response: {data.hex()}")
    return data[1], payload[0]


async def find_ring():
    device = await BleakScanner.find_device_by_address(ADDRESS, timeout=60)
    if not device:
        raise RuntimeError(f"{NAME} is not advertising; disconnect it from other devices")
    return device


async def inspect() -> None:
    async with BleakClient(await find_ring(), timeout=20) as client:
        hardware = bytes(await client.read_gatt_char(HARDWARE_UUID)).decode()
        firmware = bytes(await client.read_gatt_char(FIRMWARE_UUID)).decode()
        print(f"{NAME} {ADDRESS}")
        print(f"hardware: {hardware}")
        print(f"firmware: {firmware}")
        print(f"MTU: {client.mtu_size}")


async def flash(path: Path, confirmed: bool) -> None:
    image = path.read_bytes()
    if len(image) < 0x450 or not image[0x10:0x17] == b"RT11CR_":
        raise RuntimeError("not an RT11CR firmware image")
    digest = hashlib.sha256(image).hexdigest()
    print(f"image: {path} ({len(image)} bytes)")
    print(f"SHA-256: {digest}")
    if not confirmed:
        raise RuntimeError("refusing to flash without --yes")

    queue: asyncio.Queue[bytes] = asyncio.Queue()

    def notified(_sender, data: bytearray) -> None:
        queue.put_nowait(bytes(data))

    async def expect(command: int, timeout: float = 15) -> None:
        response = await asyncio.wait_for(queue.get(), timeout)
        actual, status = parse_response(response)
        if actual != command or status:
            raise RuntimeError(f"OTA command {command} failed: command={actual}, status={status}")

    async with BleakClient(await find_ring(), timeout=20) as client:
        hardware = bytes(await client.read_gatt_char(HARDWARE_UUID)).decode()
        if hardware != "RT11CR_V1.0":
            raise RuntimeError(f"unexpected hardware: {hardware}")
        await client.start_notify(NOTIFY_UUID, notified)
        write_size = client.services.get_characteristic(WRITE_UUID).max_write_without_response_size

        async def send(command: int, payload: bytes = b"") -> None:
            packet = frame(command, payload)
            for offset in range(0, len(packet), write_size):
                await client.write_gatt_char(WRITE_UUID, packet[offset:offset + write_size], response=False)

        await send(1)
        await expect(1)
        init = b"\x01" + struct.pack("<IHH", len(image), crc16(image), sum(image) & 0xFFFF)
        await send(2, init)
        await expect(2)

        chunks = (len(image) + 1023) // 1024
        for index in range(chunks):
            payload = struct.pack("<H", index + 1) + image[index * 1024:(index + 1) * 1024]
            await send(3, payload)
            await expect(3)

        await send(4)
        await expect(4, 30)
        await send(5)
        print("firmware accepted; ring is rebooting")


def self_test() -> None:
    assert crc16(b"123456789") == 0x4B37
    assert frame(1) == bytes.fromhex("bc010000ffff")
    assert parse_response(bytes.fromhex("bc030100bf4000")) == (3, 0)
    print("packet self-test passed")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--flash", type=Path, metavar="FIRMWARE")
    parser.add_argument("--yes", action="store_true", help="confirm an irreversible OTA attempt")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
    elif args.flash:
        asyncio.run(flash(args.flash, args.yes))
    else:
        asyncio.run(inspect())


if __name__ == "__main__":
    main()
