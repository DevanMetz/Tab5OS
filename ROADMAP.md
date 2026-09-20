# Tab5 OS long-term development plan

Updated: 2026-08-10

This roadmap assumes one primary maintainer with occasional testers. Target windows express order and intent, not promises; a phase ships only when its exit gate passes on hardware.

## Mission

Make Tab5 OS the fastest portable way to connect, discover, inspect, control, log, and recover a 3.3 V embedded or IoT target.

Tab5 OS should complement calibrated bench instruments, not imitate them. Version 1.0 is successful when a user can bring up a typical sensor or controller, capture useful evidence, export it, and safely recover the tablet without needing a laptop.

## Product boundaries

### Core jobs

1. Connect safely to wired, BLE, Wi-Fi, and USB devices.
2. Discover addresses, services, peers, and protocol settings.
3. Inspect live signals, registers, packets, and device state.
4. Control pins and send transactions with explicit safeguards.
5. Log timestamped sessions to user-owned storage and export them.
6. Recover from bad settings, storage faults, and failed updates.

### Explicit non-goals

- No desktop-style OS, package manager, or native app store.
- No general-purpose scripting runtime before stable tool operations exist.
- No cloud account requirement; cloud-backed features remain optional.
- No promise of calibrated, protected, or 5 V-safe measurements without validated external hardware.
- No new one-off BLE product app until the generic BLE path is usable.
- AI, weather, browser, and ebook features stay maintained, but they do not outrank the electronics and IoT core.

## Starting point

The released baseline is `v0.5.1`. It already boots both known display families and provides storage, notes, GPIO control, an ADC scope, Wi-Fi, clock and alarms, weather, chat and transcription, a reader browser, ebooks, USB remote desktop, and rollback OTA.

The current worktree is materially ahead of that release: it adds external I2C scanning, register read/watch/write, and capture, Govee and COLMI BLE support, cycling telemetry, and servo control. Those features are experimental until the v0.6 gate below passes.

### Execution checkpoint: 2026-08-09

Phase 0 software work is implemented and builds cleanly:

- [x] Servo owns LEDC timer 1/channel 2 and releases G53/G54 safely without touching the backlight.
- [x] Govee, Ring, and KICKR Bluetooth activity is off at boot and requires an explicit user toggle.
- [x] GPIO, Scope, Servo, and external I2C share the app-exit release path; app switching waits for Scope teardown before reusing the pins.
- [x] External 5 V remains off at boot and has no implicit enable path.
- [x] OTA blocks active writers, capture, BLE sessions, and outputs; navigation stays locked during install.
- [x] OTA rollback validation waits for a 30-second healthy LVGL window, and NVS failures preserve settings instead of erasing them.
- [x] The internal partition and runtime both use SPIFFS; only provably blank storage auto-initializes, while damaged data requires explicit confirmation.
- [x] Push/PR verification, release separation, artifact scoping, and a two-panel hardware checklist are present.
- [x] ST7121 hardware boots through the launcher with SDIO Wi-Fi/BLE, touch, SD, System diagnostics, and stable heap; unused LVGL rotation/PPA paths are disabled.
- [x] Firmware builds reject stale configurations that do not use the fixed Tab5 ESP32-C6 SDIO pin map.
- [x] ST7121 completed a corrected 100-transition run across ten safe apps after app-entry scroll reset was added: the launcher returned to row zero, the USB boot stayed continuous, internal heap remained 70 KB free/65 KB minimum, and PSRAM remained 23516 KB free before and after.
- [x] Forgetting a saved Wi-Fi network now requires an unchanged second tap within five seconds; the control is disabled when no profile is saved.
- [ ] Run the hardware matrix on both display/touch families, including OTA rollback and missing-peripheral cases.
- [ ] Repeat the transition/pin test on ILI9881C hardware and complete the eight-hour soak.
- [ ] Review and land the work as separate commits, then tag v0.6 only after every hardware gate passes.

Phase 1 software progress is also underway:

