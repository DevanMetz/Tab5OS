"""Compile the actual Files text-preview callback with native read/close faults."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default=os.environ.get("CC", "cc"))
    parser.add_argument("--main-source", type=Path, default=ROOT / "main/main.c")
    parser.add_argument("--output", type=Path, default=ROOT / "build/file-preview")
    args = parser.parse_args()
    source = args.main_source.read_text(encoding="utf-8")
    start = source.index("static void open_file(const char *path)\n{")
    code = source[start:source.index("static void file_clicked(", start)]
    assert code.count("static void ") == 1
    size = re.search(r"static char text\[(\d+)\];", code)
    sd_path = re.search(r'^#define SD_PATH "([^"]+)"$', source, re.MULTILINE)
    assert size and int(size.group(1)) == 4096 and sd_path and sd_path.group(1) == "/sdcard"
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    (output / "main_source.c").write_text(source, encoding="utf-8")
    (output / "file_preview.inc").write_text(code, encoding="utf-8")
    (output / "file_preview_config.inc").write_text(f'#define FILE_PREVIEW_BYTES {size.group(1)}\n#define SD_PATH "{sd_path.group(1)}"\n')
    temporary = output / "temp"
    temporary.mkdir(exist_ok=True)
    env = os.environ.copy()
    env.update(TEMP=str(temporary), TMP=str(temporary), TMPDIR=str(temporary))
    executable = output / ("file_preview_test.exe" if os.name == "nt" else "file_preview_test")
    command = [args.compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
               "-I", str(output), str(ROOT / "tests/file_preview_test.c"), "-o", str(executable)]
    compiled = subprocess.run(command, capture_output=True, text=True, env=env, timeout=60)
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
        "testSourceSha256": digest((ROOT / "tests/file_preview_test.c").read_text(encoding="utf-8").encode()),
        "executableSha256": digest(executable.read_bytes()), "exitCode": result.returncode,
        "actualFunctionsCompiled": 1, "previewBufferBytes": int(size.group(1)),
        "nativeFileBytesChecked": True, "uiAndCsvProbeApisControlled": True,
        "logicalPathMappingControlled": True, "sdErrorApiControlled": True, "ioFaultsControlled": True,
        "physicalStorageOrPanelVerified": False,
    }, indent=2) + "\n", encoding="utf-8")
    print(log, end="", flush=True)
    raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
