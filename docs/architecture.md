# Architecture

Tab5 OS is an ESP-IDF/FreeRTOS firmware with one LVGL display shell. It deliberately uses the platform's native drivers, NVS, SPIFFS, FAT VFS, HTTP/TLS, OTA, NimBLE, and ordinary files instead of introducing an application framework or database.

## Boot and runtime

`app_main` runs assertion-based parser/math and component checks, initializes the shared internal I2C devices, display/touch, ESP-Hosted Wi-Fi transport, NVS-backed settings, internal SPIFFS, and microSD, then creates the fixed header/content shell. With assertions enabled, reaching the `Starting Tab5 OS` boot log means those checks passed. A 30-second LVGL timer validates an OTA candidate only after NVS and internal storage are usable and the event loop remains alive.

On the pinned ESP-IDF 5.4.2, ESP32-P4 with PSRAM XIP needs `psram_exec_compat.c`: the SDK's executable-pointer check omits the linker-reserved PSRAM instruction range, causing a valid pthread TLS deletion callback to abort after a network worker exits ([upstream issue 15997](https://github.com/espressif/esp-idf/issues/15997)). The IRAM linker wrapper accepts only that instruction range and delegates every other address to the original SDK check; the FreeRTOS corruption guard remains enabled. CMake limits the wrapper to that SDK version, chip and configuration. Assertion-enabled boot checks validate the range boundaries and actual task/TLS cleanup on each CPU core. Reassess the upstream check and repeat these hardware tests when upgrading the SDK before removing the workaround.

LVGL objects and callbacks run on the display task under the BSP display lock. Network, audio, ADC, BLE, download, and cleanup work runs in bounded FreeRTOS tasks or timers. Workers never update deleted LVGL objects directly: they store bounded state/results, and the visible app's LVGL timer renders them.

LVGL's fixed 96 KiB object pool is allocated once at display initialization in byte-addressable PSRAM through its supported `LV_MEM_POOL_ALLOC` hook. The size and allocator stay unchanged; moving the pool out of static internal RAM leaves space for the ESP-Hosted SDIO driver's DMA receive buffers. `main/CMakeLists.txt` applies this only to the firmware's LVGL component, without editing managed sources or changing the native test build. Boot logs confirm the allocation as `ui-memory: LVGL pool: 98304 bytes in PSRAM`.

System identifies the running image with the SDK's semantic version and ELF SHA-256 prefix, plus the active partition label, address and capacity. The prefix matches boot/crash logs and distinguishes incremental builds whose version text is unchanged; cached app-description timestamps are not used as build identity. Partition details describe the installed layout, which may differ from the repository's current partition table.

The speaker amplifier's SPK_EN output (PI4IOE1 P1) is preloaded low before configuring the expander outputs. Alarm playback configures and mutes the codec, writes silence, then unmutes and enables the amplifier only if setup succeeds. It mutes and disables the amplifier before closing the codec. Failed setup leaves a visual alarm and logs the error. This prevents deliberately enabling an uninitialized audio path; its quiet-startup and alarm behavior still need hardware verification after the reported USB brownouts. Brownout protection and charging settings remain unchanged.

## Launcher and lifecycle

The static launcher table in `main/main.c` owns each tile's enter callback and optional leave hook. `clear_content()` invokes the active leave hook, stops shared tools/timers, releases pins and outputs, nulls screen pointers, deletes content children, and resets scroll/layout state. New substantial tools live in their own `main/*_tool.c` file; legacy code moves only when materially changed.

Electronics, Byte Lab, Subnet Lab, and Resistor Lab are offline LVGL apps with pure calculation/parser modules tested on the host. Their leave hooks detach the keyboard and clear screen pointers before content deletion. Inputs are retained only in RAM; they own no pins, worker tasks, network sessions, or storage writers. Byte Lab coalesces bulk-input rendering into one LVGL async call and cancels it on exit, avoiding per-character button-animation allocation. Subnet Lab uses bounded IPv4 inputs and 64-bit counts, including explicit /31 point-to-point and /32 host-route semantics. Resistor Lab decodes color bands and bounded SMD markings, preserving separate drafts and clearing invalid results. See [Offline bench helpers](offline-tools.md) and [Resistor Lab](resistor-lab.md).

