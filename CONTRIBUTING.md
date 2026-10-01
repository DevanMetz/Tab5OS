# Contributing

Contributions are welcome, especially reproducible hardware results, safety fixes, focused protocol tools, documentation, and tests.

Use the [bug or feature templates](https://github.com/DevanMetz/Tab5OS/issues/new/choose) for a proposed change or a reproducible problem. For a substantial new tool, discuss its scope before a pull request. Participation follows the [Code of conduct](CODE_OF_CONDUCT.md); report vulnerabilities privately through [SECURITY](SECURITY.md).

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
clang -std=c11 -Wall -Wextra -Werror -pedantic -I main main\capture_data.c tests\capture_data_test.c -o capture_data_test.exe
.\capture_data_test.exe
clang -std=c11 -Wall -Wextra -Werror -pedantic -I main main\serial_log_data.c tests\serial_log_data_test.c -o serial_log_data_test.exe
.\serial_log_data_test.exe
clang -std=c11 -Wall -Wextra -Werror -pedantic -I main main\electronics_math.c tests\electronics_math_test.c -o electronics_math_test.exe
.\electronics_math_test.exe
clang -std=c11 -Wall -Wextra -Werror -pedantic -I main main\byte_data.c tests\byte_data_test.c -o byte_data_test.exe
.\byte_data_test.exe
python tests\byte_reference_test.py .\byte_data_test.exe
python tests\byte_encode_reference_test.py .\byte_data_test.exe
clang -std=c11 -Wall -Wextra -Werror -pedantic -D_CRT_SECURE_NO_WARNINGS -I main main\payload_clipboard.c tests\payload_clipboard_test.c -o payload_clipboard_test.exe
.\payload_clipboard_test.exe
clang -std=c11 -Wall -Wextra -Werror -pedantic -I main main\uart_data.c tests\uart_data_test.c -o uart_data_test.exe
.\uart_data_test.exe
clang -std=c11 -Wall -Wextra -Werror -pedantic -I main main\modbus_data.c main\byte_data.c tests\modbus_data_test.c -o modbus_data_test.exe
.\modbus_data_test.exe
python tests\modbus_reference_test.py .\modbus_data_test.exe
clang -std=c11 -Wall -Wextra -Werror -pedantic -I main main\modbus_data.c main\byte_data.c tests\modbus_rtu_test.c -o modbus_rtu_test.exe
.\modbus_rtu_test.exe
clang -std=c11 -Wall -Wextra -Werror -pedantic -I main main\ntp_data.c tests\ntp_data_test.c -o ntp_data_test.exe
.\ntp_data_test.exe
clang -std=c11 -Wall -Wextra -Werror -pedantic -I main main\ipv4_data.c tests\ipv4_data_test.c -o ipv4_data_test.exe
.\ipv4_data_test.exe
clang -std=c11 -Wall -Wextra -Werror -pedantic -I main main\ipv4_data.c main\wol_data.c tests\wol_data_test.c -o wol_data_test.exe
.\wol_data_test.exe
clang -std=c11 -Wall -Wextra -Werror -pedantic -I main main\ipv4_data.c main\subnet_data.c tests\subnet_data_test.c -o subnet_data_test.exe
.\subnet_data_test.exe
python tests\subnet_reference_test.py .\subnet_data_test.exe
clang -std=c11 -Wall -Wextra -Werror -pedantic -I main main\resistor_data.c tests\resistor_data_test.c -o resistor_data_test.exe
.\resistor_data_test.exe
python -m compileall -q tools
python tools\remote_desktop.py --self-test
python tools\modbus_test_server.py --self-test
python tools\udp_device_fixture.py --self-test
python tests\http_fixture_test.py
git diff --check
.\tools\build_idf.ps1
```

Hardware changes also need the exact panel, firmware commit, wiring, power source, initial/final System diagnostics, logs, measured result, and corresponding [hardware smoke checklist](docs/hardware-smoke-checklist.md) update. A USB/UI observation is not proof of voltage, timing, isolation, or high-impedance release; use the correct instrument.

For Electronics, Byte Lab, Subnet Lab, and Resistor Lab, `./tools/test_offline_ui.ps1 -Snapshots` optionally runs the real LVGL widgets on the host, checks keyboard/input behavior and 100 open/close cycles per app, and writes PPM previews under `build/offline-ui/`. It also checks the shared byte clipboard and runs SPI Console with a driver adapter, verifying draft-only paste, malformed/overlong input rejection, exact transmitted bytes, cleanup, and another 100 lifecycle cycles. No physical pins are used. It requires the downloaded managed LVGL sources, a native C compiler, CMake, and Ninja. This does not replace physical panel/touch or electrical checks. The Subnet Lab reference test uses Python's standard-library `ipaddress` module to independently check 2,112 cases across all IPv4 prefix lengths.

Use `./tools/test_offline_ui.ps1 -SpiOnly -Snapshots` for the focused SPI editor/receive-copy checks and eight reopen cycles. This mode requires the cached LVGL library and inspects the build plan to refuse rebuilding it. Builds run with one job. The checks include actual hex keys/cursor/backspace, no-send Done/Cancel, independent draft/result clearing, invalid/Unicode/excess-length retention through immediate Home, one- and 32-byte bounds, RX differing from TX, original setting retention, stopped/Home copy without bus activity, driver failure after writing RX, and a real Byte Lab Float32 handoff. This focused run, a host release-mode syntax check and the 2026-09-29 single-job firmware build passed. Initial ST7121 editor/transfer/copy checks are recorded in the hardware checklist; electrical and broader panel validation remain open.

Use `./tools/test_offline_ui.ps1 -I2cOnly -Snapshots` for the I2C saved-read panel and actual Byte Lab handoff. It follows the same cached-library, single-job limits and checks all lengths 1-32, raw byte extremes, buffer ownership, invalid lengths, source identity, failed-read/clear invalidation, pre-action callback invocation, disabled-button revalidation, signed/Float32 decoding and eight reopen cycles. This mode exercises the panel without the legacy I2C screen. Compile the independent transaction adapter with `cc -std=c11 -Wall -Wextra -Werror -pedantic -I tests/i2c_host -I main main/i2c_register.c tests/i2c_register_test.c -o i2c_register_test` and run it; CI also runs its 192 bounded-read combinations and transaction/cleanup faults. Native checks and the 2026-09-29 single-job firmware build pass. ST7121 covers initial empty-bus scan/read, length/Watch controls and Home teardown. Known-device reads, confirmation/CSV faults, shared-pin electrical transitions and the second panel remain pending.

The Byte Lab reference test compares 4,096 signed/Float32 payloads in both byte orders with Python's standard-library decoders. Its encoder reference checks 7,272 integer/Float32/order cases using integer serialization and exact rational rounding. Modbus independently checks the rendered unsigned/signed/Float32 values for 4,096 register/order cases. Resistor Lab checks all 1,056 supported EIA-96 index/multiplier pairs against independently generated preferred values. On platforms with a separate math library, link Electronics, Byte Lab, Modbus, NTP, and Resistor Lab calculation tests with `-lm`, as CI does.

The shared IPv4 parser checks 1,024 octet-position cases plus malformed addresses and output clearing. It enforces the same full decimal syntax on the host and tablet, independently of the socket library's address parser.

`./tools/test_modbus_rtu_ui.ps1 -Snapshots` checks the offline RTU Frames app using the cached Debug LVGL host library. This small Windows build compiles sequentially, starts no workers or hardware, and refuses to rebuild a missing library. It covers clipboard bytes, reply decoding, invalid input, keyboard behavior, retained Unicode/excess-length drafts, popup cleanup, and eight lifecycle cycles. Its pure companion checks CRCs against an independent polynomial calculation and compares TCP/RTU PDU decoding. See [RTU Frames](docs/modbus-rtu.md).

`./tools/test_uart_ui.ps1 -Snapshots` runs the actual Serial terminal with real LVGL and focused Windows UART/GPIO/allocation adapters. It verifies exact transmitted bytes and suffixes, 256-byte limits, invalid/Unicode draft retention, history wrap/replay, clipboard paste without traffic, partial/failing writes, RX display, keyboard behavior, allocation failure, TX drain bounds, and 100 UART/RS-485 lifecycle cycles. PPMs go under `build/uart-ui/`; no physical pins or SD writes are used. The pure `uart_data` tests cover every message length and suffix choice plus bounded history formatting. See [Serial terminal](docs/serial-terminal.md).

The Serial runner also checks bounded RX capture across reads, freeze/Stop/Home tails, 128-byte limits, read/drain failures, source identity, and a real-widget RTU request/Serial reply/Byte Lab clipboard round trip. Add `-Quick` for all functional checks and eight lifecycle cycles. It compiles with one job, reuses the cached Debug LVGL library, and refuses to rebuild a missing library automatically. This quick mode and the 2026-09-29 firmware build passed; ST7121 passed an empty capture and original-link retention, while wired traffic and broader hardware checks remain pending.

`./tools/test_serial_log_ui.ps1 -Snapshots` exercises the saved serial log viewer and parser against actual generated files under `build/serial-log-ui/`. It checks row identity through filters/pages, exact Hex/ASCII copy, oversized rejection, immutable files, empty/truncated logs, both allocation failures, inspector handoff and eight lifecycle cycles. The only service adapters are heap allocation and Windows CRT spellings. This serial build requires the cached Debug LVGL library and never rebuilds it or starts workers; firmware/physical validation remains separate.

On Windows, `./tools/test_modbus_network.ps1` tests the actual Modbus app against a loopback-only Python fixture using real LVGL, sockets, and threads. Its 17 scenarios cover input preservation/rejection, read functions, 32-bit views, fragmented and invalid responses, confirmation gates, timeout/cancellation, and repeated resource cleanup. Use `-Modes ui,views` to check layouts and saved-reply interpretations; rendered PPMs go under `build/modbus-network/`. It uses the same native build tools plus Python and reuses the offline LVGL build, creating it if missing. The native adapters do not emulate ESP-IDF/lwIP or Wi-Fi hardware.

`./tools/test_device_network.ps1` uses the same Windows host setup for NTP Lab, Wake-on-LAN, and UDP Console. It covers 39 scenarios including invalid input without network activity, exact packet contents, Byte Lab/UDP clipboard round trips, oversized/wrong-peer replies, source-port conflicts, confirmation/cancellation, allocation failure, and repeated resource cleanup. Use `-Modes udp-clipboard,udp-echo,udp-home` to select cases. Logs and rendered PNGs go under `build/device-network/`. See [Device and network tools](docs/device-network-tools.md) and [UDP Console](docs/udp-console.md) for manual PC fixtures and remaining hardware validation.

For HTTP Console hardware checks, run `python tools/http_test_server.py --bind <PC-LAN-IPv4> --port 18080 --mode echo` and enter `http://<PC-LAN-IPv4>:18080/` on the tablet. All methods operate on synthetic data; the fixture does not read or change files or forward requests. Its console logs omit headers, bodies and URL queries. Modes `large`, `redirect`, `short`, `silent`, `slow-headers` and `stream` exercise the preview cap, disabled redirects, incomplete bodies, and waiting/streaming peers. `--seconds` controls the last three modes (0-60); `--interval` controls trickled headers/chunks. Stop each fixture with Ctrl+C before switching modes. It binds loopback unless explicitly given a LAN address. `python tests/http_fixture_test.py` independently checks all seven modes using Python's standard-library HTTP client and real loopback sockets; it validates the fixture, not the tablet implementation.

## Pull requests

Keep the change focused and explain the user outcome, safety/privacy impact, tests run, hardware evidence, remaining gates, and release/data-format compatibility. Do not mix reverse-engineering artifacts, generated build output, formatting churn, or unrelated refactors. Before a stable tag, pass CI, run the applicable checks on both panel families, and state any remaining gates in the release notes and hardware checklist. The v0.6.0 field beta has open gates documented there.

For vulnerabilities or exposed secrets, follow [SECURITY](SECURITY.md) instead of opening a public issue.
