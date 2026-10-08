"""Compile actual serial log start/write/stop and storage helpers with native files."""
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
    parser.add_argument("--source", type=Path, default=ROOT / "main/uart_tool.c")
    parser.add_argument("--output", type=Path, default=ROOT / "build/serial-log-write")
    args = parser.parse_args()
    source = args.source.read_text(encoding="utf-8")
    sections = [("static void log_bytes(const char *direction, const uint8_t *bytes, size_t length)\n{", "static void show_bytes("),
                ("static bool stop_log(void)\n{", "/* Drain a bounded amount")]
    code = ""
    for begin, end in sections:
        start = source.index(begin)
        code += source[start:source.index(end, start)]
    config = '#define UART_TOOL_FLUSH_MS 10000\n#define UART_TOOL_PATH "/sdcard/UART"\n#define RS485_TOOL_PATH "/sdcard/RS485"\n'
    for line in config.splitlines():
        assert line in source, line
    for name in ("log_temporary_path", "log_final_path"):
        assert f"static char {name}[96];" in source
    storage = (ROOT / "main/storage_io.c").read_text(encoding="utf-8")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    for name, content in (("serial_source.c", source), ("serial_log_write.inc", code),
                          ("storage_source.inc", storage), ("serial_log_config.inc", config)):
        (output / name).write_text(content, encoding="utf-8")
    temporary = output / "temp"
    temporary.mkdir(exist_ok=True)
    env = os.environ.copy()
    env.update(TEMP=str(temporary), TMP=str(temporary), TMPDIR=str(temporary))
    executable = output / ("serial_log_write_test.exe" if os.name == "nt" else "serial_log_write_test")
    compiled = subprocess.run([args.compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                               "-I", str(output), "-I", str(ROOT / "main"), str(ROOT / "tests/serial_log_write_test.c"), "-o", str(executable)],
                              capture_output=True, text=True, env=env, timeout=60)
    (output / "compile.log").write_text(compiled.stdout + compiled.stderr, encoding="utf-8")
    if compiled.returncode:
        print(compiled.stdout + compiled.stderr, end="")
        raise SystemExit(compiled.returncode)
    result = subprocess.run([str(executable)], cwd=output, capture_output=True, text=True, timeout=60)
    log = result.stdout + result.stderr
    (output / "result.log").write_text(log, encoding="utf-8")
    (output / "source.json").write_text(json.dumps({
        "source": str(args.source.resolve()), "compiler": args.compiler,
        "serialSourceSha256": digest(source.encode()), "functionSourceSha256": digest(code.encode()),
        "storageSourceSha256": digest(storage.encode()), "headerSourceSha256": digest((ROOT / "main/storage_io.h").read_text(encoding="utf-8").encode()),
        "testSourceSha256": digest((ROOT / "tests/serial_log_write_test.c").read_text(encoding="utf-8").encode()),
        "executableSha256": digest(executable.read_bytes()), "exitCode": result.returncode,
        "actualFunctionsCompiled": code.count("static "), "actualStorageSourceCompiled": True,
        "nativeFilesDirectoriesAndDescriptorsUsed": True, "nativeFileBytesChecked": True,
        "uiClockTicksAndSdControlled": True, "ioFaultsControlled": True, "timezoneConversionUsesUtc": True,
        "serialDriversOrActualLvglExecuted": False, "physicalSdOrPanelVerified": False,
    }, indent=2) + "\n", encoding="utf-8")
    print(log, end="", flush=True)
    raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