- [x] System reports the firmware/commit, reset reason, uptime, panel, heap, storage capacity, current OTA state, and persistent last OTA result.
- [x] Notes and ride summaries use recoverable backup replacement; rides publish only after durable sync, and health logs repair incomplete final rows.
- [x] Built-in SD paths, timestamps, units, 8.3-safe collision names, `.TMP`, and `.BAK` behavior are documented.
- [x] Persisted brightness choices, a dimmed two-minute screen, and configurable 5/10/30-minute or Never screen-off behavior are implemented.
- [x] ST7121 UI and boot logs verify 100/75/50/25% brightness persistence plus the two-minute dim, five-minute off, and first-tap-only wake path without a watchdog or panic.
- [x] Source-built complete flashing, app-only flashing, destructive clean recovery, and post-recovery checks are documented.
- [x] USB Serial/JTAG is the build-enforced sole console; COM7 flashing, ROM-to-launcher logs, Wi-Fi startup, and remote desktop pass on ST7121, leaving hardware UARTs available to tools.
- [ ] Validate SD removal/full-card behavior, ten representative power cuts, display wake behavior, and energy impact on hardware.
- [x] Launcher metadata now lives in one static app table; Scope and Settings use explicit leave hooks, and other cleanup moves only when each app is materially touched.

The first Phase 2 workflow is implemented in software:

- [x] External I2C supports 100/400 kHz byte reads, a 1 Hz watch, five-second two-tap raw writes, and durable timestamped SD capture without retaining the bus between samples.
- [x] On ST7121 hardware, the no-pull-up bus reports timeout safely; the speed control, write-confirmation expiry, watch start/stop, Home teardown, and heap baseline pass over USB.
- [ ] Validate known and stuck devices, successful transfers at both speeds, capture recovery, and repeated G53/G54 transitions on hardware.
- [x] UART1 uses the documented G47 TX/G48 RX 3.3 V pin pair with baud/parity/stop controls, ASCII/hex receive and send, timestamps, send history, durable SD logging, and deterministic high-impedance teardown; its unrouted internal loopback passes on ST7121 hardware.
- [ ] Validate UART with a physical G47/G48 loopback and external 3.3 V target across representative line settings.
- [x] The same serial terminal supports the onboard SIT3088 RS-485 path on G20/G21/G34 with automatic half-duplex direction, physical-termination guidance, durable logging, and receive-safe teardown; disconnected receive-only Start/Stop passes on ST7121 hardware without regressing UART loopback.
- [ ] Validate bidirectional RS-485 traffic, polarity, turnaround, termination, and fault recovery with an external transceiver fixture.
- [ ] USB keyboard and CDC/VCP host remain gated: linking the ESP-IDF 5.4.2 USB host/HID path currently makes the combined ESP-Hosted image fail during pre-scheduler task creation, before USB application code runs. Re-test this first on the migration target; do not expose USB-A power until the combined image survives disconnect/reconnect testing.
- [x] SPI2 master uses the documented M5-Bus MOSI G18, MISO G19, SCK G5, and explicit CS G45 pins with modes 0-3, 100 kHz-10 MHz clocks, 32-byte full-duplex transfers, and deterministic high-impedance teardown.
- [ ] Validate SPI with a physical G18/G19 loopback and target device across all modes and representative clocks; verify CS timing and shared Scope/GPIO transitions with a logic analyzer.
  - ST7121, 2026-08-09: the compact launcher, SPI page, mode 1/5 MHz selection, disconnected Start/Stop, and Home teardown pass over USB without transmitting. Electrical release and loopback remain open.
- [x] Signal Generator uses M5-Bus G6 and isolated LEDC timer 2/channel 3 for 50 Hz-50 kHz PWM, 10-90% duty, bounded 10 us-10 ms single pulses, five-minute automatic stop, and high-impedance teardown.
- [ ] Measure every PWM frequency/duty and pulse width, automatic timeout, backlight isolation, and GPIO transitions on both hardware families before treating the output as characterized.
  - ST7121, 2026-08-10: all five LEDC frequency configurations and all five duty choices were accepted, with the driver reporting each exact requested frequency. All four pulse actions, Home/re-entry, and the verified five-minute automatic-stop path completed over USB. Electrical timing, actual high-impedance release, backlight isolation, pin transitions, and the other panel family remain open.
