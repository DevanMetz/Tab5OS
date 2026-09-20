"""Reset the Tab5 over USB-Serial/JTAG and capture the boot console log."""
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM7"
DURATION = float(sys.argv[2]) if len(sys.argv) > 2 else 15.0

port = serial.Serial(PORT, 115200, timeout=0.2)
# USB-Serial/JTAG reset protocol (same as esptool's default_reset):
# assert RTS (reset) with DTR low, then release.
port.dtr = False
port.rts = True
time.sleep(0.15)
port.rts = False

end = time.time() + DURATION
out = bytearray()
while time.time() < end:
    chunk = port.read(4096)
    if chunk:
        out.extend(chunk)
    else:
        time.sleep(0.05)
port.close()
sys.stdout.write(out.decode("utf-8", errors="replace"))
