"""Check displayed register-pair values against independent Python decoders."""
import math
from pathlib import Path
import struct
import subprocess
import sys


def main():
    binary = Path(sys.argv[1]).resolve(strict=True)
    rows = subprocess.run([str(binary), "--vectors"], check=True, capture_output=True,
                          text=True, timeout=30).stdout.splitlines()
    assert len(rows) == 4096, len(rows)
    permutations = ((0, 1, 2, 3), (2, 3, 0, 1), (1, 0, 3, 2), (3, 2, 1, 0))
    for row in rows:
        wire, order, unsigned, signed, floating = row.split(",")
        raw = bytes.fromhex(wire)
        data = bytes(raw[index] for index in permutations[int(order)])
        assert int(unsigned) == int.from_bytes(data, "big"), row
        assert int(signed) == int.from_bytes(data, "big", signed=True), row
        expected = struct.unpack(">f", data)[0]
        actual = float(floating)
        if math.isnan(expected):
            assert math.isnan(actual), row
        else:
            # Nine significant digits preserve binary32 round trips. Check the
            # actual rendered decimal, including the sign of zero and infinity.
            assert actual == float(format(expected, ".9g")), row
            assert math.copysign(1, actual) == math.copysign(1, expected), row
    print("Modbus reference: 4096 register/order cases match Python unsigned/signed/Float32 views PASS")


if __name__ == "__main__":
    main()