- [x] Scope reports frequency and duty from complete rising-edge spans, applies bounded persistent scale/offset per input after ESP-IDF calibration, resets to neutral, and atomically publishes the visible 300-point chart to SD.
- [ ] Validate Scope timing across known frequencies/duties and sample rates, calibrate every input against physical references, and exercise full-card/removal capture failures.
  - ST7121, 2026-08-10: disconnected G16 correctly showed timing unavailable; +10 mV/102.5% survived app re-entry and a hard reset, neutral reset saved, and a final CSV with the documented calibrated 5 kS/s rows was browsable on SD.

Phase 3 software progress has begun:

- [x] Settings scans report Wi-Fi channel and RSSI; Network Diagnostics reports link addressing, performs bounded DNS lookup and four ICMP echo probes, and enumerates up to eight standard DNS-SD mDNS service types without blocking LVGL or OTA restart safety.
- [x] On ST7121 hardware, `example.com` resolved and returned four of four pings; mDNS found five real service types, off-screen operations rendered after re-entry, System reported a USB reset rather than panic, and internal heap remained 55 KB free/53 KB minimum after mDNS startup.
- [ ] Validate disconnected, unresolvable, partial-loss, empty-mDNS, and second-panel cases before calling the network-diagnosis slice complete.
- [x] HTTP Console performs bounded GET/POST/PUT/DELETE requests, verifies HTTPS with the certificate bundle, refuses automatic redirects, caps textual response previews at 4 KiB, validates up to four custom headers, and requires a matching second tap within five seconds before sending plain HTTP.
- [x] HTTP SD logging is explicitly enabled per session and records only timestamp, method, query-free URL, status, response size, duration, and outcome; request headers/bodies and response content are never exported.
- [x] On ST7121 hardware, two `https://example.com/` GETs returned HTTP 200 with 559-byte bodies in 885-922 ms; the metadata CSV was browsable, an active request completed safely off-screen and rendered after re-entry, System still reported `Reset: USB`, and internal heap was 48 KB free/35 KB minimum after TLS use.
- [ ] Validate POST/PUT/DELETE with a disposable endpoint, custom authorization without persistence, query/fragment redaction, redirect and over-4-KiB responses, timeout/DNS/TLS failures, the plain-HTTP confirmation/expiry path, SD faults, and the second panel family.
- [x] MQTT Console supports `mqtts`/`wss` with certificate-bundle verification, gates cleartext `mqtt`/`ws` behind an unchanged five-second confirmation, and exposes one bounded topic/filter, QoS 0/1/2, retain state, an eight-message receive ring, and capped display history.
- [x] A TLS/WSS broker profile can be saved explicitly as one validated, versioned NVS blob and removed with a five-second two-tap action. Cleartext profiles remain memory-only; developer settings flash is disclosed as unencrypted; SD exports never contain the username, password, or payload.
- [x] MQTT SD logging is opt-in per session and records only timestamp, direction, topic, QoS, retain state, payload byte count, and outcome; credentials and payloads are never exported. Disconnect and Home teardown run off the LVGL thread while remaining visible to the OTA restart blocker.
- [x] On ST7121 hardware, Tab5 connected to the public TLS test broker on port 8886 and exchanged an 18-byte QoS 1 retained message on its device-specific loopback topic. The metadata-only CSV contained matching TX/RX rows, Home returned to the launcher within 0.82 seconds while cleanup continued, System showed 64 KB free/62 KB minimum heap after the exchange, and a fresh boot of the same image reported `Reset: USB`.
- [ ] Validate authenticated TLS and WSS brokers, saved-profile reboot/overwrite/delete/corruption recovery, cleartext confirmation/expiry, QoS 0/2, wildcard subscriptions, fragmentation and receive-ring overflow, failed auth/DNS/TLS/timeout paths, SD full/removal, and the second panel family. NVS encryption remains part of the opt-in hardened-device profile.
- [x] BLE GATT Explorer reuses the shared NimBLE scanner but never starts it or connects automatically. It caps results at eight devices, 16 services, and 32 characteristics; requires product-specific BLE profiles to be off; exposes UUIDs, handles, and properties; supports explicit reads and one notification/indication subscription; gates 1-64-byte raw writes behind an unchanged five-second confirmation; and atomically exports the bounded evidence snapshot to SD only after an explicit tap.
- [x] On ST7121 hardware, an explicit scan filled the eight-device cap and found the owned `KICKR CORE 4816`. Read-only discovery reported 12 services and the disclosed 32-characteristic cap; Manufacturer Name returned `WahooFitness`; Indoor Bike Data `0x2AD2` subscribed, delivered an 8-byte notification, and unsubscribed. The published CSV contained eight advertisements, 32 characteristic rows, and the correctly labeled notification row. Home released the live connection, immediate re-entry was idle, System reported `Reset: USB`, and internal heap remained 62 KB free/60 KB minimum.
- [ ] Validate no-target and overflowing scans, connection cancel/failure, pairing/authentication errors, indications and fragmented/over-64-byte values, the raw-write confirmation/expiry path on a disposable target, SD full/removal and retained-temp behavior, interaction with each optional product profile, and the second panel family.