Byte Lab also encodes bounded decimal integers and Float32 values into a selected byte order, reusing its existing checksum/interpretation calculation. Syntax and length checks precede standard-library float conversion; range checks reject finite overflow and nonzero underflow to zero. The encoder requires a binary32 C float, covered by compile-time guards and known-byte startup/host checks. An explicit UI action copies a valid result to the Hex draft.

`payload_clipboard.c` is a bounded 128-byte RAM clipboard shared by the bench apps and saved serial log viewer. Only LVGL callbacks read or change it; workers continue using their immutable request snapshots, so the clipboard needs no lock. Failed copies preserve the previous value; clearing or shorter copies zero unused bytes. Hex formatting either succeeds completely or returns an empty error output. Each console validates its own size limit before replacing a draft. Paste does not start a bus/worker or send traffic, and UDP paste disarms confirmation. Only complete UDP replies of at most 128 bytes can be copied, after socket cleanup.

The legacy I2C inspector uses `i2c_register.c` for bounded synchronous transactions: one register-pointer byte followed by 1-32 read bytes, or one confirmed register/value write. It accepts only ordinary 7-bit addresses and 100/400 kHz, rejects invalid parameters before bus activity, and zeroes receive output on transaction or cleanup failure. Device and bus handles survive failed removals for retry; another transaction cannot begin until cleanup succeeds. Home retries cleanup, restart/OTA and GPIO/Scope/Servo entry respect retained bus ownership, and unconditional Servo cleanup skips shared G54 while I2C still owns it.

`i2c_result.c` is the inspector's UI-only saved-read panel. It owns up to 32 bytes and the original address/register pointer/speed. Failed or invalid reads clear it; confirmed writes invalidate it before bus I/O. Copy/clear callbacks first cancel pending write confirmation. Home drops panel pointers and stops the existing Watch timer while retaining bytes in RAM. SCAN and READ ONCE are explicit. One-shot reads stop Watch and finish CSV capture; Watch/CSV remain one byte per sample. The panel adds no driver, timer, file or worker. Native transaction-fault/panel/Byte Lab checks and the 2026-09-29 firmware build pass. ST7121 passed empty-bus failures, length selection/retention, Watch-to-read transition and popup teardown; known-device reads, CSV faults and electrical validation remain open.

SPI stores one completed transfer in a static snapshot: up to 32 TX bytes, 32 RX bytes, and the original mode/clock. COPY RX validates that snapshot and copies raw receive bytes without bus activity. Draft or setting edits leave it intact; an actual driver transfer failure zeroes it, clears the display and disables copying. CLEAR RESULT zeroes the snapshot independently of the clipboard. A separate RAM draft retains the first excess character and up to four UTF-8 bytes per LVGL character; restore happens before attaching input callbacks to avoid modifying the source during per-character events. CLEAR TX affects only that draft. The hex keyboard attaches on focus/tap and detaches on Done/Cancel or Home. Stop/Home releases the bus and UI pointers while retaining the draft and snapshot in RAM. No worker, timer or storage path is added. Native UI checks and the 2026-09-29 firmware build pass. ST7121 passed keyboard entry/dismissal, one disconnected transfer, original-setting retention and COPY RX -> Byte Lab. Electrical checks, a real peripheral and the second panel family remain open.

The Serial terminal uses `uart_data.c` for bounded ASCII/Hex parsing, explicit ASCII suffixes, and exact history reconstruction. Its eight RAM history entries retain binary bytes and format/suffix instead of truncating text. The existing receive timer coalesces draft validation after a bulk paste; SEND always revalidates synchronously. Home invokes its explicit launcher leave hook, cancels that timer, detaches the keyboard, drains pending TX using the selected framing/baud, and releases its output buffer and interface. See [Serial terminal](serial-terminal.md).

