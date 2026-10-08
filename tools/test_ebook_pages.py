"""Compile actual ebook page/navigation callbacks with native file faults."""
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
    parser.add_argument("--output", type=Path, default=ROOT / "build/ebook-pages")
    args = parser.parse_args()
    source = args.main_source.read_text(encoding="utf-8")
    match = re.search(r"^static void ebook_load_page\((void|long offset)\)\n\{", source, re.MULTILINE)
    assert match
    transactional = match.group(1) == "long offset"
    code = source[match.start():source.index("static void ebook_library_clicked(", match.start())]
    start = source.index("static void ebook_prev_clicked(lv_event_t *event)\n{")
    code += source[start:source.index("static void ebook_text_clicked(", start)]
    assert code.count("static ") == 3
    start = source.index("static void show_ebook_reader(const char *path)\n{")
    initialization = source[start:source.index("static void ebook_open_clicked(", start)]
    if transactional:
        assert "ebook_next_offset = 0;" in initialization
        assert "lv_obj_add_state(ebook_prev, LV_STATE_DISABLED);" in initialization
        assert "lv_obj_add_state(ebook_next, LV_STATE_DISABLED);" in initialization
        assert "ebook_load_page(0);" in initialization
    page_bytes = re.search(r"^#define EBOOK_PAGE_BYTES (\d+)\s*$", source, re.MULTILINE)
    assert page_bytes and int(page_bytes.group(1)) == 8192
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    (output / "main_source.c").write_text(source, encoding="utf-8")
    (output / "ebook_pages.inc").write_text(code, encoding="utf-8")
    (output / "reader_initialization.inc").write_text(initialization, encoding="utf-8")
    (output / "ebook_config.inc").write_text(f"#define EBOOK_TRANSACTIONAL {int(transactional)}\n#define EBOOK_PAGE_BYTES {page_bytes.group(1)}\n")
    executable = output / ("ebook_pages_test.exe" if os.name == "nt" else "ebook_pages_test")
    temporary = output / "temp"
    temporary.mkdir(exist_ok=True)
    env = os.environ.copy()
    env.update(TEMP=str(temporary), TMP=str(temporary), TMPDIR=str(temporary))
    command = [args.compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
               "-I", str(output), str(ROOT / "tests/ebook_pages_test.c"), "-o", str(executable)]
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
        "readerInitializationSourceSha256": digest(initialization.encode()),
        "testSourceSha256": digest((ROOT / "tests/ebook_pages_test.c").read_text(encoding="utf-8").encode()),
        "executableSha256": digest(executable.read_bytes()), "exitCode": result.returncode,
        "actualFunctionsCompiled": 3, "initializationSourceCheckedOnly": True,
        "transactionalLoadSignature": transactional, "pageBytes": int(page_bytes.group(1)),
        "nativeFileBytesChecked": True, "uiAndHeapApisControlled": True,
        "sdErrorApiControlled": True, "ioFaultsControlled": True,
        "physicalSdOrPanelVerified": False,
    }, indent=2) + "\n")
    print(log, end="", flush=True)
    raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