Phase 4 release hardening has started in software:

- [x] Stable OTA now consumes a bounded schema-1 manifest and rejects the wrong hardware/channel, invalid or non-newer versions, unsupported predecessors, oversized images, and URLs outside the immutable tagged-release path.
- [x] The updater streams SHA-256 over the exact downloaded `.bin`, verifies byte count, HTTP length when available, and embedded application version, and aborts before boot selection on any mismatch. Tag CI publishes the binary, manifest, and `SHA256SUMS`; prerelease tags are excluded from the stable channel.
- [x] Tag CI also publishes a native ESP-IDF 16 MiB merged factory/recovery image, Apache and LVGL license/notice files, and the pinned dependency lock. App-only, complete-source, and destructive factory paths plus the current partition/NVS/filesystem migration contract are documented.
- [x] Architecture, troubleshooting, privacy, contribution, security-reporting/support, conduct, compatibility, installation/recovery, data-format, and pin-safety contracts are now repository-owned documentation.
- [ ] Add independent release signing and verify the signature with an embedded public key; HTTPS/GitHub control remains the trust root until then.
- [ ] Exercise valid, malformed, incompatible, downgrade, truncated, wrong-size, wrong-version, and wrong-hash manifests plus rollback on hardware before enabling the manifest path in a stable release.
- [ ] Verify the released factory image from checksums on a clean Windows machine, recover a fully erased Tab5, and confirm both display families plus deliberate NVS/SPIFFS erasure before calling distribution complete.

Current strengths:

- A working ESP-IDF/LVGL product rather than a scaffold.
- Pinned dependencies and a reproducible release container.
- Dual 6 MB OTA slots with substantial image headroom.
- HTTPS transport, revocable relay credentials, and OTA rollback support.
- Small parser/math self-checks plus relay and desktop-tool checks.

Current constraints and risks:

- Nearly all firmware lives in one roughly 7,000-line `main/main.c`.
- The static launcher/lifecycle table exists, but most legacy app timers, pins, and UI pointers still use the shared manual cleanup path until those apps are touched.
- G53/G54 remain shared by GPIO, ADC, Servo, and external I2C, so their software release path still needs hardware transition testing.
- OTA metadata and image hashes are verified through a versioned stable manifest, but the manifest is not yet independently signed; GitHub repository control and Web PKI remain trusted.
- SD writers now preserve or repair incomplete data, but removable/full-card behavior and retained-temp recovery still need field validation.
- Reverse-engineered wearable work is excluded from release inputs but still needs a provenance decision before publication.
- The stable-release process still depends on manually completing the hardware gate before a tag is pushed.
- The factory BSP remains pinned to ESP-IDF 5.4.2; the published support matrix lists the 5.4 line as end-of-life on 2027-07-05.

## Roadmap summary

| Target window | Release | Outcome |
| --- | --- | --- |
| Aug-Sep 2026 | v0.6 | Turn the current worktree into a safe, testable field beta |
| Oct-Dec 2026 | v0.7 | Establish reliability, CI, recovery, and shared capture conventions |
| Jan-Apr 2027 | v0.8 | Deliver the core wired electronics bench |
| May-Aug 2027 | v0.9 | Add generic IoT commissioning and complete the IDF migration |
| Sep-Dec 2027 | v1.0 | Ship a documented, signed, field-ready stable release |
| 2028 | v1.x | Add repeatable projects, profiles, USB workflows, and optional hardware expansion |

## Phase 0: make the current work releasable

