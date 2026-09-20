#!/usr/bin/env python3
"""Patch COLMI R12 1.00.09 to render its retained BPM on every frame."""

import argparse
import hashlib
import struct
from pathlib import Path


STOCK_SHA256 = "3f27c47d5ab829ad768e4f4dcfb2aa5153f36dcbb1e3defd721520920ce5a3fb"
STOCK_VERSION = b"1.00.09"
PATCH_VERSION = b"1.01.03"
RECOVERY_VERSION = b"1.01.04"
GIT_VERSION_OFFSET = 0xB0
STOCK_GIT_VERSION = 0x1041
PATCH_GIT_VERSION = 0x62041
RECOVERY_GIT_VERSION = 0x63041
ENDURANCE_PATCH_VERSION = b"1.01.05"
ENDURANCE_RECOVERY_VERSION = b"1.01.06"
ENDURANCE_PATCH_GIT_VERSION = 0x64041
ENDURANCE_RECOVERY_GIT_VERSION = 0x65041
ZWIFT_PATCH_VERSION = b"1.00.99"
ZWIFT_RECOVERY_VERSION = b"1.01.00"
ZWIFT_PATCH_GIT_VERSION = 0x5B041
ZWIFT_RECOVERY_GIT_VERSION = 0x5C041
STOCK_CTRL_FLAGS = 0x0981
PATCH_CTRL_FLAGS = 0x0901
# Store the exact completed BPM, keep it across navigation, and load it in the
# unconditional render path rather than the measurement-complete branch.
PATCHES = (
    (
        "setter",
        0xE178,
        bytes.fromhex("10b500218d4c5d2821700ad8f2f7"),
        bytes.fromhex("282803d3dc2801d88c4908707047"),
    ),
    ("clear_callback", 0xE244, bytes.fromhex("5b49002008707047"), bytes.fromhex("704700bf00bf00bf")),
    ("clear_init", 0xE278, bytes.fromhex("2070"), bytes.fromhex("00bf")),
    (
        "commit",
        0x1520A,
        bytes.fromhex("3471707800280dd0"),
        bytes.fromhex("2046f8f7b4ff00bf"),
    ),
    (
        "render",
        0x1523C,
        bytes.fromhex("f078401cc0b2f070002c05d0"),
        bytes.fromhex("f8f7b2ff0446002c06d000bf"),
    ),
)

ENDURANCE_PATCHES = (
    ("setter_trampoline", 0xE178, bytes.fromhex("10b50021"), bytes.fromhex("00f09ab9")),
    ("fake_retained_call", 0xE23E, bytes.fromhex("fff79bff"), bytes.fromhex("00bf00bf")),
    ("clear_callback", 0xE244, bytes.fromhex("5b49002008707047"), bytes.fromhex("704700bf00bf00bf")),
    ("disable_fake_start", 0xE24C, bytes.fromhex("10b5"), bytes.fromhex("7047")),
    ("clear_init", 0xE278, bytes.fromhex("2070"), bytes.fromhex("00bf")),
    (
        "periodic_callback",
        0xE468,
        bytes.fromhex("70b5834d2d1da888401c80b2a880032807d90e2806d801f0fdf9002801d100f0e5f870bd01f0fef9022802d0a8883228f7d301f0faf90446fdf7dbfa744e083e002809d0462c21db"),
        bytes.fromhex("70b50f4dae880136ae800f2e17d301f009fa02280fd101f008fafff779fe04464cb10848007818b121462868fff798ff00f0dcf803e01e2e01d300f0d7f870bd0cc1200004c12000"),
    ),
    (
        "validator",
        0xE4B0,
        bytes.fromhex("707800281ed1012070700420a88070bd282c06daf2f7aaf90a210af024fd28310fe0782c0eddfdf779f91e280ada68480078022806d0f2f799f90a210af013fd5f310c4630780028c5d0e1b22868fff7"),
        bytes.fromhex("70b504462838b4281ed8f3f7befa06460e4d6968a1b18e4216d0701a02280fd86878201a00d54042032809d8a8780130a8706e60032807d32c70ae60204670bd6c700120a8706e60002070bdecc02000"),
    ),
    (
        "spot_callback",
        0xE770,
        bytes.fromhex("10b56a4c0c342078401cc0b2207003283ed90e2806d801f079f8002838d16349486033e001f07af8022802d0207832282ed301f076f80446"),
        bytes.fromhex("70b50c4d2e7b01362e730f2e11d301f085f8022809d101f084f8fff7f5fc20b1fff78cfffff780ff03e01e2e01d3fff77bff70bd18c12000"),
    ),
    (
        "screen_fallback",
        0x151F6,
        bytes.fromhex("ebf711fbc117890f091889088900401a5f30c4b2"),
        bytes.fromhex("002400bf00bf00bf00bf00bf00bf00bf00bf00bf"),
    ),
    ("commit", 0x1520A, bytes.fromhex("3471707800280dd0"), bytes.fromhex("2046f8f7b4ff00bf")),
    ("render", 0x1523C, bytes.fromhex("f078401cc0b2f070002c05d0"), bytes.fromhex("f8f7b2ff0446002c06d000bf")),
)

