"""Drive the Tab5 remote-desktop protocol: capture frames, scroll the
launcher, open the Ender 3 app, and report color-layout evidence."""
import struct
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM7"
HEADER = struct.Struct("<4sBBBBHHII")
COMMAND = struct.Struct("<2sBBHHH")

# tile colors (RGB) used by the Ender 3 app / launcher
KNOWN = {
    "ENDER3_TILE": (27, 94, 32),     # 0x1B5E20 launcher tile
    "PAD_GREEN": (46, 125, 50),      # 0x2E7D32 FWD/BACK
    "PAD_BLUE": (21, 101, 192),      # 0x1565C0 LEFT/RIGHT
    "PAD_RED": (198, 40, 40),        # 0xC62828 STOP
    "BG": (16, 20, 31),              # 0x10141f content bg
}


def rgb565(v):
    return (((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)


def near(a, b, tol=12):
    return abs(a[0] - b[0]) <= tol and abs(a[1] - b[1]) <= tol and abs(a[2] - b[2]) <= tol


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


def command(kind, pressed=0, x=0, y=0):
    return COMMAND.pack(b"T5", kind, pressed, x, y, 0)


def read_frame(port):
    port.write(command(1))  # request a frame
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


def layout_map(w, h, px, block=8):
    """Downsample to a character map: known colors get letters, bright text '#'."""
    cols, rows = w // block, h // block
    out = []
    for by in range(rows):
        line = []
        for bx in range(cols):
            r = g = b = 0
            n = 0
            for yy in range(by * block, (by + 1) * block, 2):
                for xx in range(bx * block, (bx + 1) * block, 2):
                    idx = yy * w + xx
                    if idx < len(px):
                        r += px[idx][0]
                        g += px[idx][1]
                        b += px[idx][2]
                        n += 1
            if n:
                avg = (r // n, g // n, b // n)
            else:
                avg = (0, 0, 0)
            ch = "."
            for name, color in KNOWN.items():
                if near(avg, color, 14):
                    ch = name[0]
                    break
            if ch == ".":
                lum = (avg[0] + avg[1] + avg[2]) // 3
                ch = "#" if lum > 140 else ("+" if lum > 60 else ".")
            line.append(ch)
        out.append("".join(line))
    return out


def color_stats(w, h, px):
    stats = {}
    for name, color in KNOWN.items():
        count = 0
        for i in range(0, len(px), 8):
            if near(px[i], color, 14):
                count += 1
        stats[name] = count
    return stats


def touch(port, x, y, pressed):
    port.write(command(2, 1 if pressed else 0, x, y))
    time.sleep(0.05)


def drag(port, x0, y0, x1, y1, steps=8):
    touch(port, x0, y0, True)
    for i in range(1, steps + 1):
        x = x0 + (x1 - x0) * i // steps
        y = y0 + (y1 - y0) * i // steps
        touch(port, x, y, True)
        time.sleep(0.02)
    touch(port, x1, y1, False)
    time.sleep(0.3)


def tap(port, x, y):
    touch(port, x, y, True)
    time.sleep(0.06)
    touch(port, x, y, False)
    time.sleep(0.4)


def main():
    port = serial.Serial(PORT, 115200, timeout=0.2)
    time.sleep(2.5)  # let the frame stream start

    print("== frame 1: launcher ==")
    frame = read_frame(port)
    if not frame:
        print("no frame")
        return
    w, h, px, fn = frame
    print("frame", fn, "size", w, "x", h, "colors", color_stats(w, h, px))
    for line in layout_map(w, h, px):
        print(line)

    # scroll the launcher up until the Ender 3 tile (row 6) is visible
    for _ in range(3):
        drag(port, 360, 1150, 360, 350, 10)
        frame = read_frame(port)
        if not frame:
            print("no frame after scroll")
            return
        w, h, px, fn = frame
        stats = color_stats(w, h, px)
        print("== after scroll: colors ==", stats)
        if stats["ENDER3_TILE"] > 50:
            break

    # find the Ender 3 tile (dark green) and tap its center
    block = 4
    xs, ys = [], []
    for i in range(0, len(px), block):
        if near(px[i], KNOWN["ENDER3_TILE"], 14):
            ys.append(i // w)
            xs.append(i % w)
    if xs:
        cx = (min(xs) + max(xs)) // 2
        cy = (min(ys) + max(ys)) // 2
        print("tapping Ender 3 tile at", cx, cy)
        tap(port, cx, cy)
        time.sleep(0.8)
        frame = read_frame(port)
        if frame:
            w, h, px, fn = frame
            print("== frame: after tap ==", color_stats(w, h, px))
            for line in layout_map(w, h, px):
                print(line)
    else:
        print("Ender 3 tile not found")
    port.close()


if __name__ == "__main__":
    main()
