"""Compile the actual BLE evidence save callback and storage with native files."""
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
    parser.add_argument("--source", type=Path, default=ROOT / "main/ble_tool.c")
    parser.add_argument("--output", type=Path, default=ROOT / "build/ble-evidence")
    args = parser.parse_args()
    source = args.source.read_text(encoding="utf-8")
    code = ""
    for begin, end in (("static void set_status(const char *format, ...)\n{", "static void request_view("),
                       ("static void address_text(", "static void format_value("),
                       ("static void save_evidence_clicked(lv_event_t *event)\n{", "static void render_scan_view(")):
        start = source.index(begin)
        code += source[start:source.index(end, start)]
    assert code.count("static ") == 5
    config = ""
    for name, count in (("DEVICE", 8), ("SERVICE", 16), ("CHARACTERISTIC", 32), ("VALUE", 64)):
        line = f"#define BLE_TOOL_{name}_MAX {count}\n"
        assert line in source
        config += line
    assert 'static char status_text[192]' in source and 'char temporary_path[48]' in code and 'char final_path[48]' in code
    start = source.index("typedef struct {")
    types = source[start:source.index("static const char *TAG", start)]
    assert types.count("typedef struct") == 3
    storage = (ROOT / "main/storage_io.c").read_text(encoding="utf-8")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    for name, content in (("ble_source.c", source), ("ble_evidence.inc", code), ("ble_evidence_types.inc", types),
                          ("ble_evidence_config.inc", config), ("storage_source.inc", storage)):
        (output / name).write_text(content, encoding="utf-8")
    temporary = output / "temp"
    temporary.mkdir(exist_ok=True)
    env = os.environ.copy()
    env.update(TEMP=str(temporary), TMP=str(temporary), TMPDIR=str(temporary))
    executable = output / ("ble_evidence_test.exe" if os.name == "nt" else "ble_evidence_test")
    compiled = subprocess.run([args.compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(output),
                               "-I", str(ROOT / "main"), str(ROOT / "tests/ble_evidence_test.c"), "-o", str(executable)],
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
        "bleSourceSha256": digest(source.encode()), "functionSourceSha256": digest(code.encode()),
        "cacheTypesSourceSha256": digest(types.encode()), "storageSourceSha256": digest(storage.encode()),
        "headerSourceSha256": digest((ROOT / "main/storage_io.h").read_text(encoding="utf-8").encode()),
        "testSourceSha256": digest((ROOT / "tests/ble_evidence_test.c").read_text(encoding="utf-8").encode()),
        "executableSha256": digest(executable.read_bytes()), "exitCode": result.returncode,
        "actualFunctionsCompiled": 5, "actualCacheTypesCompiled": 3, "actualStorageSourceCompiled": True,
        "nativeFilesDirectoriesAndDescriptorsUsed": True, "nativeFileBytesChecked": True,
        "clockLocksBleCacheAndUuidConversionControlled": True, "ioFaultsControlled": True, "timezoneConversionUsesUtc": True,
        "nimbleOrActualLvglExecuted": False, "physicalSdOrPanelVerified": False,
    }, indent=2) + "\n", encoding="utf-8")
    print(log, end="", flush=True)
    raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
