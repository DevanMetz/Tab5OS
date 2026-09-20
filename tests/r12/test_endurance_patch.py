import hashlib
import struct
import unittest
from pathlib import Path

from unicorn import Uc, UC_ARCH_ARM, UC_HOOK_CODE, UC_MODE_LITTLE_ENDIAN, UC_MODE_THUMB
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_SP


ROOT = Path(__file__).resolve().parents[2]
STOCK = Path(r"C:\Users\metzd\AppData\Local\Temp\colmi-r12-firmware\RT11CR_1.00.09_260424.bin")
CANDIDATE = ROOT / "artifacts/r12/RT11CR_1.01.05_hr-endurance.bin"
FILE_BASE = 0x825FB0
VALIDATOR = 0x834460
PERIODIC_CALLBACK = 0x834418
GET_EPOCH_SECONDS = 0x8279EA
SENSOR_STATUS = 0x83583C
GET_LIVE_HR = 0x835842
WRITE_HR_HISTORY = 0x834378
STOP_PERIODIC_HR = 0x834604
RETAINED_STATE = 0x20C0EC
PERIODIC_ACTIVE = 0x20C104
PERIODIC_STATE = 0x20C10C
SENTINEL = 0x820100


class EnduranceEmulator:
    def __init__(self):
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_LITTLE_ENDIAN)
        self.uc.mem_map(0x820000, 0x100000)
        self.uc.mem_map(0x200000, 0x20000)
        self.uc.mem_write(FILE_BASE, CANDIDATE.read_bytes())
        self.uc.reg_write(UC_ARM_REG_SP, 0x21FFF0)
        self.epoch = 0
        self.sensor_status = 2
        self.live_hr = 0
        self.history = []
        self.stop_count = 0
        self.uc.hook_add(UC_HOOK_CODE, self._hook)

    def _return(self):
        self.uc.reg_write(UC_ARM_REG_PC, self.uc.reg_read(UC_ARM_REG_LR))

    def _hook(self, uc, address, size, _user):
        if address == SENTINEL:
            uc.emu_stop()
        elif address == GET_EPOCH_SECONDS:
            uc.reg_write(UC_ARM_REG_R0, self.epoch)
            self._return()
        elif address == SENSOR_STATUS:
            uc.reg_write(UC_ARM_REG_R0, self.sensor_status)
            self._return()
        elif address == GET_LIVE_HR:
            uc.reg_write(UC_ARM_REG_R0, self.live_hr)
            self._return()
        elif address == WRITE_HR_HISTORY:
            self.history.append((uc.reg_read(UC_ARM_REG_R0), uc.reg_read(UC_ARM_REG_R1)))
            self._return()
        elif address == STOP_PERIODIC_HR:
            self.stop_count += 1
            self._return()

    def call(self, address, r0=0):
        self.uc.reg_write(UC_ARM_REG_R0, r0)
        self.uc.reg_write(UC_ARM_REG_LR, SENTINEL | 1)
        self.uc.reg_write(UC_ARM_REG_PC, address | 1)
        self.uc.emu_start(address | 1, 0, count=10000)
        return self.uc.reg_read(UC_ARM_REG_R0)

    def retained(self):
        raw = bytes(self.uc.mem_read(RETAINED_STATE, 12))
        return {
            "bpm": raw[0],
            "candidate": raw[1],
            "count": raw[2],
            "candidate_epoch": struct.unpack_from("<I", raw, 4)[0],
            "retained_epoch": struct.unpack_from("<I", raw, 8)[0],
        }


class EnduranceFirmwareTests(unittest.TestCase):
    def test_artifact_hash_and_stock_recovery_are_fixed(self):
        candidate = CANDIDATE.read_bytes()
        recovery = (ROOT / "artifacts/r12/RT11CR_1.01.06_stock-recovery.bin").read_bytes()
        self.assertEqual(hashlib.sha256(candidate).hexdigest(), "59758ecbec249f332a524b62e54d75d0d5d696c292e9aaf63dc5ec0a89dd8c13")
        self.assertEqual(hashlib.sha256(recovery).hexdigest(), "97fc119d7783ed5ec67cb3e624ce91db5d33aab829b6fb44f81d822a90f31b0d")

    def test_alternating_80_120_never_validates(self):
        emu = EnduranceEmulator()
        for epoch, bpm in enumerate([80, 120] * 8, 100):
            emu.epoch = epoch
            self.assertEqual(emu.call(VALIDATOR, bpm), 0)
        self.assertEqual(emu.retained()["bpm"], 0)

    def test_three_stable_distinct_samples_preserve_bpm_and_age(self):
        emu = EnduranceEmulator()
        for epoch, bpm, expected in [(100, 72, 0), (101, 74, 0), (102, 73, 73)]:
            emu.epoch = epoch
            self.assertEqual(emu.call(VALIDATOR, bpm), expected)
        state = emu.retained()
        self.assertEqual(state["bpm"], 73)
        self.assertEqual(state["retained_epoch"], 102)

    def test_duplicate_calls_in_one_second_do_not_fake_stability(self):
        emu = EnduranceEmulator()
        emu.epoch = 100
        for _ in range(5):
            self.assertEqual(emu.call(VALIDATOR, 72), 0)
        self.assertEqual(emu.retained()["count"], 1)

    def test_periodic_stable_result_finishes_at_17_seconds(self):
        emu = EnduranceEmulator()
        emu.uc.mem_write(PERIODIC_ACTIVE, b"\x01")
        emu.uc.mem_write(PERIODIC_STATE, struct.pack("<I", 123456))
        for second in range(1, 18):
            emu.epoch = 1000 + second
            emu.live_hr = 71
            emu.call(PERIODIC_CALLBACK)
        self.assertEqual(emu.history, [(123456, 71)])
        self.assertEqual(emu.stop_count, 1)

    def test_periodic_unstable_result_stops_at_30_seconds_without_history(self):
        emu = EnduranceEmulator()
        emu.uc.mem_write(PERIODIC_ACTIVE, b"\x01")
        emu.uc.mem_write(PERIODIC_STATE, struct.pack("<I", 123456))
        for second in range(1, 31):
            emu.epoch = 2000 + second
            emu.live_hr = 80 if second % 2 else 120
            emu.call(PERIODIC_CALLBACK)
        self.assertEqual(emu.history, [])
        self.assertEqual(emu.stop_count, 1)

    def test_periodic_vendor_not_ready_stops_at_30_seconds(self):
        emu = EnduranceEmulator()
        emu.sensor_status = 1
        for second in range(1, 31):
            emu.epoch = 3000 + second
            emu.call(PERIODIC_CALLBACK)
        self.assertEqual(emu.history, [])
        self.assertEqual(emu.stop_count, 1)


if __name__ == "__main__":
    unittest.main()
