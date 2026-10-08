"""Compile actual OTA result, health and worker callbacks with controlled APIs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default=os.environ.get("CC", "cc"))
    parser.add_argument("--main-source", type=Path, default=ROOT / "main/main.c")
    parser.add_argument("--output", type=Path, default=ROOT / "build/ota-result")
    args = parser.parse_args()
    source = args.main_source.read_text(encoding="utf-8")
    start = source.index("static void ota_record_pending(const char *version)")
    end = source.index("static const char *restart_blocker(void)", start)
    code = source[start:end]
    assert code.count("static void ota_") == 3
    assert "static void ota_record_result(const char *result)" in code
    assert "static void ota_load_result(void)" in code
    health_start = source.index("static void validate_running_ota(void)\n{")
    health = source[health_start:source.index("void app_main(void)", health_start)]
    assert health.count("static void ") == 2
    assert "static void confirm_running_ota(lv_timer_t *timer)" in health
    code += health
    worker_start = source.index("static void ota_update_task(void *argument)\n{")
    worker = source[worker_start:source.index("static const char *reset_reason_name(", worker_start)]
    assert worker.count("static void ") == 3
    assert "static void ota_clicked(lv_event_t *event)" in worker
    assert "static void ota_tick(lv_timer_t *timer)" in worker
    code += worker
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "ota_result.inc").write_text(code, encoding="utf-8")
    executable = args.output.resolve() / ("ota_result_test.exe" if os.name == "nt" else "ota_result_test")
    temporary = args.output.resolve() / "temp"
    temporary.mkdir(exist_ok=True)
    env = os.environ.copy()
    env.update(TEMP=str(temporary), TMP=str(temporary), TMPDIR=str(temporary))
    command = [args.compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
               "-D_CRT_SECURE_NO_WARNINGS", "-I", str(args.output.resolve()),
               str(ROOT / "tests/ota_result_test.c"), "-o", str(executable)]
    compile_result = subprocess.run(command, capture_output=True, text=True, env=env, timeout=60)
    (args.output / "compile.log").write_text(compile_result.stdout + compile_result.stderr)
    if compile_result.returncode:
        print(compile_result.stdout + compile_result.stderr, end="")
        raise SystemExit(compile_result.returncode)
    result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=15)
    log = result.stdout + result.stderr
    (args.output / "result.log").write_text(log)
    (args.output / "source.json").write_text(json.dumps({
        "mainSource": str(args.main_source.resolve()), "compiler": args.compiler,
        "mainSourceSha256": hashlib.sha256(source.encode()).hexdigest(),
        "testSourceSha256": hashlib.sha256((ROOT / "tests/ota_result_test.c").read_text(encoding="utf-8").encode()).hexdigest(),
        "functionSourceSha256": hashlib.sha256(code.encode()).hexdigest(),
        "executableSha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
        "exitCode": result.returncode, "nvsAndOtaStateApisControlled": True,
        "validationAndTimerApisControlled": True, "workerApisAndSignalAccessControlled": True,
        "actualFunctionsCompiled": 8,
        "physicalFlashVerified": False,
    }, indent=2) + "\n")
    print(log, end="", flush=True)
    raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
