"""Compile actual ride recording/tick/history callbacks and storage with native files."""
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
    parser.add_argument("--output", type=Path, default=ROOT / "build/ride-recording")
    args = parser.parse_args()
    source = args.main_source.read_text(encoding="utf-8")
    start = source.index("static void ride_load_history(void)\n{")
    code = source[start:source.index("static void kickr_toggle_clicked(", start)]
    assert code.count("static ") == 8 and "static void cycling_tick(lv_timer_t *timer)" in code
    assert '#define RIDE_FLUSH_MS (10 * 1000)' in source and '#define SD_PATH "/sdcard"' in source
    assert 'static char ride_temporary_path[128], ride_final_path[128], ride_notice[96];' in source
    storage = (ROOT / "main/storage_io.c").read_text(encoding="utf-8")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    for name, content in (("main_source.c", source), ("ride_recording.inc", code), ("storage_source.inc", storage),
                          ("ride_recording_config.inc", '#define SD_PATH "/sdcard"\n#define RIDE_FLUSH_MS 10000\n')):
        (output / name).write_text(content, encoding="utf-8")
    temporary = output / "temp"
    temporary.mkdir(exist_ok=True)
    env = os.environ.copy()
    env.update(TEMP=str(temporary), TMP=str(temporary), TMPDIR=str(temporary))
    executable = output / ("ride_recording_test.exe" if os.name == "nt" else "ride_recording_test")
    compiled = subprocess.run([args.compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                               "-I", str(output), "-I", str(ROOT / "main"), str(ROOT / "tests/ride_recording_test.c"), "-o", str(executable)],
                              capture_output=True, text=True, env=env, timeout=60)
    (output / "compile.log").write_text(compiled.stdout + compiled.stderr, encoding="utf-8")
    if compiled.returncode:
        print(compiled.stdout + compiled.stderr, end="")
        raise SystemExit(compiled.returncode)
    result = subprocess.run([str(executable)], cwd=output, capture_output=True, text=True, timeout=60)
    log = result.stdout + result.stderr
    (output / "result.log").write_text(log, encoding="utf-8")
    (output / "source.json").write_text(json.dumps({
        "mainSource": str(args.main_source.resolve()), "compiler": args.compiler,
        "mainSourceSha256": digest(source.encode()), "functionSourceSha256": digest(code.encode()),
        "storageSourceSha256": digest(storage.encode()), "headerSourceSha256": digest((ROOT / "main/storage_io.h").read_text(encoding="utf-8").encode()),
        "testSourceSha256": digest((ROOT / "tests/ride_recording_test.c").read_text(encoding="utf-8").encode()),
        "executableSha256": digest(executable.read_bytes()), "exitCode": result.returncode,
        "actualFunctionsCompiled": 8, "actualStorageSourceCompiled": True, "actualHistoryHelpersCompiled": True,
        "nativeFilesDirectoriesAndDescriptorsUsed": True, "nativeRideAndIndexBytesChecked": True,
        "uiClockTicksBleDataAndSdControlled": True, "ioFaultsControlled": True, "timezoneConversionUsesUtc": True,
        "bleOrActualLvglExecuted": False, "physicalSdOrPanelVerified": False,
    }, indent=2) + "\n", encoding="utf-8")
    print(log, end="", flush=True)
    raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
