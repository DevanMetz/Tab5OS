"""Run the real root CMake checks against tiny SDK configure fixtures.

The fixture's project macro records when SDK configuration would be regenerated.
No real SDK, compiler, dependency download, device or existing sdkconfig is used.
"""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def check(cmake, work, *, major=5, minor=4, version="5.4", target="esp32p4",
          legacy="y", copro=None, sdio="y", failure=None, before_project=False):
    sdk = work / "SDK with spaces"
    scripts = sdk / "tools/cmake"
    scripts.mkdir(parents=True, exist_ok=True)
    (scripts / "version.cmake").write_text(
        f"set(IDF_VERSION_MAJOR {major})\nset(IDF_VERSION_MINOR {minor})\n", encoding="utf-8")
    marker = work / "project-called"
    config = work / "fixture-sdkconfig"
    marker.unlink(missing_ok=True)
    config.write_text("original configuration\n", encoding="utf-8")
    (scripts / "project.cmake").write_text(
        "macro(project)\n"
        f'  file(WRITE "{marker.as_posix()}" "configured")\n'
        f'  file(WRITE "{config.as_posix()}" "regenerated configuration\\n")\n'
        "endmacro()\n", encoding="utf-8")
    environment = dict(os.environ, IDF_PATH=str(sdk))
    environment.pop("ESP_IDF_VERSION", None)
    if version is not None:
        environment["ESP_IDF_VERSION"] = version
    command = [cmake]
    for name, value in (("IDF_TARGET", target), ("CONFIG_SLAVE_IDF_TARGET_ESP32C6", legacy),
                        ("CONFIG_ESP_HOSTED_CP_TARGET_ESP32C6", copro),
                        ("CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE", sdio)):
        if value is not None:
            command.append(f"-D{name}={value}")
    command += ["-P", str(ROOT / "CMakeLists.txt")]
    result = subprocess.run(command, env=environment, cwd=work, capture_output=True, text=True, timeout=10)
    diagnostic = result.stdout + result.stderr
    if failure is None:
        assert result.returncode == 0, diagnostic
        assert marker.read_text() == "configured"
    else:
        assert result.returncode != 0, "Invalid configuration accepted"
        assert failure in diagnostic, diagnostic
        if before_project:
            assert not marker.exists(), "SDK project ran before export validation"
            assert config.read_text() == "original configuration\n", "SDK configuration mutated on invalid export"
        else:
            assert marker.read_text() == "configured", "Board settings checked before SDK supplied them"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmake", default=shutil.which("cmake"))
    args = parser.parse_args()
    if not args.cmake:
        parser.error("CMake is required; add it to PATH or pass --cmake")
    cases = [
        {},
        {"version": None, "failure": "SDK environment mismatch", "before_project": True},
        {"version": "", "failure": "SDK environment mismatch", "before_project": True},
        {"version": "5.3", "failure": "SDK environment mismatch", "before_project": True},
        {"version": "5.4.2", "failure": "SDK environment mismatch", "before_project": True},
        {"version": "5.4 ", "failure": "SDK environment mismatch", "before_project": True},
        {"target": "esp32c6", "failure": "IDF_TARGET=esp32p4"},
        {"target": None, "failure": "IDF_TARGET=esp32p4"},
        {"legacy": None, "failure": "ESP32-C6 hosted Wi-Fi target"},
        {"legacy": "n", "failure": "ESP32-C6 hosted Wi-Fi target"},
        {"legacy": None, "copro": "y", "failure": "ESP32-C6 hosted Wi-Fi target"},
        {"sdio": None, "failure": "ESP-Hosted SDIO transport"},
        {"sdio": "n", "failure": "ESP-Hosted SDIO transport"},
        {"major": 5, "minor": 5, "version": "5.5"},
        {"major": 6, "minor": 0, "version": "6.0", "legacy": None, "copro": "y"},
        {"major": 6, "minor": 0, "version": "5.4", "copro": "y",
         "failure": "SDK environment mismatch", "before_project": True},
        {"major": 6, "minor": 0, "version": None, "copro": "y",
         "failure": "SDK environment mismatch", "before_project": True},
        {"major": 6, "minor": 0, "version": "6.0", "legacy": "y", "copro": None,
         "failure": "ESP32-C6 hosted Wi-Fi target"},
        {"major": 6, "minor": 0, "version": "6.0", "copro": "n",
         "failure": "ESP32-C6 hosted Wi-Fi target"},
        {"major": 6, "minor": 0, "version": "6.0", "copro": "y", "sdio": "n",
         "failure": "ESP-Hosted SDIO transport"},
    ]
    base = (ROOT / "build/board-config").resolve()
    assert base.is_relative_to(ROOT), "Fixture directory must stay within the workspace"
    base.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="fixture-", dir=base) as directory:
        work = Path(directory)
        assert work.resolve().parent == base
        for index, case in enumerate(cases, 1):
            try:
                check(args.cmake, work, **case)
            except (AssertionError, OSError, subprocess.SubprocessError) as error:
                raise AssertionError(f"Board configure case {index}: {error}") from error
    print(f"Board configuration: {len(cases)} root-CMake guard checks passed")


if __name__ == "__main__":
    main()
