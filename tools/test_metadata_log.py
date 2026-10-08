"""Compile actual HTTP/MQTT metadata appenders with native files and I/O faults."""
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default=os.environ.get("CC", "cc"))
    parser.add_argument("--http-source", type=Path, default=ROOT / "main/http_tool.c")
    parser.add_argument("--mqtt-source", type=Path, default=ROOT / "main/mqtt_tool.c")
    parser.add_argument("--output", type=Path, default=ROOT / "build/metadata-log")
    args = parser.parse_args()
    sources = {"http": args.http_source.read_text(encoding="utf-8"),
               "mqtt": args.mqtt_source.read_text(encoding="utf-8")}
    types = {}
    for name, type_name in (("http", "http_job_t"), ("mqtt", "mqtt_log_entry_t")):
        source = sources[name]
        end = source.index(f"}} {type_name};") + len(f"}} {type_name};")
        types[name] = source[source.rindex("typedef struct {", 0, end):end] + "\n"
    code = {"http": section(sources["http"], "static void safe_log_url(", "static bool header_name_character(") +
                    section(sources["http"], "static int csv_field(", "static bool request_cancelled("),
            "mqtt": section(sources["mqtt"], "static int csv_field(", "static void append_history(")}
    config = {}
    for name, constants in (("http", ("HTTP_URL_MAX", "HTTP_HEADERS_MAX", "HTTP_BODY_MAX", "HTTP_PREVIEW_MAX", "HTTP_LOG_PATH")),
                            ("mqtt", ("MQTT_TOPIC_MAX", "MQTT_LOG_PATH"))):
        config[name] = "".join(next(line + "\n" for line in sources[name].splitlines()
                                    if line.startswith(f"#define {constant} ")) for constant in constants)
    names = next(line for line in sources["http"].splitlines() if line.startswith("static const char *const method_names[]"))
    config["http"] += names + "\n"
    storage = (ROOT / "main/storage_io.c").read_text(encoding="utf-8")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    for name in sources:
        for suffix, content in (("source.c", sources[name]), ("log.inc", code[name]),
                                ("log_types.inc", types[name]), ("log_config.inc", config[name])):
            (output / f"{name}_{suffix}").write_text(content, encoding="utf-8")
    (output / "storage_source.inc").write_text(storage, encoding="utf-8")
    temporary = output / "temp"
    temporary.mkdir(exist_ok=True)
    env = os.environ.copy()
    env.update(TEMP=str(temporary), TMP=str(temporary), TMPDIR=str(temporary))
    runs = {}
    for name in sources:
        executable = output / (f"{name}_metadata_log_test.exe" if os.name == "nt" else f"{name}_metadata_log_test")
        compiled = subprocess.run([args.compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                                   f"-DMETA_HTTP={int(name == 'http')}", "-I", str(output), "-I", str(ROOT / "main"),
                                   str(ROOT / "tests/metadata_log_test.c"), "-o", str(executable)],
                                  capture_output=True, text=True, env=env, timeout=60)
        (output / f"{name}-compile.log").write_text(compiled.stdout + compiled.stderr, encoding="utf-8")
        if compiled.returncode:
            print(compiled.stdout + compiled.stderr, end="")
            raise SystemExit(compiled.returncode)
        result = subprocess.run([str(executable)], cwd=output, capture_output=True, text=True, timeout=90)
        log = result.stdout + result.stderr
        (output / f"{name}-result.log").write_text(log, encoding="utf-8")
        runs[name] = {"exitCode": result.returncode, "executableSha256": digest(executable.read_bytes())}
        print(log, end="", flush=True)
    (output / "source.json").write_text(json.dumps({
        "compiler": args.compiler,
        "sourceFileSha256": {"main/http_tool.c": digest(sources["http"].encode()),
                             "main/mqtt_tool.c": digest(sources["mqtt"].encode()),
                             "main/storage_io.c": digest(storage.encode()),
                             "main/storage_io.h": digest((ROOT / "main/storage_io.h").read_text(encoding="utf-8").encode()),
                             "tests/metadata_log_test.c": digest((ROOT / "tests/metadata_log_test.c").read_text(encoding="utf-8").encode())},
        "functionSourceSha256": {name: digest(value.encode()) for name, value in code.items()},
        "metadataTypesSourceSha256": {name: digest(value.encode()) for name, value in types.items()},
        "configSourceSha256": {name: digest(value.encode()) for name, value in config.items()},
        "actualAppendAndFormattingFunctionsCompiled": 6, "actualMetadataTypesCompiled": 2,
        "actualStorageSourceCompiled": True, "nativeFilesAndDirectoriesUsed": True,
        "nativeFileBytesChecked": True, "ioFaultsControlled": True, "timeAndSdkTypesControlled": True,
        "hostStreamsUseBinaryMode": True,
        "uiOrNetworkWorkerExecuted": False, "physicalSdOrTlsVerified": False, "runs": runs,
    }, indent=2) + "\n", encoding="utf-8")
    raise SystemExit(any(run["exitCode"] for run in runs.values()))


if __name__ == "__main__":
    main()
