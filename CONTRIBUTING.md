# Contributing

Contributions are welcome, especially reproducible hardware results, safety fixes, focused protocol tools, documentation, and tests.

Use the [bug or feature templates](https://github.com/DevanMetz/Tab5OS/issues/new/choose) for a proposed change or a reproducible problem. For a substantial new tool, discuss its scope before a pull request. Participation follows the [Code of conduct](CODE_OF_CONDUCT.md); report vulnerabilities privately through [SECURITY](SECURITY.md).

## Quickstart (Windows PowerShell)

Use an ESP-IDF 5.4.2 PowerShell environment for firmware. Install Python 3 and Node.js on PATH for the quick host checks below. A tablet is not needed for these steps.

In a regular PowerShell session, activate the SDK with its `export.ps1` first. CMake checks its exported `ESP_IDF_VERSION=5.4` before regenerating configuration, then requires the Tab5 P4/C6 SDIO profile. [Testing](docs/testing.md#firmware-configuration) explains the checks and their host runner.

1. Clone the repository:

   ```powershell
   git clone https://github.com/DevanMetz/Tab5OS.git
   cd Tab5OS
   ```

2. Build the pinned firmware and fetch its managed dependencies:

   ```powershell
   idf.py set-target esp32p4
   idf.py build
   ```

3. Run the quick host smoke checks:

   ```powershell
   python tools/remote_desktop.py --self-test
   node --test relay/worker.test.mjs
   git diff --check
   ```

These smoke checks are a starting point. Before a PR, run the parser, UI or protocol checks relevant to your change in [Testing](docs/testing.md).

For optional real LVGL UI checks, install native Clang, CMake and Ninja on PATH, then run `./tools/test_offline_ui.ps1 -Snapshots`. This builds the Debug host library used by the focused UI runners. [Testing](docs/testing.md#offline-and-peripheral-ui) lists their prerequisites and coverage; several adapters require Windows.

Flashing is a separate step. Follow [Install and recovery](docs/install-recovery.md), including the installed partition table and active-slot check. The app-only wrapper always writes `0x20000` and does not detect a legacy layout.

## Before changing code

1. Read [ROADMAP](ROADMAP.md), [Architecture](docs/architecture.md), [Pin safety](docs/pin-safety.md), and the applicable hardware checklist section.
2. Use ESP-IDF 5.4.2 and the pinned dependencies. Do not regenerate dependency versions incidentally.
3. Keep external hardware disconnected until the firmware builds and the intended pin ownership/release path is understood.
4. Never add wearable stock/patched firmware, decompiled proprietary material, credentials, raw NVS, or private captures to a contribution.

## Design rules

- Reuse ESP-IDF/FreeRTOS/LVGL/NVS/files before adding a dependency or framework.
- Put a substantial new tool in its own source file. Move legacy code only while materially changing it.
- Give every pin, bus, LEDC channel/timer, BLE connection, and storage writer one owner and a deterministic stop path.
- Default outputs, external power, scans, connections, logging, cleartext transport, and destructive actions to safe/off or explicit confirmation.
- Bound network input, dynamic memory, result counts, transfer sizes, history, and timeouts. Never retain pointers to deleted LVGL objects.
- Preserve credentials and user data on recoverable faults; use the documented atomic/recovery pattern for persistent writes.
- Add one small runnable check for new parser/calculation/branch logic. Do not mock the whole board.

## Verification

Run the relevant checks in [Testing](docs/testing.md) and a clean ESP-IDF build before a PR. The guide separates pure calculations/parsers, real LVGL UI checks, network fixtures and physical hardware work; host success alone does not establish electrical or panel behavior.

For hardware changes, record the firmware/image identity, panel, wiring, power source, initial/final System diagnostics and measured result in the [hardware smoke checklist](docs/hardware-smoke-checklist.md). Keep credentials and private captures out of reports.

## Pull requests

Keep the change focused and explain the user outcome, safety/privacy impact, tests run, hardware evidence, remaining gates, and release/data-format compatibility. Do not mix reverse-engineering artifacts, generated build output, formatting churn, or unrelated refactors. Before a stable tag, pass CI, run the applicable checks on both panel families, and state any remaining gates in the release notes and hardware checklist. The v0.6.0 field beta has open gates documented there.

For vulnerabilities or exposed secrets, follow [SECURITY](SECURITY.md) instead of opening a public issue.
