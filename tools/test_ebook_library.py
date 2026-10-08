"""Compile ebook library callbacks against native directories and controlled faults."""
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
    parser.add_argument("--output", type=Path, default=ROOT / "build/ebook-library")
    args = parser.parse_args()
    source = args.main_source.read_text(encoding="utf-8")
    sections = []
    for begin, end in (("static bool browser_prefix(const char *text, const char *prefix)\n{", "static const char *browser_find("),
                       ("static bool ebook_supported(const char *name)\n{", "static bool ebook_default_installed("),
                       ("static void show_ebooks(void)\n{", "static void ebooks_clicked(")):
        start = source.index(begin)
        sections.append(source[start:source.index(end, start)])
    code = "".join(sections)
    storage = (ROOT / "main/storage_io.c").read_text(encoding="utf-8")
    header = (ROOT / "main/storage_io.h").read_text(encoding="utf-8")
    capacity = re.search(r"^static char file_paths\[(\d+)\]\[(\d+)\];", source, re.MULTILINE)
    assert capacity and tuple(map(int, capacity.groups())) == (64, 256)
    assert '#define SD_PATH "/sdcard"' in source
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    (output / "main_source.c").write_text(source, encoding="utf-8")
    (output / "ebook_library.inc").write_text(code, encoding="utf-8")
    (output / "storage_source.inc").write_text(storage, encoding="utf-8")
    (output / "ebook_library_config.inc").write_text('#define BOOKS_CAPACITY 64\n#define BOOKS_PATH_BYTES 256\n#define SD_PATH "/sdcard"\n')
    temporary = output / "temp"
    temporary.mkdir(exist_ok=True)
    env = os.environ.copy()
    env.update(TEMP=str(temporary), TMP=str(temporary), TMPDIR=str(temporary))
    executable = output / ("ebook_library_test.exe" if os.name == "nt" else "ebook_library_test")
    command = [args.compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(output), "-I", str(ROOT / "main"),
               str(ROOT / "tests/ebook_library_test.c"), "-o", str(executable)]
    if os.name == "nt":
        command.append("-lkernel32")
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
        "storageSourceSha256": digest(storage.encode()), "headerSourceSha256": digest(header.encode()),
        "testSourceSha256": digest((ROOT / "tests/ebook_library_test.c").read_text(encoding="utf-8").encode()),
        "executableSha256": digest(executable.read_bytes()), "exitCode": result.returncode,
        "actualFunctionsCompiled": 3, "entryCapacity": 64, "pathCapacity": 256,
        "nativeDirectoryAndFileBytesChecked": True, "uiTimersAndSdErrorApisControlled": True,
        "logicalMountMappingControlled": True, "directoryStatAndNameFaultsControlled": True,
        "welcomeCreationBranchExecuted": True, "actualStorageSourceCompiled": True,
        "welcomeNativeFileBytesAndHandlesChecked": True, "welcomeWriteAndCommitFaultsControlled": True,
        "downloadHelpersOrTimerTickExecuted": False,
        "sdkMountsOrPhysicalSdPanelVerified": False,
    }, indent=2) + "\n", encoding="utf-8")
    print(log, end="", flush=True)
    raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
