"""Compile actual ebook worker/admission/timer and restart guards with controlled APIs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def digest(data):
    return hashlib.sha256(data.encode() if isinstance(data, str) else data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default=os.environ.get("CC", "cc"))
    parser.add_argument("--main-source", type=Path, default=ROOT / "main/main.c")
    parser.add_argument("--output", type=Path, default=ROOT / "build/ebook-worker")
    args = parser.parse_args()
    source = args.main_source.read_text(encoding="utf-8")
    start = source.index("static void ebook_download_task(void *argument)\n{")
    code = source[start:source.index("static void ebook_load_page(", start)]
    assert code.count("static void ") == 3 and code.count("static bool ") == 1
    start = source.index("static const char *restart_blocker(void)\n{")
    code += source[start:source.index("static void ota_update_task(", start)]
    types = re.search(r"typedef struct \{\n    const char \*filename;\n    const char \*url;\n\} ebook_default_t;", source).group(0) + "\n"
    defaults = re.search(r"^static const ebook_default_t ebook_defaults\[\] = \{.*?^\};", source, re.M | re.S).group(0)
    declarations = []
    handle = re.search(r"^static TaskHandle_t ebook_download_task_handle;", source, re.M)
    if handle:
        declarations.append(handle.group(0))
    for name in ("busy", "done"):
        declarations.append(re.search(rf"^static (?:volatile bool|atomic_bool) ebook_download_{name};", source, re.M).group(0))
    signals = "\n".join(declarations) + "\n"
    atomic = "atomic_bool" in signals
    assert (signals.count("atomic_bool") == 2) if atomic else (signals.count("volatile bool") == 2)
    config = f"#define EBOOK_LEGACY_HANDLE {int(bool(handle))}\n#define EBOOK_SIGNALS_ATOMIC {int(atomic)}\n" + defaults + "\n"
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    for name, text in (("main_source.c", source), ("ebook_worker.inc", code), ("ebook_worker_types.inc", types),
                       ("ebook_worker_signals.inc", signals), ("ebook_worker_config.inc", config)):
        (output / name).write_text(text, encoding="utf-8")
    temporary = output / "temp"
    temporary.mkdir(exist_ok=True)
    env = os.environ.copy()
    env.update(TEMP=str(temporary), TMP=str(temporary), TMPDIR=str(temporary))
    executable = output / ("ebook_worker_test.exe" if os.name == "nt" else "ebook_worker_test")
    command = [args.compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(output),
               str(ROOT / "tests/ebook_worker_test.c"), "-o", str(executable)]
    compiled = subprocess.run(command, capture_output=True, text=True, env=env, timeout=60)
    (output / "compile.log").write_text(compiled.stdout + compiled.stderr, encoding="utf-8")
    if compiled.returncode:
        print(compiled.stdout + compiled.stderr, end="")
        raise SystemExit(compiled.returncode)
    result = subprocess.run([str(executable)], cwd=output, capture_output=True, text=True, timeout=15)
    log = result.stdout + result.stderr
    (output / "result.log").write_text(log, encoding="utf-8")
    (output / "source.json").write_text(json.dumps({
        "mainSource": str(args.main_source.resolve()), "compiler": args.compiler,
        "mainSourceSha256": digest(source), "functionSourceSha256": digest(code),
        "typesSourceSha256": digest(types), "signalsSourceSha256": digest(signals), "configSourceSha256": digest(config),
        "testSourceSha256": digest((ROOT / "tests/ebook_worker_test.c").read_text(encoding="utf-8")),
        "executableSha256": digest(executable.read_bytes()), "exitCode": result.returncode,
        "actualFunctionsCompiled": 5, "actualTypesCompiled": 1, "actualSignalDeclarationsCompiled": len(declarations),
        "actualAtomicOperationsCompiled": atomic, "clockRatesHzChecked": [100, 1000],
        "taskAllocationAndDeletionApisControlled": True, "signalAccessAndInterleavingControlled": True,
        "wifiSdAndDownloadApisControlled": True, "timerAndOtherRestartOwnersControlled": True,
        "actualSdkSchedulerOrLvglExecuted": False, "physicalSdOrWifiVerified": False,
    }, indent=2) + "\n", encoding="utf-8")
    print(log, end="", flush=True)
    raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
