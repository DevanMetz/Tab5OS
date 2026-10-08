"""Compile actual Ring log/checkpoint/timer callbacks with native file faults."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default=os.environ.get("CC", "cc"))
    parser.add_argument("--main-source", type=Path, default=ROOT / "main/main.c")
    parser.add_argument("--output", type=Path, default=ROOT / "build/ring-history")
    args = parser.parse_args()
    source = args.main_source.read_text(encoding="utf-8")
    start = source.index("static bool ring_hr_append_log(time_t sample_time, int heart_rate)\n{")
    code = source[start:source.index("static int ring_request_history(void)", start)]
    start = source.index("static void ring_health_tick(lv_timer_t *timer)\n{")
    code += source[start:source.index("static int ring_subscribed(", start)]
    assert code.count("static ") == 3 and "ring_hr_load_last_saved(void)" in code
    storage = (ROOT / "main/storage_io.c").read_text(encoding="utf-8")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    (output / "main_source.c").write_text(source, encoding="utf-8")
    (output / "ring_history.inc").write_text(code, encoding="utf-8")
    (output / "storage_source.inc").write_text(storage, encoding="utf-8")
    executable = output / ("ring_history_test.exe" if os.name == "nt" else "ring_history_test")
    temporary = output / "temp"
    temporary.mkdir(exist_ok=True)
    env = os.environ.copy()
    env.update(TEMP=str(temporary), TMP=str(temporary), TMPDIR=str(temporary))
    command = [args.compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
               "-I", str(output), "-I", str(ROOT / "main"),
               str(ROOT / "tests/ring_history_test.c"), "-o", str(executable)]
    compiled = subprocess.run(command, capture_output=True, text=True, env=env, timeout=60)
    (output / "compile.log").write_text(compiled.stdout + compiled.stderr)
    if compiled.returncode:
        print(compiled.stdout + compiled.stderr, end="")
        raise SystemExit(compiled.returncode)
    result = subprocess.run([str(executable)], cwd=output, capture_output=True, text=True, timeout=90)
    log = result.stdout + result.stderr
    (output / "result.log").write_text(log)
    (output / "source.json").write_text(json.dumps({
        "mainSource": str(args.main_source.resolve()), "compiler": args.compiler,
        "mainSourceSha256": digest(source.encode()), "functionSourceSha256": digest(code.encode()),
        "storageSourceSha256": digest(storage.encode()),
        "headerSourceSha256": digest((ROOT / "main/storage_io.h").read_text(encoding="utf-8").encode()),
        "testSourceSha256": digest((ROOT / "tests/ring_history_test.c").read_text(encoding="utf-8").encode()),
        "executableSha256": digest(executable.read_bytes()), "exitCode": result.returncode,
        "actualFunctionsCompiled": 3, "actualStorageSourceCompiled": True,
        "nativeFileBytesChecked": True, "queueAndTimerApisControlled": True,
        "timezoneConversionUsesUtc": True, "sdErrorApiControlled": True,
        "ioFaultsControlled": True, "physicalSdOrBleVerified": False,
    }, indent=2) + "\n")
    print(log, end="", flush=True)
    raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
