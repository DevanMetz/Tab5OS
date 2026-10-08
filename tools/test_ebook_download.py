"""Check actual ebook download callbacks and storage with native file faults."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def section(source, begin, end):
    start = source.index(begin)
    return source[start:source.index(end, start)]


def struct(source, name):
    end = source.index(f"}} {name};") + len(f"}} {name};")
    return source[source.rindex("typedef struct {", 0, end):end] + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default=os.environ.get("CC", "cc"))
    parser.add_argument("--main-source", type=Path, default=ROOT / "main/main.c")
    parser.add_argument("--output", type=Path, default=ROOT / "build/ebook-download")
    args = parser.parse_args()
    source = args.main_source.read_text(encoding="utf-8")
    code = section(source, "static esp_err_t ebook_http_event(", "static bool start_voice_mic(") + section(
        source, "static bool ebook_default_installed(", "static void ebook_download_task(")
    assert code.count("static ") == 3
    has_state = "} ebook_download_state_t;" in source
    types = struct(source, "ebook_default_t") + (struct(source, "ebook_download_state_t") if has_state else "")
    defaults = section(source, "static const ebook_default_t ebook_defaults[]", "#define GPIO_CONTROL")
    sd = next(line for line in source.splitlines() if line.startswith("#define SD_PATH "))
    config = sd + f"\n#define EBOOK_HAS_DOWNLOAD_STATE {int(has_state)}\n" + defaults
    storage = (ROOT / "main/storage_io.c").read_text(encoding="utf-8")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    for name, content in (("main_source.c", source), ("ebook_download.inc", code), ("ebook_download_types.inc", types),
                          ("ebook_download_config.inc", config), ("storage_source.inc", storage)):
        (output / name).write_text(content, encoding="utf-8")
    temporary = output / "temp"
    temporary.mkdir(exist_ok=True)
    env = os.environ.copy()
    env.update(TEMP=str(temporary), TMP=str(temporary), TMPDIR=str(temporary))
    executable = output / ("ebook_download_test.exe" if os.name == "nt" else "ebook_download_test")
    compiled = subprocess.run([args.compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(output),
                               "-I", str(ROOT / "main"), str(ROOT / "tests/ebook_download_test.c"), "-o", str(executable)],
                              capture_output=True, text=True, env=env, timeout=60)
    (output / "compile.log").write_text(compiled.stdout + compiled.stderr, encoding="utf-8")
    if compiled.returncode:
        print(compiled.stdout + compiled.stderr, end="")
        raise SystemExit(compiled.returncode)
    result = subprocess.run([str(executable)], cwd=output, capture_output=True, text=True, timeout=90)
    log = result.stdout + result.stderr
    (output / "result.log").write_text(log, encoding="utf-8")
    (output / "source.json").write_text(json.dumps({
        "mainSource": str(args.main_source.resolve()), "compiler": args.compiler,
        "mainSourceSha256": digest(source.encode()), "functionSourceSha256": digest(code.encode()),
        "typesSourceSha256": digest(types.encode()), "configSourceSha256": digest(config.encode()),
        "storageSourceSha256": digest(storage.encode()),
        "headerSourceSha256": digest((ROOT / "main/storage_io.h").read_text(encoding="utf-8").encode()),
        "testSourceSha256": digest((ROOT / "tests/ebook_download_test.c").read_text(encoding="utf-8").encode()),
        "executableSha256": digest(executable.read_bytes()), "exitCode": result.returncode,
        "actualFunctionsCompiled": 3, "actualTypesCompiled": 2 if has_state else 1, "actualStorageSourceCompiled": True,
        "nativeFileBytesChecked": True, "ioFaultsControlled": True, "httpEventsAndClientResultsControlled": True,
        "nativeDirectoriesChecked": True, "directorySizesAndSpecialPathKindsControlled": True,
        "laterEventsDeliveredDespiteCallbackFailure": True, "actualSdkOrUiExecuted": False,
        "physicalSdOrTlsVerified": False,
    }, indent=2) + "\n", encoding="utf-8")
    print(log, end="", flush=True)
    raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
