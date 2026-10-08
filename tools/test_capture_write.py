"""Compile actual Scope/I2C save callbacks and storage helpers with native files."""
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
    parser.add_argument("--output", type=Path, default=ROOT / "build/capture-write")
    args = parser.parse_args()
    source = args.main_source.read_text(encoding="utf-8")
    finish = "static bool i2c_capture_finish(int error)\n{"
    sections = [("static void scope_capture_clicked(lv_event_t *event)\n{", "static uint8_t bcd("),
                (finish if finish in source else "static bool i2c_capture_stop(void)\n{", "static bool i2c_capture_start("),
                ("static void i2c_capture_log(esp_err_t transaction_error, uint8_t value)\n{", "static void i2c_address_step_clicked(")]
    code = ""
    for begin, end in sections:
        start = source.index(begin)
        code += source[start:source.index(end, start)]
    assert '#define SCOPE_CHART_POINTS 300' in source and '#define I2C_CAPTURE_FLUSH_MS (10 * 1000)' in source
    assert '#define SCOPE_PATH SD_PATH "/SCOPE"' in source and '#define SD_PATH "/sdcard"' in source
    assert 'static char i2c_capture_notice[128];' in source and 'static char scope_capture_notice[128];' in source
    storage = (ROOT / "main/storage_io.c").read_text(encoding="utf-8")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    (output / "main_source.c").write_text(source, encoding="utf-8")
    (output / "capture_write.inc").write_text(code, encoding="utf-8")
    (output / "storage_source.inc").write_text(storage, encoding="utf-8")
    (output / "capture_write_config.inc").write_text('#define SD_PATH "/sdcard"\n#define SCOPE_PATH SD_PATH "/SCOPE"\n#define SCOPE_CHART_POINTS 300\n#define I2C_CAPTURE_FLUSH_MS 10000\n')
    temporary = output / "temp"
    temporary.mkdir(exist_ok=True)
    env = os.environ.copy()
    env.update(TEMP=str(temporary), TMP=str(temporary), TMPDIR=str(temporary))
    executable = output / ("capture_write_test.exe" if os.name == "nt" else "capture_write_test")
    compiled = subprocess.run([args.compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                               "-I", str(output), "-I", str(ROOT / "main"), str(ROOT / "tests/capture_write_test.c"), "-o", str(executable)],
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
        "testSourceSha256": digest((ROOT / "tests/capture_write_test.c").read_text(encoding="utf-8").encode()),
        "executableSha256": digest(executable.read_bytes()), "exitCode": result.returncode,
        "actualFunctionsCompiled": code.count("static "), "actualStorageSourceCompiled": True,
        "scopeChartPoints": 300, "nativeFileBytesChecked": True, "uiClockTicksSamplesAndSdControlled": True,
        "ioFaultsControlled": True, "timezoneConversionUsesUtc": True, "i2cStartOrBusOrAdcExecuted": False,
        "physicalSdOrPanelVerified": False,
    }, indent=2) + "\n", encoding="utf-8")
    print(log, end="", flush=True)
    raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
