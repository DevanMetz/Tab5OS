"""Check complete capture/serial CSV viewers with real LVGL and native file faults."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
FILES = ("capture_viewer.c", "capture_data.c", "serial_log_viewer.c", "serial_log_data.c")


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default=os.environ.get("CC", "clang"))
    parser.add_argument("--source-dir", type=Path, default=ROOT / "main")
    parser.add_argument("--lvgl-library", required=True, type=Path)
    parser.add_argument("--output", type=Path, default=ROOT / "build/csv-viewer")
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("This runner requires Windows and the cached Debug LVGL library")
    library = args.lvgl_library.resolve()
    if not library.is_file():
        parser.error("Cached LVGL library is missing; run tools/test_offline_ui.ps1 first")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    temporary = output / "temp"
    temporary.mkdir(exist_ok=True)
    env = os.environ.copy()
    env.update(TEMP=str(temporary), TMP=str(temporary), TMPDIR=str(temporary))
    flags = [args.compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-fms-runtime-lib=dll_dbg",
             "-D_CRT_SECURE_NO_WARNINGS", "-DLV_CONF_SKIP", "-DLV_KCONFIG_IGNORE", "-DLV_USE_OS=0",
             "-DLV_MEM_SIZE=98304", "-DLV_FONT_MONTSERRAT_28=1", "-DLV_FONT_MONTSERRAT_48=1", "-DLV_USE_FLOAT=0",
             "-DLV_ASSERT_HANDLER={abort();}", "-DLV_ASSERT_HANDLER_INCLUDE=<stdlib.h>",
             "-I", str(ROOT / "main"), "-I", str(ROOT / "managed_components/lvgl__lvgl"),
             "-I", str(ROOT / "tests/serial_log_host"), "-include", str(ROOT / "tests/serial_log_host/compat.h")]
    objects = []
    source_hashes = {}
    log = ""
    for name in (*FILES, "payload_clipboard.c"):
        source = (args.source_dir / name if name in FILES else ROOT / "main" / name).read_text(encoding="utf-8")
        snapshot = output / name
        snapshot.write_text(source, encoding="utf-8")
        source_hashes[f"main/{name}"] = digest(source.encode())
        object_file = output / (name + ".obj")
        command = flags + (["-include", str(ROOT / "tests/csv_viewer_host/io_faults.h")] if name in FILES else [])
        result = subprocess.run(command + ["-c", str(snapshot), "-o", str(object_file)], capture_output=True, text=True, env=env, timeout=60)
        log += result.stdout + result.stderr
        (output / "compile.log").write_text(log, encoding="utf-8")
        if result.returncode:
            print(log, end="")
            raise SystemExit(result.returncode)
        objects.append(str(object_file))
    executable = output / "csv_viewer_test.exe"
    command = flags + [str(ROOT / "tests/csv_viewer_test.c"), *objects, str(library), "-nostdlib",
                       "-Wl,-defaultlib:msvcrtd,-defaultlib:oldnames", "-o", str(executable)]
    compiled = subprocess.run(command, capture_output=True, text=True, env=env, timeout=60)
    log += compiled.stdout + compiled.stderr
    (output / "compile.log").write_text(log, encoding="utf-8")
    if compiled.returncode:
        print(log, end="")
        raise SystemExit(compiled.returncode)
    result = subprocess.run([str(executable)], cwd=output, capture_output=True, text=True, timeout=120)
    log = result.stdout + result.stderr
    (output / "result.log").write_text(log, encoding="utf-8")
    for name in ("capture_viewer.h", "capture_data.h", "serial_log_viewer.h", "serial_log_data.h", "payload_clipboard.h"):
        source_hashes[f"main/{name}"] = digest((ROOT / "main" / name).read_text(encoding="utf-8").encode())
    (output / "source.json").write_text(json.dumps({
        "compiler": args.compiler, "sourceFileSha256": source_hashes,
        "testSourceSha256": digest((ROOT / "tests/csv_viewer_test.c").read_text(encoding="utf-8").encode()),
        "ioAdapterSha256": digest((ROOT / "tests/csv_viewer_host/io_faults.h").read_text(encoding="utf-8").encode()),
        "executableSha256": digest(executable.read_bytes()), "lvglLibrarySha256": digest(library.read_bytes()),
        "exitCode": result.returncode, "completeViewerAndParserFilesCompiled": 4,
        "actualPayloadClipboardCompiled": True, "actualLvglLinked": True, "lvglPoolBytes": 98304,
        "uiApisControlled": False, "heapAndIoFaultsControlled": True, "nativeFileBytesChecked": True,
        "physicalSdOrPanelVerified": False,
    }, indent=2) + "\n", encoding="utf-8")
    print(log, end="", flush=True)
    raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
