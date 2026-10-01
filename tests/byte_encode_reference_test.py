"""Check encoded bytes against integer serialization and exact rational rounding."""
from fractions import Fraction
from pathlib import Path
import random
import subprocess
import sys


def binary32_fraction(bits):
    exponent, fraction = (bits >> 23) & 255, bits & 0x7fffff
    significand = fraction | (0x800000 if exponent else 0)
    power = exponent - 150 if exponent else -149
    return Fraction(significand * (1 << power)) if power >= 0 else Fraction(significand, 1 << -power)


def float_bits(text):
    if text == "NaN":
        return 0x7fc00000
    if text in ("Inf", "+Inf", "-Inf"):
        return 0xff800000 if text[0] == "-" else 0x7f800000
    negative = text.startswith("-")
    value = abs(Fraction(text))
    sign = 0x80000000 if negative else 0
    if not value:
        return sign
    # Locate adjacent binary32 numbers and choose the nearer one, with even
    # low bit at a tie. All comparisons use exact rationals, not C/host floats.
    lo, hi = 0, 0x7f7fffff
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if binary32_fraction(mid) <= value:
            lo = mid
        else:
            hi = mid - 1
    lower = binary32_fraction(lo)
    upper = binary32_fraction(lo + 1) if lo < 0x7f7fffff else Fraction(1 << 128)
    delta = 2 * value - lower - upper
    rounded = lo + (delta > 0 or (delta == 0 and lo % 2 == 1))
    if rounded == 0 or rounded == 0x7f800000:
        return None  # Nonzero underflow to zero, or finite decimal overflow.
    return sign | rounded


def main():
    binary = Path(sys.argv[1]).resolve(strict=True)
    random_source = random.Random(0x5aab207)
    cases = []
    for kind, width, signed in ((0, 1, False), (1, 1, True), (2, 2, False),
                                (3, 2, True), (4, 4, False), (5, 4, True)):
        minimum = -(1 << (width * 8 - 1)) if signed else 0
        maximum = (1 << (width * 8 - int(signed))) - 1
        values = [minimum - 1, minimum, minimum + 1, maximum - 1, maximum, maximum + 1]
        values += [random_source.randint(minimum, maximum) for _ in range(256)]
        for value in values:
            for little in (0, 1):
                text = str(value) if value < 0 else "+" + str(value)
                expected = value.to_bytes(width, "little" if little else "big", signed=signed).hex().upper() if minimum <= value <= maximum else None
                cases.append((f"{kind},{little},{text}", expected))
    floats = ["0", "-0", "+0.00e999", "-0e-999", "NaN", "Inf", "+Inf", "-Inf",
              "1.000000059604644775390624", "1.000000059604644775390625", "1.000000059604644775390626",
              "-1.000000059604644775390626", "3.40282347e38", "3.4028236e38", "1e-45", "1e-46"]
    for _ in range(2048):
        floats.append(f"{random_source.randrange(-10**12, 10**12)}e{random_source.randrange(-58, 31)}")
    for text in floats:
        bits = float_bits(text)
        for little in (0, 1):
            expected = None if bits is None else bits.to_bytes(4, "little" if little else "big").hex().upper()
            cases.append((f"6,{little},{text}", expected))
    rows = subprocess.run([str(binary), "--encode"], input="\n".join(case[0] for case in cases) + "\n",
                          text=True, capture_output=True, check=True, timeout=30).stdout.splitlines()
    assert len(rows) == len(cases), (len(rows), len(cases))
    for (case, expected), actual in zip(cases, rows):
        assert (actual.startswith("ERROR:") if expected is None else actual == expected), (case, actual, expected)
    print(f"Byte encoder: {len(rows)} integer/Float32/order cases match independent exact references PASS")


if __name__ == "__main__":
    main()