ZWIFT_PATCHES = (
    (
        "setter",
        0xE178,
        bytes.fromhex("10b500218d4c5d2821700ad8f2f74afbc117890f091889088900401a5d30c0b202e0642800d96420207010bd"),
        bytes.fromhex("10b50446282c0ad3dc2c08d805490c7005480021017044700221f9f7bffe204610bd00bfecc02000949e2000"),
    ),
    ("clear_callback", 0xE244, bytes.fromhex("5b49002008707047"), bytes.fromhex("704700bf00bf00bf")),
    ("clear_init", 0xE278, bytes.fromhex("2070"), bytes.fromhex("00bf")),
    ("screen_gate", 0x15142, bytes.fromhex("edf7dafd"), bytes.fromhex("002000bf")),
    ("start_delay", 0x15184, bytes.fromhex("0628"), bytes.fromhex("0028")),
    ("restart", 0x151C6, bytes.fromhex("32e0"), bytes.fromhex("c2e7")),
    ("commit", 0x1520A, bytes.fromhex("3471707800280dd0"), bytes.fromhex("2046f8f7b4ff00bf")),
    ("render", 0x1523C, bytes.fromhex("f078401cc0b2f070002c05d0"), bytes.fromhex("0bf09ef90446002c06d000bf")),
    ("tap_next_call", 0x15366, bytes.fromhex("401cc0b2"), bytes.fromhex("0bf019f9")),
    ("hrs_size", 0x7EC2, bytes.fromhex("fc22"), bytes.fromhex("7022")),
    (
        "hrs_notify",
        0x7F14,
        bytes.fromhex("1cb503460120009101908d488d49807c042209780ef0c1fd1cbd"),
        bytes.fromhex("01220eb5022907d103468d488d49807c022209780ef0c1fd0ebd"),
    ),
    ("hrs_service_id", 0x8158, bytes.fromhex("5b9e2000"), bytes.fromhex("5e9e2000")),
    (
        "hrs_repeat",
        0x2057C,
        bytes.fromhex("020003280200000000000000000000000000010000000000010000000400c9fe"),
        bytes.fromhex("10b506484478002c06d031780322114202d10221e7f7c0fc204610bd949e2000"),
    ),
    (
        "tap_next",
        0x2059C,
        bytes.fromhex("000000000000000000000000000000000000000001000000"),
        bytes.fromhex("032804d10348417c0320022900d00130704700bf409e2000"),
    ),
    ("adv_uuid_lo", 0x75F0, bytes.fromhex("e725"), bytes.fromhex("0d25")),
    ("adv_uuid_hi", 0x75F4, bytes.fromhex("fe26"), bytes.fromhex("1826")),
    (
        "hrs_table",
        0x2050C,
        bytes.fromhex("02080028e7fe00000000000000000000000002000000000001000000020003281200000000000000000000000000010000000000010000000400a1fe00000000000000000000000000000000000000000100000012000229000000000000000000000000000002000000000011000000"),
        bytes.fromhex("020800280d1800000000000000000000000002000000000001000000020003281000000000000000000000000000010000000000010000000400372a00000000000000000000000000000000000000000100000012000229000000000000000000000000000002000000000011000000"),
    ),
)


def crc16_btx(payload: bytes) -> int:
    """Realtek BTX CRC-16: reflected 0x8005 polynomial, initial value 0."""
    crc = 0
    for byte in payload:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xA001 if crc & 1 else 0)
    return crc


