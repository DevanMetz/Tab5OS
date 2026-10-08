"""Check actual CSV tail recovery with native files and controlled I/O faults."""
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
    parser.add_argument("--storage-source", type=Path, default=ROOT / "main/storage_io.c")
    parser.add_argument("--output", type=Path, default=ROOT / "build/storage-csv-tail")
    args = parser.parse_args()
    source = args.storage_source.read_text(encoding="utf-8")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    (output / "storage_source.inc").write_text(source, encoding="utf-8")
    executable = output / ("storage_csv_tail_test.exe" if os.name == "nt" else "storage_csv_tail_test")
    temporary = output / "temp"
    temporary.mkdir(exist_ok=True)
    env = os.environ.copy()
    env.update(TEMP=str(temporary), TMP=str(temporary), TMPDIR=str(temporary))
    command = [args.compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
               "-I", str(output), "-I", str(ROOT / "main"),
               str(ROOT / "tests/storage_csv_tail_test.c"), "-o", str(executable)]
    compile_result = subprocess.run(command, capture_output=True, text=True, env=env, timeout=60)
    (output / "compile.log").write_text(compile_result.stdout + compile_result.stderr)
    if compile_result.returncode:
        print(compile_result.stdout + compile_result.stderr, end="")
        raise SystemExit(compile_result.returncode)
    result = subprocess.run([str(executable)], cwd=output, capture_output=True, text=True, timeout=90)
    log = result.stdout + result.stderr
    (output / "result.log").write_text(log)
    (output / "source.json").write_text(json.dumps({
        "storageSource": str(args.storage_source.resolve()), "compiler": args.compiler,
        "storageSourceSha256": digest(source.encode()),
        "headerSourceSha256": digest((ROOT / "main/storage_io.h").read_text(encoding="utf-8").encode()),
        "testSourceSha256": digest((ROOT / "tests/storage_csv_tail_test.c").read_text(encoding="utf-8").encode()),
        "executableSha256": digest(executable.read_bytes()), "exitCode": result.returncode,
        "actualStorageSourceCompiled": True, "nativeFileBytesChecked": True,
        "ioFaultsControlled": True, "physicalSdOrPowerLossVerified": False,
    }, indent=2) + "\n")
    print(log, end="", flush=True)
    raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
