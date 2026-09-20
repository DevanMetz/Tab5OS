"""Capture one frame from the Tab5 and render a screen region as ASCII text,
so label text can be read without OCR."""
import struct
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM7"
HEADER = struct.Struct("<4sBBBBHHII")
COMMAND = struct.Struct("<2sBBHHH")


def read_exact(port, size, deadline):
    data = b""
    while len(data) < size:
        if time.monotonic() > deadline:
            return None
        chunk = port.read(size - len(data))
        if chunk:
            data += chunk
        else:
            time.sleep(0.02)
    return data


def rgb565(v):
    return (((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)


def read_frame(port):
    port.write(COMMAND.pack(b"T5", 1, 0, 0, 0, 0))
    time.sleep(0.05)
    deadline = time.monotonic() + 5
    hdr = read_exact(port, HEADER.size, deadline)
    if not hdr:
        return None
    magic, version, typ, enc, flags, w, h, payload_size, frame_number = HEADER.unpack(hdr)
    payload = read_exact(port, payload_size, deadline)
    if not payload:
        return None
    px = []
    for count, value in struct.iter_unpack("<HH", payload):
        px.extend([rgb565(value)] * count)
    return w, h, px, frame_number


def render(px, w, x0, y0, x1, y1, step=2):
    """Render region as ASCII: text pixels '#', mid '+', dim '.', bg ' '."""
    out = []
    for y in range(y0, y1, step):
        line = []
        for x in range(x0, x1, step):
            r, g, b = px[y * w + x]
            lum = (r + g + b) // 3
            # text is bright white/light gray on dark bg
            ch = " "
            if lum > 200:
                ch = "#"
            elif lum > 120:
                ch = "+"
            elif lum > 60:
                ch = "."
            line.append(ch)
        out.append("".join(line))
    return out


def main():
    port = serial.Serial(PORT, 115200, timeout=0.2)
    time.sleep(2.0)
    frame = read_frame(port)
    if not frame:
        print("no frame")
        return
    w, h, px, fn = frame
    print("frame", fn, w, "x", h)

    x0, y0 = 60, 185
    x1, y1 = 660, 380
    for line in render(px, w, x0, y0, x1, y1, step=2):
        print(line)
    port.close()


if __name__ == "__main__":
    main()