Target: v0.6, Aug-Sep 2026

Outcome: preserve the new capability while removing the immediate hardware and release hazards.

### Deliverables

- Land the current I2C, BLE sensor, cycling, ring, and servo work as separate reviewable changes.
- Move the servo to a timer/channel that cannot alter the display backlight.
- Make BLE scanning and connection user-controlled; never connect to a KICKR or ring merely because it is nearby.
- Give G53/G54 and other shared outputs one deterministic acquire/release path. Leaving an app must restore safe input/high-impedance or explicitly off state.
- Stop enabling external 5 V by default; keep it unavailable until an explicit UI control and signal-level guidance are implemented.
- Block OTA and shutdown while a capture, SD writer, microphone, ADC sampler, BLE control session, or external output is active; graceful quiescing can follow later.
- Delay OTA validation until display, storage, settings, and the main event loop have remained healthy for a meaningful window.
- Stop silently erasing all NVS settings on a recoverable initialization fault; surface recovery and preserve credentials where possible.
- Resolve the `littlefs` partition versus SPIFFS mount mismatch and choose one internal filesystem before storing user data there.
- Add push/PR CI for a clean firmware build, relay tests, Python checks, `git diff --check`, and image-size reporting.
- Keep stock/patched wearable binaries and uncertain decompiled material out of release artifacts pending provenance review.
- Record a manual hardware smoke checklist covering both panel/touch variants, SD present/absent, Wi-Fi offline/online, BLE absent/present, and every launcher transition.

### Exit gate

- A clean clone builds and produces the same release inputs as CI.
- Both display variants pass cold boot, touch, storage, Wi-Fi, and OTA recovery checks.
- One hundred mixed app enter/exit transitions leave no pin driven, timer repurposed, or material heap loss.
- An empty I2C bus, missing SD card, absent BLE devices, and offline Wi-Fi all fail safely.
- An eight-hour idle/mixed-use soak completes without reset.
- The release contains no vendor wearable firmware.

## Phase 1: reliable field beta

Target: v0.7, Oct-Dec 2026

Outcome: make every existing tool predictable before adding another protocol surface.

### Deliverables

- Show firmware version/commit, reset reason, uptime, panel type, free/minimum heap, storage state, and last OTA result in System.
- Use temp-file-plus-rename writes for notes, captures, ride summaries, and health logs where the filesystem supports it.
- Standardize session paths, timestamps, CSV headers, units, and filename collision handling.
- Add brightness, screen timeout, and conservative idle behavior; measure battery impact of Wi-Fi and continuous BLE scanning.
- Make SD removal/full-card errors visible and prevent partial files from appearing successful.
- Add a documented factory-flash and recovery path alongside app-only flashing.
- Introduce one static app table with name, icon, enter, and leave callbacks, then migrate apps into it only as they are touched. One lifecycle path should replace the growing app-specific cleanup list.
- Extract code from `main.c` only when it is being changed: new apps get their own source file; shared hardware code appears only after a second real consumer.
- Keep stable releases milestone-based. Add candidate/prerelease tags only when there are external testers; do not create release-channel machinery before then.

### Exit gate

- Ten power interruptions during representative writes leave previous data readable and no corrupt file reported as complete.
- Ten OTA install/rollback cycles preserve settings and user data.
- A repeated 100-cycle launcher soak stays within an agreed heap baseline recorded in the release notes.
- Every hardware app documents pins, voltage limits, power state, and exit behavior on-screen.

## Phase 2: core wired electronics bench

Target: v0.8, Jan-Apr 2027

Outcome: bring up a common sensor or microcontroller entirely offline.

### Deliverables, in order

1. I2C register read, write, watch, bus-speed selection, and CSV capture. Raw writes require an explicit confirmation.
2. UART terminal with safe pin choices, baud/parity/stop settings, ASCII/hex views, timestamps, send history, and SD logging.
3. RS-485 terminal using the onboard transceiver, with documented direction and termination behavior.
4. USB keyboard input and CDC/VCP serial host after the existing USB host path passes disconnect/reconnect testing.
5. SPI master transaction console with explicit chip-select and bounded transfer sizes.
6. General PWM/frequency/duty/pulse output built from the proven servo work without sharing display resources.
7. Frequency and duty-cycle measurement plus ADC offset/scale calibration and scope CSV export.

