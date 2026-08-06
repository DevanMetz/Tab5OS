#!/usr/bin/env python3
"""Tiny USB remote desktop client for Tab5 OS."""

import argparse
import queue
import struct
import threading
import time
import tkinter as tk

try:
    import serial
except ImportError as error:
    raise SystemExit("pyserial is required: python -m pip install pyserial") from error

COMMAND = struct.Struct("<2sBBHHH")
HEADER = struct.Struct("<4sBBBBHHII")


def command(kind, pressed=0, x=0, y=0):
    return COMMAND.pack(b"T5", kind, pressed, x, y, 0)


def ppm_from_rle(payload, width, height):
    chunks = [f"P6\n{width} {height}\n255\n".encode()]
    pixels = 0
    if len(payload) % 4:
        raise ValueError("invalid RLE payload")
    for count, value in struct.iter_unpack("<HH", payload):
        red = ((value >> 11) & 31) * 255 // 31
        green = ((value >> 5) & 63) * 255 // 63
        blue = (value & 31) * 255 // 31
        chunks.append(bytes((red, green, blue)) * count)
        pixels += count
    if pixels != width * height:
        raise ValueError("RLE pixel count mismatch")
    return b"".join(chunks)


class Client:
    def __init__(self, port):
        self.serial = serial.Serial(port, timeout=0.1, write_timeout=1)
        self.commands = queue.Queue()
        self.frames = queue.Queue(maxsize=1)
        self.running = True
        threading.Thread(target=self._worker, daemon=True).start()

    def send_touch(self, pressed, x, y):
        self.commands.put(command(2, pressed, x, y))

    def _read_exact(self, size, deadline):
        data = bytearray()
        while len(data) < size and time.monotonic() < deadline and self.running:
            data.extend(self.serial.read(size - len(data)))
        return bytes(data) if len(data) == size else None

    def _read_frame(self):
        deadline = time.monotonic() + 3
        magic = bytearray()
        while time.monotonic() < deadline and self.running:
            byte = self.serial.read(1)
            if not byte:
                continue
            magic = (magic + byte)[-4:]
            if magic == b"T5RD":
                break
        else:
            return None
        rest = self._read_exact(HEADER.size - 4, deadline)
        if not rest:
            return None
        _, version, kind, encoding, _, width, height, size, _ = HEADER.unpack(b"T5RD" + rest)
        if version != 1 or kind != 1 or encoding != 1 or size > width * height * 4:
            return None
        payload = self._read_exact(size, deadline)
        return ppm_from_rle(payload, width, height) if payload else None

    def _worker(self):
        time.sleep(2.5)
        while self.running:
            while not self.commands.empty():
                self.serial.write(self.commands.get_nowait())
            self.serial.write(command(1))
            frame = self._read_frame()
            if frame:
                if self.frames.full():
                    self.frames.get_nowait()
                self.frames.put(frame)

    def close(self):
        self.running = False
        self.serial.close()


def run(port, scale):
    client = Client(port)
    root = tk.Tk()
    root.title(f"Tab5 OS - {port}")
    canvas = tk.Canvas(root, width=720 // scale, height=1280 // scale, highlightthickness=0)
    canvas.pack()

    def touch(event, pressed):
        client.send_touch(pressed, max(0, min(719, event.x * scale)), max(0, min(1279, event.y * scale)))

    canvas.bind("<ButtonPress-1>", lambda event: touch(event, 1))
    canvas.bind("<B1-Motion>", lambda event: touch(event, 1))
    canvas.bind("<ButtonRelease-1>", lambda event: touch(event, 0))

    def refresh():
        try:
            while True:
                image = tk.PhotoImage(data=client.frames.get_nowait(), format="PPM").subsample(scale)
                canvas.delete("all")
                canvas.create_image(0, 0, anchor="nw", image=image)
                canvas.image = image
        except queue.Empty:
            pass
        if client.running:
            root.after(30, refresh)

    def close():
        client.close()
        root.destroy()

    root.protocol("WM_DELETE_WINDOW", close)
    refresh()
    root.mainloop()


def self_test():
    payload = struct.pack("<HHHH", 2, 0xF800, 1, 0x07E0)
    assert ppm_from_rle(payload, 3, 1).endswith(b"\xff\x00\x00" * 2 + b"\x00\xff\x00")
    assert len(command(2, 1, 719, 1279)) == 10


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM7")
    parser.add_argument("--scale", type=int, default=2)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
    else:
        run(args.port, max(1, args.scale))
