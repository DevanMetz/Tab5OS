"""Compare Byte Lab's signed/Float32 decoding with Python's wire decoders."""

import math
from pathlib import Path
import struct
import subprocess
import sys


def main():
    binary = Path(sys.argv[1]).resolve(strict=True)
    output = subprocess.run(
        [str(binary), "--vectors"], check=True, capture_output=True, text=True, timeout=30
    ).stdout
    rows = output.splitlines()
    if len(rows) != 4096:
        raise AssertionError(f"Expected 4096 vectors, got {len(rows)}")
    for row in rows:
        payload, signed_be, signed_le, float_be, float_le = row.split(",")
        data = bytes.fromhex(payload)
        for byte_order, fmt, integer, floating in (
            ("big", ">f", signed_be, float_be), ("little", "<f", signed_le, float_le)
        ):
            expected_integer = int.from_bytes(data, byte_order, signed=True)
            if int(integer) != expected_integer:
                raise AssertionError(f"{payload} {byte_order}: {integer} != {expected_integer}")
            expected_float = struct.unpack(fmt, data)[0]
            actual_float = float.fromhex(floating)
            if math.isnan(expected_float):
                equal = math.isnan(actual_float)
            else:
                equal = actual_float == expected_float and (
                    math.copysign(1, actual_float) == math.copysign(1, expected_float)
                )
            if not equal:
                raise AssertionError(f"{payload} {byte_order}: {actual_float} != {expected_float}")
    print(f"Byte reference: {len(rows)} signed/Float32 vectors match Python in both byte orders PASS")


if __name__ == "__main__":
    main()
