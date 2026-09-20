# Contributing

Contributions are welcome, especially reproducible hardware results, safety fixes, focused protocol tools, documentation, and tests.

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

Run the checks relevant to the change:

```powershell
node --test relay\worker.test.mjs
clang -std=c11 -Wall -Wextra -Werror -pedantic -I main main\storage_io.c tests\storage_io_test.c -o storage_io_test.exe
.\storage_io_test.exe
python -m compileall -q tools
python tools\remote_desktop.py --self-test
git diff --check
.\tools\build_idf.ps1
```

Hardware changes also need the exact panel, firmware commit, wiring, power source, initial/final System diagnostics, logs, measured result, and corresponding [hardware smoke checklist](docs/hardware-smoke-checklist.md) update. A USB/UI observation is not proof of voltage, timing, isolation, or high-impedance release; use the correct instrument.

## Pull requests

Keep the change focused and explain the user outcome, safety/privacy impact, tests run, hardware evidence, remaining gates, and release/data-format compatibility. Do not mix reverse-engineering artifacts, generated build output, formatting churn, or unrelated refactors. Stable tags are created only after CI and the applicable two-panel hardware gates pass.

For vulnerabilities or exposed secrets, follow [SECURITY](SECURITY.md) instead of opening a public issue.
