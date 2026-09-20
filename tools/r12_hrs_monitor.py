#!/usr/bin/env python3
"""Monitor the COLMI R12 standard BLE Heart Rate characteristic."""

import argparse
import asyncio
import time

from bleak import BleakClient, BleakScanner


ADDRESS = "30:31:48:31:A1:04"
HRS_UUID = "00002a37-0000-1000-8000-00805f9b34fb"


async def monitor(seconds: int, scan_timeout: int) -> None:
    device = await BleakScanner.find_device_by_address(ADDRESS, timeout=scan_timeout)
    if not device:
        raise RuntimeError("ring not advertising")
    packets: list[tuple[float, int]] = []
    disconnected = asyncio.Event()

    def disconnected_callback(_client: BleakClient) -> None:
        print("DISCONNECTED", flush=True)
        disconnected.set()

    async with BleakClient(device, timeout=20, disconnected_callback=disconnected_callback) as client:
        started = time.monotonic()

        def notified(_sender, data: bytearray) -> None:
            raw = bytes(data)
            if len(raw) < 2 or (raw[0] & 1 and len(raw) < 3):
                print(f"invalid raw={raw.hex()}", flush=True)
                return
            bpm = int.from_bytes(raw[1:3], "little") if raw and raw[0] & 1 else raw[1]
            now = time.monotonic()
            packets.append((now, bpm))
            print(f"{now - started:6.1f}s bpm={bpm} raw={raw.hex()}", flush=True)

        await client.start_notify(HRS_UUID, notified)
        print("SUBSCRIBED", flush=True)
        try:
            await asyncio.wait_for(disconnected.wait(), seconds)
        except TimeoutError:
            pass
        print(f"connected={client.is_connected} packets={len(packets)}", flush=True)
    if len(packets) > 1:
        gaps = [b[0] - a[0] for a, b in zip(packets, packets[1:])]
        print(f"max_gap={max(gaps):.2f}s", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--seconds", type=int, default=180)
    parser.add_argument("--scan-timeout", type=int, default=180)
    args = parser.parse_args()
    asyncio.run(monitor(args.seconds, args.scan_timeout))