def patch_firmware(
    source: Path,
    destination: Path,
    recovery: bool = False,
    compiled_dir: Path | None = None,
    zwift: bool = False,
    endurance: bool = False,
) -> None:
    data = bytearray(source.read_bytes())
    digest = hashlib.sha256(data).hexdigest()
    if digest != STOCK_SHA256:
        raise SystemExit(f"unsupported firmware SHA-256: {digest}")
    if zwift and endurance:
        raise SystemExit("--zwift and --endurance are mutually exclusive")
    selected_patches = ENDURANCE_PATCHES if endurance else (ZWIFT_PATCHES if zwift else PATCHES)
    for name, offset, original, _ in selected_patches:
        if data[offset : offset + len(original)] != original:
            raise SystemExit(f"{name} site does not match stock firmware")
    patches = []
    for name, offset, original, expected in selected_patches:
        patched = expected
        if compiled_dir is not None and not recovery:
            patched = (compiled_dir / f"{name}.bin").read_bytes()
            if patched != expected:
                raise SystemExit(
                    f"compiled {name} differs from verified bytes: "
                    f"{patched.hex()} != {expected.hex()}"
                )
        patches.append((name, offset, original, patched))
    if not recovery:
        for _, offset, _, patched in patches:
            data[offset : offset + len(patched)] = patched

    # The updater accepts a same-version transfer, but the device may retain the
    # active image. Bump every fixed-width package/runtime version occurrence so
    # activation is eligible and visible through Device Information (0x2A26).
    version_offsets = [i for i in range(len(data)) if data.startswith(STOCK_VERSION, i)]
    if len(version_offsets) != 4:
        raise SystemExit(f"expected 4 version strings, found {len(version_offsets)}")
    if endurance:
        version = ENDURANCE_RECOVERY_VERSION if recovery else ENDURANCE_PATCH_VERSION
    elif zwift:
        version = ZWIFT_RECOVERY_VERSION if recovery else ZWIFT_PATCH_VERSION
    else:
        version = RECOVERY_VERSION if recovery else PATCH_VERSION
    for offset in version_offsets:
        data[offset : offset + len(version)] = version

    # Realtek's dual-bank bootloader selects the bank with the higher image
    # version. This is the first word of the 16-byte git_ver header field.
    if struct.unpack_from("<I", data, GIT_VERSION_OFFSET)[0] != STOCK_GIT_VERSION:
        raise SystemExit("image version metadata does not match stock firmware")
    if endurance:
        git_version = ENDURANCE_RECOVERY_GIT_VERSION if recovery else ENDURANCE_PATCH_GIT_VERSION
    elif zwift:
        git_version = ZWIFT_RECOVERY_GIT_VERSION if recovery else ZWIFT_PATCH_GIT_VERSION
    else:
        git_version = RECOVERY_GIT_VERSION if recovery else PATCH_GIT_VERSION
    struct.pack_into("<I", data, GIT_VERSION_OFFSET, git_version)

    # QRing's custom transport writes the image directly but never invokes
    # Realtek's dfu_set_ready(), so ship the validated image as ready.
    if struct.unpack_from("<H", data, 0x52)[0] != STOCK_CTRL_FLAGS:
        raise SystemExit("image control flags do not match stock firmware")
    struct.pack_into("<H", data, 0x52, PATCH_CTRL_FLAGS)

    # A nonzero CRC16 makes Realtek validate the payload with BTX CRC instead
    # of the vendor-generated SHA field, whose build-time input is unavailable.
    assert crc16_btx(b"123456789") == 0xBB3D
    struct.pack_into("<H", data, 0x56, crc16_btx(data[0x450:]))

    # QRing's 0x50-byte wrapper stores the 32-bit byte sum of its payload.
    struct.pack_into("<I", data, 0x0C, sum(data[0x50:]) & 0xFFFFFFFF)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(data)

    written = destination.read_bytes()
    for name, offset, original, patched in patches:
        expected = original if recovery else patched
        if written[offset : offset + len(expected)] != expected:
            raise AssertionError(f"written {name} patch failed verification")
    assert struct.unpack_from("<I", written, GIT_VERSION_OFFSET)[0] == git_version
    assert struct.unpack_from("<H", written, 0x52)[0] == PATCH_CTRL_FLAGS
    assert struct.unpack_from("<H", written, 0x56)[0] == crc16_btx(written[0x450:])
    assert struct.unpack_from("<I", written, 0x0C)[0] == sum(written[0x50:]) & 0xFFFFFFFF
    print(f"wrote {destination} ({len(written)} bytes)")
    print(f"SHA-256 {hashlib.sha256(written).hexdigest()}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--recovery", action="store_true", help="restore stock code with newer OTA metadata")
    parser.add_argument("--compiled-dir", type=Path, help="verified assembler section binaries")
    parser.add_argument("--zwift", action="store_true", help="expose a standard BLE Heart Rate Service")
    parser.add_argument("--endurance", action="store_true", help="stable bounded daily-HR acquisition")
    args = parser.parse_args()
    patch_firmware(
        args.source,
        args.destination,
        args.recovery,
        args.compiled_dir,
        args.zwift,
        args.endurance,
    )


if __name__ == "__main__":
    main()