A multi-channel logic capture is a feasibility experiment, not a promise. It enters the release only if measured sample rate, channel count, memory use, and signal integrity are useful and honestly documented.

### Exit gate

- A loopback fixture verifies UART, RS-485, SPI, PWM, and frequency measurement.
- A user can identify an unknown I2C device, inspect a register, watch a controller's boot log, exercise an output, and export a timestamped capture without a PC.
- All tools return pins and peripherals to a safe state when stopped, navigated away from, disconnected, or timed out.
- Calibration values are visible, persistent, bounded, and resettable.

## Phase 3: generic IoT commissioning

Target: v0.9, May-Aug 2027

Outcome: commission common BLE and network devices without compiling a custom app for each product.

### Deliverables

- Replace product-driven background BLE behavior with a generic scanner and GATT explorer: advertisements, services, characteristics, read/write, notifications, and explicit connection controls.
- Keep Govee, Ring, and KICKR as optional views or data profiles over shared BLE machinery.
- Add Wi-Fi channel/RSSI survey, address details, DNS lookup, ping, and mDNS discovery.
- Add a bounded HTTP request console and MQTT publish/subscribe with TLS, retained-message visibility, and timestamped logs.
- Store credentials in NVS; exported project/session files exclude secrets by default.
- Start an ESP-IDF migration spike no later than Jan 2027 and complete migration by May 2027, leaving time before the 5.4 support deadline. Select the newest line that passes Tab5 display, ESP-Hosted Wi-Fi/BLE, OTA, audio, and USB tests; do not upgrade solely for a version number.
- Keep the old toolchain branch only for time-bounded critical fixes after migration.

### Exit gate

- The hardware matrix passes on a supported ESP-IDF line with no display or wireless regression.
- A user can discover and subscribe to a generic BLE characteristic, diagnose a network path, publish and receive an MQTT message, and export the evidence.
- Offline operation remains intact; cloud or broker outages do not block wired tools.

## Phase 4: field-ready 1.0

Target: v1.0, Sep-Dec 2027

Outcome: define a stable product and recovery contract.

### Deliverables

- Replace direct `latest` OTA consumption with a versioned manifest containing version, hardware compatibility, size, SHA-256, URL, release channel, and minimum supported predecessor.
- Sign release images and verify signatures during OTA while preserving normal developer builds. Offer a separately documented hardened-device profile before enabling irreversible secure-boot or flash-encryption eFuses.
- Publish app-only and factory/recovery images, checksums, LICENSE, third-party notices, and useful release notes.
- Document partition, NVS, settings, and SD-format compatibility plus migration policy.
- Add installation, recovery, pin-safety, architecture, troubleshooting, privacy, contribution, and security-reporting documentation.
- Protect stable releases with CI and a hardware sign-off checklist. Add branch protection when contribution volume makes it useful.
- Freeze only the user-visible data formats and operations proven by v0.8/v0.9; do not freeze a plugin ABI.

### 1.0 release gate

- Both panel variants, battery/no-battery operation, SD absent/present/full, Wi-Fi offline/online, and BLE absent/present pass the published matrix.
- An eight-hour mixed-workload soak and one hundred app transitions produce no crash, unsafe output, or material memory regression.
- A bad candidate image rolls back; a good image is not marked valid until the health window passes.
- Factory recovery works from documented steps on a clean Windows machine and the CI container build is reproducible.
- Five canonical workflows pass end-to-end: I2C inspection, UART/RS-485 capture, signal generation/measurement, BLE GATT subscription, and MQTT commissioning.

## Phase 5: repeatable workflows, not an app store

Target: v1.x through 2028

Outcome: turn isolated measurements into reusable field projects without creating a second operating system inside the firmware.

### Candidate work, promoted only by demonstrated demand

