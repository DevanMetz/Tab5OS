"""Check actual Notes callbacks and storage helpers with native files."""
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
    parser.add_argument("--output", type=Path, default=ROOT / "build/notes-io")
    parser.add_argument("--lvgl-library", type=Path, help="Also exercise the real UI with the cached Debug LVGL library")
    args = parser.parse_args()
    source = args.main_source.read_text(encoding="utf-8")
    start = source.find("static void notes_leave(void)\n{")
    has_leave = start >= 0
    if not has_leave:
        start = source.index("static void save_note(lv_event_t *event)\n{")
    code = source[start:source.index("static void update_counter(void)", start)]
    assert "static void notes_clicked(lv_event_t *event)" in code
    limit = re.search(r"^#define NOTE_MAX_BYTES (\d+)\s*$", source, re.MULTILINE)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    (output / "main_source.c").write_text(source, encoding="utf-8")
    (output / "notes_callbacks.inc").write_text(code, encoding="utf-8")
    (output / "notes_config.inc").write_text(
        f"#define NOTES_HAS_LEAVE {int(has_leave)}\n#define NOTE_MAX_BYTES {limit.group(1) if limit else 65535}\n")
    storage = (ROOT / "main/storage_io.c").read_text(encoding="utf-8")
    (output / "storage_source.inc").write_text(storage, encoding="utf-8")
    executable = output / ("notes_io_test.exe" if os.name == "nt" else "notes_io_test")
    temporary = output / "temp"
    temporary.mkdir(exist_ok=True)
    env = os.environ.copy()
    env.update(TEMP=str(temporary), TMP=str(temporary), TMPDIR=str(temporary))
    command = [args.compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
               "-I", str(output), "-I", str(ROOT / "main"),
               str(ROOT / "tests/notes_io_test.c"), "-o", str(executable)]
    if args.lvgl_library:
        command += ["-DNOTES_REAL_LVGL", "-DLV_CONF_SKIP", "-DLV_KCONFIG_IGNORE", "-DLV_USE_OS=0",
                    "-DLV_MEM_SIZE=98304", "-DLV_FONT_MONTSERRAT_28=1", "-DLV_FONT_MONTSERRAT_48=1",
                    "-DLV_USE_FLOAT=0", "-DLV_ASSERT_HANDLER={abort();}", "-DLV_ASSERT_HANDLER_INCLUDE=<stdlib.h>",
                    "-I", str(ROOT / "managed_components/lvgl__lvgl"), str(args.lvgl_library.resolve())]
        if os.name == "nt":
            command += ["-fms-runtime-lib=dll_dbg", "-nostdlib", "-Wl,-defaultlib:msvcrtd,-defaultlib:oldnames"]
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
        "testSourceSha256": digest((ROOT / "tests/notes_io_test.c").read_text(encoding="utf-8").encode()),
        "executableSha256": digest(executable.read_bytes()), "exitCode": result.returncode,
        "actualFunctionsCompiled": 4 if has_leave else 2, "actualStorageSourceCompiled": True,
        "nativeFileBytesChecked": True, "uiApisControlled": not bool(args.lvgl_library),
        "heapApisControlled": True, "actualLvglLinked": bool(args.lvgl_library),
        "lvglLibrarySha256": digest(args.lvgl_library.read_bytes()) if args.lvgl_library else None,
        "ioFaultsControlled": True, "physicalSdOrPowerLossVerified": False,
    }, indent=2) + "\n")
    print(log, end="", flush=True)
    raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
