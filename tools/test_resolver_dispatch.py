"""Compile the actual shared resolver with controlled core-dispatch/clock models."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
def digest(raw):
    return hashlib.sha256(raw).hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default=os.environ.get("CC", "cc"))
    parser.add_argument("--resolver-source", type=Path, default=ROOT / "main/network_resolver.c")
    parser.add_argument("--output", type=Path, default=ROOT / "build/resolver-dispatch")
    args = parser.parse_args()
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=True)
    sources = output / "sources"
    inputs = ["main/network_resolver.c", "main/network_resolver.h", "tests/resolver_dispatch_test.c", "tools/test_resolver_dispatch.py"]
    inputs += [path.relative_to(ROOT).as_posix() for path in sorted((ROOT / "tests/resolver_dispatch_host/stubs").rglob("*.h"))]
    hashes = {}
    for name in inputs:
        original = args.resolver_source if name == "main/network_resolver.c" else ROOT / name
        raw = original.read_text(encoding="utf-8-sig").encode()
        target = sources / name; target.parent.mkdir(parents=True, exist_ok=True); target.write_bytes(raw)
        hashes[name] = digest(raw)
    temporary = output / "temp"; temporary.mkdir(exist_ok=True)
    env = os.environ.copy(); env.update(TEMP=str(temporary), TMP=str(temporary), TMPDIR=str(temporary))
    metadata = {"compiler": args.compiler, "normalizedSourceSha256": hashes, "runs": {},
                "actualResolverSourceCompiled": False, "clockAndCoreDispatchControlled": True,
                "sdkTypesAndLockApisControlled": True, "addressCopyCanariesAndSlotReuseChecked": False,
                "actualSdkSchedulerOrDnsNetworkExecuted": False, "exitCode": 0}
    completed = 0
    for capacity in (1, 4):
        executable = output / (f"resolver-{capacity}.exe" if os.name == "nt" else f"resolver-{capacity}")
        compiled = subprocess.run([args.compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                                  f"-DDNS_MAX_HOST_IP={capacity}", "-I", str(sources / "tests/resolver_dispatch_host/stubs"),
                                  "-I", str(sources / "main"), str(sources / "main/network_resolver.c"),
                                  str(sources / "tests/resolver_dispatch_test.c"), "-o", str(executable)],
                                 capture_output=True, text=True, env=env, timeout=60)
        (output / f"compile-{capacity}.log").write_text(compiled.stdout + compiled.stderr, encoding="utf-8")
        if compiled.returncode:
            print(compiled.stdout + compiled.stderr, end="")
            metadata["exitCode"] = compiled.returncode
            break
        metadata["actualResolverSourceCompiled"] = True
        result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=15)
        log = result.stdout + result.stderr
        (output / f"result-{capacity}.log").write_text(log, encoding="utf-8")
        if "8 resolver dispatch cases failures=" in log:
            completed += 1
        metadata["runs"][str(capacity)] = {"exitCode": result.returncode, "executableSha256": digest(executable.read_bytes())}
        if result.returncode:
            metadata["exitCode"] = result.returncode
        print(log, end="", flush=True)
    metadata["addressCopyCanariesAndSlotReuseChecked"] = completed == 2
    (output / "source.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    raise SystemExit(metadata["exitCode"])

if __name__ == "__main__":
    main()