- SD project folders containing notes, non-secret connection profiles, pin assignments, captures, and exports.
- A small USB/remote CLI for screenshots, logs, file transfer, and starting/stopping existing captures.
- Declarative device profiles for I2C registers, BLE characteristics, MQTT topics, and simple decoders.
- Constrained recipes using existing operations such as read, write, wait, assert, and log. No arbitrary native code or general-purpose interpreter.
- Camera-assisted QR/barcode provisioning, IMU logging, USB mass-storage workflows, and supported LoRa/cellular modules.
- Thread/Zigbee only when the ESP32-C6 hosted transport and upstream support are stable enough to test and maintain.
- A protected companion board with level shifting, buffered logic inputs, and improved analog front ends only after repeated users establish real voltage, bandwidth, accuracy, and connector requirements.

### Promotion rule

A profile/recipe format becomes stable only after at least three materially different devices use it without firmware-specific exceptions. A new shared abstraction follows the same rule.

## Engineering policy

### Architecture

- Keep ESP-IDF, FreeRTOS, LVGL, NVS, SPIFFS, and ordinary files. Do not rewrite the working product.
- New substantial apps live in their own source files. Existing apps move only when being materially changed.
- Use one owner at a time for each pin, bus, LEDC timer/channel, ADC unit, BLE connection, and storage writer.
- Every app or service has a deterministic stop path; background work cannot retain deleted LVGL objects.
- Keep NVS for small settings, SPIFFS for built-in state, and microSD for user logs. Do not add a database until file formats measurably fail.

### Verification

- Keep the smallest on-device self-check for pure parsers and calculations.
- Add host tests when logic can run without ESP-IDF; do not mock the entire board.
- Run clean firmware builds and existing relay/tool tests before merge, not first on a tag.
- Maintain a manual hardware matrix until at least three repeatable fixtures justify automated hardware-in-loop testing.
- Record firmware size, minimum internal heap, PSRAM use, boot time, and soak results for each stable release. Regressions need an explicit reason, not an arbitrary universal limit.

### Security and privacy

- Keep API keys off the tablet and repository; use independently revocable per-device relay tokens.
- Apply size/rate limits at network boundaries and avoid exporting secrets.
- Use HTTPS plus signed OTA images before 1.0. Secure boot, flash encryption, anti-rollback eFuses, and locked debug interfaces belong to an opt-in production profile because they permanently change a development device.
- Publish a security contact and support window with 1.0.

## Prioritization rule

Work enters a milestone only if it materially improves one of the six core jobs: connect, discover, inspect, control, log, or recover. Within a milestone, order work by:

1. Prevent hardware damage or data/credential loss.
2. Restore build, boot, update, or recovery reliability.
3. Complete an end-to-end field workflow.
4. Remove repeated maintenance cost proven in at least two places.
5. Add optional polish.

## Next implementation queue

1. Run the checked smoke matrix on both display/touch families, including backlight/Servo and every G53/G54 transition.
2. Complete the eight-hour mixed-use soak; the corrected ST7121 100-transition resource/heap test passes, while ILI9881C remains part of the two-panel matrix.
3. Exercise ten OTA install/rollback cycles while preserving NVS and SD data.
4. Land the current work as reviewable commits and publish v0.6 only after those gates pass.
5. Validate Phase 1 atomic writes, display wake, and SD full/removal behavior on hardware.
6. Migrate each legacy app's cleanup into the static lifecycle table only when that app is materially changed.
7. Hardware-validate the completed I2C inspect/control/capture workflow, including retained `.TMP` recovery.
8. Hardware-validate UART1 and the implemented onboard RS-485 path with external loop fixtures.
9. Hardware-validate the SPI master console, CS timing, transfer bound, and G18/G19 shared-pin teardown.
10. Hardware-characterize the G6 PWM/pulse generator across its full range and verify timer/backlight isolation.
11. Hardware-characterize Scope frequency/duty, per-input calibration, CSV publication, and storage-fault behavior.
12. Begin the supported-IDF migration spike by Jan 2027.

## External references

- [M5Stack Tab5 hardware documentation](https://docs.m5stack.com/en/core/Tab5)
- [M5Stack Tab5 factory user demo](https://github.com/m5stack/M5Tab5-UserDemo)
- [ESP-IDF support periods](https://docs.espressif.com/projects/idf-build-apps/en/latest/others/CONTRIBUTING.html#supported-esp-idf-versions)
- [ESP32-P4 security guidance](https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32p4/security/security.html)
- [ESP32-P4 USB host guidance](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32p4/api-reference/peripherals/usb_host.html)