Serial's optional RX window uses a static 128-byte buffer in that same LVGL task. Capture start first drains queued input into the transcript/log; receive ticks append across driver reads. Freeze/Stop/Home drain a bounded tail and freeze the window, retaining its original link settings. Overflow or a read/drain failure clears the captured bytes and prevents copy. Explicit COPY RX alone changes the shared clipboard; no capture worker, timer or protocol framing layer is added. The window observes driver-delivered bytes and cannot prove physical frame completeness or absence of FIFO loss.

[RTU Frames](modbus-rtu.md) is a separate offline app that builds eight-byte read requests into the shared clipboard and decodes complete pasted replies. It shares the pure read PDU decoder/value formatter with Modbus TCP and the CRC helper with Byte Lab. Input edits invalidate results once per edit sequence, avoiding per-character rendering allocations. Its leave hook detaches the keyboard and clears UI pointers; it owns no UART, timer, worker, network session or storage writer. A pasted frame cannot establish serial timing or its originating request address. Native checks and the 2026-09-29 firmware build pass. ST7121 passed the example decode and request handoff into stopped Serial; broader panel and wired-fixture checks remain open.

Modbus TCP uses a single bounded network worker for a connection test or one read request. The worker owns and closes its nonblocking socket; Stop/Home signals cancellation without waiting in LVGL. OTA checks its busy state through cleanup. It publishes raw register/bit values with the request identity; the UI formats that snapshot into 16-bit or selectable 32-bit views without new traffic. Float32 interpretation shares Byte Lab's pure binary32 decoder. Its framing/decoding module is host-testable and supports only read functions 01-04. See [Modbus TCP inspector](modbus-tcp.md).

NTP Lab and Wake-on-LAN each own one bounded UDP worker and publish immutable request/result snapshots for the LVGL timer. Home requests cancellation and detaches screen objects; the worker remains busy through socket cleanup, preventing OTA restart. NTP Lab measures four samples without setting the clock. Wake-on-LAN sends one confirmed magic packet without retrying or claiming that the target woke. Neither app opens a listener, saves settings, or logs traffic. See [Device and network tools](device-network-tools.md).

UDP Console uses the same worker/snapshot lifecycle for one confirmed custom datagram. It reuses Byte Lab's bounded payload parser, transmits at most 128 bytes, and receives a 512-byte preview plus one sentinel byte to distinguish oversized replies. A connected, nonblocking UDP socket filters replies to the selected peer; a three-second deadline and cancellation bound the exchange. The worker alone closes the socket and remains visible to the OTA blocker until cleanup completes. No listener, task, or UI timer remains after an idle exit. See [UDP Console](udp-console.md).

Every resource has one owner at a time:

- G53/G54: GPIO, Scope ADC, or external I2C; Servo Toy also uses G54 for its LED driver.
- G0: GPIO or Servo Toy PWM.
- G6 and LEDC timer 2/channel 3: Signal Generator.
- LEDC timer 1/channel 2: Servo; display backlight remains timer 0/channel 1.
- UART1: G47/G48 terminal or onboard RS-485 through its explicit mode.
- SPI2: bounded M5-Bus transaction console.
- BLE connection/scanner: optional product view or generic GATT Explorer, never both.
- SD writer/capture: the owning app until stop/commit/abort completes.

Leaving an app restores shared pins to disabled/high-impedance or an explicitly safe receive/off state. OTA checks the same ownership state and refuses restart while a writer, output, sampling task, network operation, or BLE session is active.

Ender 3 joins its connection worker before uninstalling CDC or freeing callback semaphores. The BSP USB event task handles pending device-free events and uninstalls the host outside `usb_host_lib_handle_events`; a completion semaphore makes stop idempotent and allows a timed-out caller to retry. Failed cleanup retains ownership and blocks re-entry/restart/OTA, while UI pointers detach immediately. Host install and task-allocation errors return to the UI instead of aborting. ST7121 passed 16 empty-port cycles after this corrected a reproducible second-entry abort; live printer and fault-injection coverage remains open.

