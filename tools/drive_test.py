"""Open the Ender 3 app on the Tab5, hold FWD, and capture the console log
(which now includes TX commands and Marlin RX responses)."""
import struct
import sys
import threading
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM7"
HEADER = struct.Struct("<4sBBBBHHII")
COMMAND = struct.Struct("<2sBBHHH")

GREEN = (46, 125, 50)   # 0x2E7D32 FWD/BACK pads
ENDER3 = (27, 94, 32)   # 0x1B5E20 launcher tile


def rgb565(v):
    return (((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)


def near(a, b, tol=14):
    return abs(a[0] - b[0]) <= tol and abs(a[1] - b[1]) <= tol and abs(a[2] - b[2]) <= tol


class Link:
    def __init__(self, port):
        self.port = port
        self.text = bytearray()
        self.buf = bytearray()
        self.lock = threading.Lock()
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        while True:
            try:
                data = self.port.read(4096)
            except serial.SerialException:
                return
            if data:
                with self.lock:
                    self.buf.extend(data)

    def _wait_for(self, count, deadline):
        while True:
            with self.lock:
                if len(self.buf) >= count:
                    return True
            if time.monotonic() > deadline:
                return False
            time.sleep(0.01)

    def read_frame(self):
        for _ in range(3):
            self.port.write(COMMAND.pack(b"T5", 1, 0, 0, 0, 0))
            time.sleep(0.05)
            deadline = time.monotonic() + 5
            while True:  # sync on T5RD, keeping any prefix as console text
                with self.lock:
                    idx = self.buf.find(b"T5RD")
                    if idx >= 0:
                        self.text.extend(self.buf[:idx])
                        del self.buf[:idx]
                        break
                if time.monotonic() > deadline:
                    return None
                time.sleep(0.02)
            if not self._wait_for(HEADER.size, deadline):
                return None
            with self.lock:
                hdr = bytes(self.buf[:HEADER.size])
                del self.buf[:HEADER.size]
            magic, version, typ, enc, flags, w, h, payload_size, frame_number = HEADER.unpack(hdr)
            if version != 1 or typ != 1 or enc != 1 or payload_size > w * h * 4:
                continue
            if not self._wait_for(payload_size, deadline):
                return None
            with self.lock:
                payload = bytes(self.buf[:payload_size])
                del self.buf[:payload_size]
            px = []
            try:
                for count, value in struct.iter_unpack("<HH", payload):
                    px.extend([rgb565(value)] * count)
            except struct.error:
                continue
            if w == 720 and h == 1280 and len(px) == w * h:
                return w, h, px, frame_number
        return None

    def touch(self, x, y, pressed):
        self.port.write(COMMAND.pack(b"T5", 2, 1 if pressed else 0, x, y, 0))
        time.sleep(0.04)

    def drag(self, x0, y0, x1, y1, steps=10):
        self.touch(x0, y0, True)
        for i in range(1, steps + 1):
            self.touch(x0 + (x1 - x0) * i // steps, y0 + (y1 - y0) * i // steps, True)
            time.sleep(0.02)
        self.touch(x1, y1, False)
        time.sleep(0.3)

    def tap(self, x, y):
        self.touch(x, y, True)
        time.sleep(0.06)
        self.touch(x, y, False)
        time.sleep(0.4)


def find_color(px, w, h, color, block=4):
    xs, ys = [], []
    for i in range(0, len(px), block):
        if near(px[i], color, 14):
            ys.append(i // w)
            xs.append(i % w)
    if not xs:
        return None
    return (min(xs) + max(xs)) // 2, (min(ys) + max(ys)) // 2


def find_top_green(px, w, h, color, block=4):
    """Centroid of the highest green cluster (FWD sits above BACK)."""
    pts = [(i % w, i // w) for i in range(0, len(px), block) if near(px[i], color, 14)]
    if not pts:
        return None
    min_y = min(p[1] for p in pts)
    top = [p for p in pts if p[1] < min_y + 220]
    if not top:
        return None
    return sum(p[0] for p in top) // len(top), sum(p[1] for p in top) // len(top)


def main():
    port = serial.Serial(PORT, 115200, timeout=0.2)
    link = Link(port)
    time.sleep(6)  # post-flash boot

    # Return to the launcher first so the app opens fresh (motors re-enabled).
    link.tap(60, 50)
    time.sleep(1)

    # Scroll the launcher until the Ender 3 tile is visible, then open it.
    # If the app is already open (no tile found), continue to the FWD test.
    opened = False
    for _ in range(4):
        frame = link.read_frame()
        if not frame:
            print("no frame; aborting")
            break
        w, h, px, fn = frame
        center = find_color(px, w, h, ENDER3)
        if center:
            print("tile at", center)
            link.tap(*center)
            opened = True
            break
        link.drag(360, 1150, 360, 500, 10)

    if not opened:
        print("tile not found; assuming app already open")
    time.sleep(6)  # app opens, USB host starts, connect + init sequence runs

    frame = link.read_frame()
    if frame:
        w, h, px, fn = frame
        fwd = find_top_green(px, w, h, GREEN)
        if fwd:
            print("FWD at", fwd)
            link.touch(*fwd, True)
            time.sleep(2.0)
            link.touch(*fwd, False)
        else:
            print("FWD pad not found")
    time.sleep(3)

    port.close()
    time.sleep(0.5)
    print("===== CONSOLE =====")
    sys.stdout.write(bytes(link.text).decode("utf-8", errors="replace"))


if __name__ == "__main__":
    main()