BLE receives copy at most 64 bytes out of NimBLE buffer chains into locked state, retaining the original handle, total received length and indication/notification kind. Only the UI copy handler touches the shared clipboard, after taking a snapshot under that lock. Truncated values, failed buffer copies and stopping/disconnected sessions cannot replace the clipboard. Firmware startup tests exercise real buffer traversal and the receive/copy callbacks without radio traffic. The explicitly saved CSV marks unavailable data and distinguishes indications from notifications.

## Storage

The serial log viewer loads at most 512 validated rows and closes the file before presenting its eight-row pages. Its selector maps each visible option to the original loaded-row index, which remains stable across direction filters. COPY ROW revalidates that mapping and copies raw bytes to the UI-owned clipboard only for a whole row of 1-128 bytes; formatted ASCII/Hex text is never the source. Partial allocation failures free both buffers immediately. Home/Back frees all viewer buffers and clears selection/UI pointers. Native fixture/UI checks and the 2026-09-29 firmware build pass; physical SD/panel checks for record copying remain pending.

- NVS stores small validated settings and credentials. Initialization faults preserve data rather than auto-erasing it.
- Internal `storage` is SPIFFS for built-in state. Only provably blank flash auto-initializes.
- FAT microSD stores user files and evidence. Writers use durable sync, unique `.TMP` publication, recoverable `.BAK` replacement, or incomplete-tail repair according to the data type.

See [Storage and compatibility contract](compatibility.md) and [Data formats](data-formats.md).

## Network and radio

`main/ipv4_data.c` provides the strict decimal IPv4 parser shared by Subnet Lab, Wake-on-LAN, NTP Lab, Modbus TCP, and UDP Console. Its numeric result has the most-significant octet first; socket callers convert with `htonl` at the boundary. Address syntax is independent of platform socket parsers, while each app applies its own unicast/broadcast policy. Input fields retain invalid characters and line breaks for validation, with bounded UTF-8 drafts and enough space to detect excess length.

The ESP32-P4 uses the onboard ESP32-C6 over the Tab5 fixed four-bit SDIO bus for Wi-Fi and BLE. Offline startup is valid; wired tools and the launcher do not depend on cloud availability. HTTP, MQTT, browser, weather, relay, and OTA traffic use bounded buffers and I/O timeouts. HTTP Console still needs an overall deadline for continuously streaming peers, as tracked in the roadmap. TLS certificate bundles verify secure transports. Plain HTTP/MQTT requires an unchanged second tap within five seconds.

HTTP Console checks response framing after the SDK reports completion, so a short Content-Length body or unfinished chunks are shown as incomplete. Its 640 x 300 keyboard appears only while editing and temporarily replaces the response preview; Done/Cancel closes it without sending. The worker owns an immutable request snapshot while the UI may be edited or closed.

Settings opens a blank, masked Wi-Fi password field with an explicit Show/Hide button. Toggling changes only visibility and preserves the current edit; reopening the form resets to masked input without loading the stored credential. Its 640 x 320 keyboard keeps the connection form visible on the portrait screen.

Chat and transcription send authenticated HTTPS requests to a separately deployed relay, which holds the upstream OpenAI API key. The tablet stores only a revocable device token. Generic BLE scanning and connection are explicit; product profiles are off at boot.

## Build and release

`sdkconfig.defaults`, `partitions.csv`, `main/idf_component.yml`, and `dependencies.lock` are release inputs. Compile-time guards reject the wrong ESP-Hosted SDIO pins or a hardware-UART console. Push/PR CI runs host checks and a clean ESP-IDF build. Stable tags additionally create app-only and 16 MiB factory images, an OTA manifest, checksums, and license/dependency notices. See [OTA manifest contract](ota-manifest.md).
