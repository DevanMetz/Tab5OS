# Manual hardware smoke checklist

Record the firmware version/commit, tester, date, power source, SD card, and detected panel. Repeat this checklist on the original ILI9881C/GT911 hardware and the ST7123/ST7121 family before a release.

Use current-limited power and 3.3 V signals. Disconnect external loads before boot; never assume a Grove device is 3.3 V-safe.

## Cold boot and touch

- [ ] Cold boot reaches the launcher without a reset loop, display corruption, or backlight flicker.
- [ ] Tap and drag at all four corners and the center; coordinates and orientation are correct.
- [ ] Cycle 100/75/50/25% brightness, reboot at each value, and confirm the saved value returns without affecting Servo PWM.
  - ST7121, 2026-08-09: all four values returned in the UI and boot backlight log after reboot; physical brightness and Servo interaction remain open.
- [ ] Let the UI dim at two minutes and turn off at 5/10/30 minutes; tap the center time, weather, every forecast-card area, and edges. The first tap only wakes, and the configured brightness returns.
  - ST7121, 2026-08-09: the five-minute path logged 100% -> 20% -> 0% -> 100%, and a first tap over Settings returned to the launcher without opening it. Other hit areas, 10/30/Never, and the second panel family remain open.
- [ ] Select Never and confirm the display remains dimmed rather than turning fully off.

## Missing and present peripherals

- [ ] Boot without an SD card; Notes, logs, and Ebooks report the missing card without hanging or claiming success.
- [ ] Insert a writable SD card, save/read/delete a disposable file, reboot, and confirm it remains readable.
- [ ] Fill a disposable card, then try Notes, a ride, an ebook download, and ring logging; each reports failure and no partial file appears as complete.
- [ ] Remove the card during a ride and during heart-rate logging; the writer stops, System reports removed/unresponsive, and no auto-unmount/remount race occurs. Reinsert and reboot before writing again.
- [ ] Interrupt power during note replacement, ride recording/stop, summary replacement, and heart-rate append; recover `.BAK`, retain useful `.TMP`, and discard an incomplete CSV tail as documented.
- [ ] Erase only the `storage` partition, boot once, and confirm blank internal storage initializes and mounts.
- [ ] Write non-`0xFF` invalid data to `storage`, boot, and confirm it stays unformatted until the two-step System action is confirmed.
- [ ] Start offline with the configured Wi-Fi unavailable; wired tools and the launcher remain usable.
- [ ] Connect Wi-Fi, sync time/weather, use one relay request, disconnect, and reconnect.
- [ ] With a disposable saved network, tap Forget once and let the five-second warning expire without erasing it; then confirm that two taps within five seconds erase it. With no saved profile, Forget remains disabled.
- [ ] In Settings, verify channel/RSSI survey rows and Network Diagnostics link addressing; resolve a hostname, ping LAN/Internet targets, discover mDNS service types, leave during a run, and re-enter without a reset. Repeat with an invalid name, packet loss, disconnected Wi-Fi, an empty mDNS network, and both panel families.
  - ST7121, 2026-08-10: the survey showed channel/RSSI, link details matched `192.168.0.122`, `example.com` resolved, four of four ICMP replies averaged 20 ms, and mDNS found `_airplay`, `_spotify-connect`, `_display`, `_zwift-protocol`, and `_wahoo-fitness-tnp`. Off-screen operations rendered after re-entry; System reported `Reset: USB` and 55 KB free/53 KB minimum internal heap after mDNS startup.
- [x] Send an HTTPS GET with HTTP Console, enable the opt-in SD log, verify the capped body preview and redacted metadata row, then leave during a request and re-enter after it completes.
  - ST7121, 2026-08-10: two `https://example.com/` requests returned HTTP 200 with 559-byte bodies in 885-922 ms; `/HTTP/HTTPLOG.CSV` contained metadata only, off-screen completion rendered after re-entry, and System reported USB reset with 48 KB free/35 KB minimum internal heap after TLS use.
- [ ] Exercise POST/PUT/DELETE, four valid custom headers and every rejection case, a disposable authorization value, query/fragment redaction, redirect visibility, a response over 4 KiB, DNS/TLS/timeout failures, SD full/removal, and both panel families. Verify that plain HTTP requires the same unchanged request to be confirmed within five seconds and that expiry sends nothing.
- [x] Connect MQTT Console to a TLS broker, publish before subscribing with retain enabled on a device-specific loopback topic, receive the retained value, inspect the payload-free metadata log, and return Home during an active connection without blocking the UI.
  - ST7121, 2026-08-10: `mqtts://test.mosquitto.org:8886` accepted an 18-byte QoS 1 retained publish and returned the same topic/payload after subscribe. `/MQTT/MQTTLOG.CSV` contained TX/RX topic and size metadata without payload or credentials; Home returned within 0.82 seconds while cleanup continued. System showed 64 KB free/62 KB minimum heap after the exchange, and a fresh boot of the same image reported USB reset.
- [ ] Exercise authenticated TLS and WSS, QoS 0/2, wildcard subscriptions, fragmented messages, ring overflow, failed auth/DNS/TLS/timeout, SD full/removal, both panel families, and the two-tap cleartext confirmation/expiry path. Confirm that logging and retain both default off on every entry.
- [ ] Save a disposable authenticated TLS/WSS MQTT profile, reboot, and confirm URI/username/password/topic reload exactly. Overwrite it once, verify cleartext Save is refused, let Delete expire without loss, then delete with two taps and confirm the profile stays absent after reboot. Repeat with an intentionally malformed profile blob and confirm Delete remains available without erasing unrelated NVS settings.
- [x] Start BLE GATT scanning explicitly, select a known target, connect explicitly, discover its bounded GATT database, read a standard characteristic, subscribe/unsubscribe a notification, save the evidence snapshot, then leave while connected and confirm teardown completes without reconnecting.
  - ST7121, 2026-08-10: the eight-result scan found `KICKR CORE 4816`; discovery reported 12 services and the disclosed 32-characteristic cap. Manufacturer Name `0x2A29` returned `WahooFitness`; Indoor Bike Data `0x2AD2` delivered `44 00 00 00 00 00 00 00` and unsubscribed. The atomically published CSV contained eight advertisement rows, 32 characteristic rows, and the labeled notification row. Home released the live connection, immediate re-entry was idle, and System reported USB reset with 62 KB free/60 KB minimum internal heap.
- [ ] Repeat with no targets, more than eight advertisers, connection cancel/failure, a device requiring pairing, indications, fragmented/over-64-byte values, a disposable raw-write target and five-second expiry, SD full/removal, each optional product profile, and both panel families.

## Launcher and shared hardware

- [ ] Open every launcher tile, exercise one safe action, and return to the launcher; no stale screen, crash, or unexpected background activity remains.
- [ ] Run empty-bus and known-device I2C scans on G53/G54; both finish and report the correct result.
- [ ] With a 3.3 V BME280 at 0x76/0x77, read register 0xD0 and confirm 0x60; then test an absent address and SDA held low.
- [ ] Watch register 0xD0 at 1 Hz at both 100 and 400 kHz; stop/start repeatedly and confirm the bus is released between samples.
- [ ] Arm a harmless writable test register, let the five-second confirmation expire without a write, then confirm a deliberate two-tap write on a disposable target.
- [ ] Capture at least 15 seconds to SD, confirm the documented header and 1 Hz rows, then repeat with card-full/removal faults and recover the retained `.TMP`.
- [x] Run the UART internal loopback test while stopped; it passes without routing G47/G48.
  - ST7121, 2026-08-09: the UART1 peripheral returned `55 AA 00 FF`; the terminal remained stopped and reported that no external pins were routed.
- [ ] Jumper UART1 G47 TX to G48 RX, test 9600/115200/921600 baud plus parity and stop-bit changes, verify ASCII/hex send history, and confirm both pins are high-impedance after Stop and Home.
- [ ] Capture UART RX/TX to SD, confirm the documented hex CSV rows, then repeat with card-full/removal faults and recover the retained `.TMP`.
- [x] With RS-485 disconnected, select RS-485, Start, and Stop without transmitting; DIR stays receive-safe and the UI remains responsive.
  - ST7121, 2026-08-09: receive-only Start/Stop passed over USB remote control; no bytes were sent, Stop reported receive-safe, UART internal loopback still passed, and Home returned to the launcher.
- [ ] With a second isolated RS-485 transceiver, verify bidirectional ASCII/hex traffic at 9600/115200/921600 baud, automatic direction turnaround, A/B polarity, and no self-echo loss.
- [ ] Measure the physical 120-ohm switch open/closed, use it only at a bus end, and capture RS-485 RX/TX plus retained `.TMP` recovery on SD faults.
- [ ] With SPI pins disconnected, cycle modes 0-3 and all clock choices, then Start/Stop and leave/re-enter ten times without transferring; G18/G19/G5/G45 return high-impedance and the UI stays responsive.
  - ST7121, 2026-08-09: one mode 1/5 MHz disconnected Start/Stop/Home cycle passed over USB without a transfer. The repeated and electrical checks remain open.
- [ ] Jumper SPI MOSI G18 to MISO G19, transfer known 1-byte and 32-byte patterns at 100 kHz/1 MHz/5 MHz/10 MHz, and verify identical RX data. A 33-byte entry is rejected without toggling the bus.
- [ ] With a logic analyzer, verify G45 is active only for each transaction, clock polarity/phase matches modes 0-3, and CS returns high between transfers. Repeat Scope -> SPI -> GPIO transitions on G18/G19.
- [ ] Feed a protected 3.3 V square wave into each Scope input and compare reported frequency/duty at 1/5/20/80 kS/s across representative rates and duty cycles. Confirm unavailable timing is shown when fewer than two complete rising edges exist.
- [ ] Calibrate each Scope input against known low and high DC references, reboot, confirm its bounded offset/scale returns, then reset it to 0 mV/100.0% and reboot again.
- [ ] Save a Scope chart, verify the documented 300-row header/data and 8.3-safe name, then repeat with full-card/removal faults and recover or discard the unpublished `.TMP` honestly.
  - ST7121, 2026-08-10: disconnected G16 showed timing unavailable; +10 mV/102.5% survived Home/re-entry and a hard reset, neutral reset saved, and `00285100.CSV` published with calibrated 5 kS/s rows. Known-waveform accuracy and storage faults remain open.
- [ ] Open a saved Scope CSV from Files, tap the graph for a value, zoom and pan, and compare the visible min/max/average with the CSV. Repeat with an I2C capture containing a failed read; the failed sample appears as a gap. Back returns to the same folder without losing SD access.
  - ST7121, 2026-09-22: `v0.5.1-2-gb900d6d-dirty` (ELF SHA-256 prefix `2825df562`) passed app-only COM7 flash hash verification and boot self-checks. Files opened the existing 300-row `00285100.CSV`, showed 0.000-59.800 ms and 164/180/167 mV visible min/max/average, displayed a 42.600 ms/180 mV cursor, updated zoom/pan, and returned to the same folder. A new 19-row empty-bus I2C capture opened with no successful readings; its cursor marked a failed read. System still showed the SD mounted, USB reset, and 62 KB free/57 KB minimum internal heap. Raw CSV cross-check, a successful I2C reading, and the second panel family remain open.
- [ ] With G6 disconnected, open Signal Generator, cycle every frequency/duty/pulse choice, Start/Stop repeatedly without sending a pulse, and return Home; the launcher remains responsive and G6 is high-impedance while stopped.
  - ST7121, 2026-08-10: every frequency, duty, and pulse choice plus repeated Start/Stop and Home/re-entry passed over USB with G6 disconnected. The driver reported each requested frequency exactly; electrical high-impedance remains unmeasured.
- [ ] With a logic analyzer on G6, measure 50/100/1000/10000/50000 Hz at 10/25/50/75/90% duty. Record requested versus actual values and confirm the backlight does not change.
- [ ] Measure single 10/100/1000/10000 us high pulses, confirm the line starts low and releases afterward, then let PWM reach its five-minute automatic stop.
  - ST7121, 2026-08-10: the verified-active 1 kHz/25% run returned to `START PWM`, `Output: stopped`, and `Five-minute safety timeout reached; G6 released` after five minutes without a reboot or USB loss. Pulse timing and the physical line state remain unmeasured.
- [ ] Repeat GPIO G6 -> Signal Generator -> GPIO G6 transitions and verify no stale LEDC routing, pull, or output remains. Use an external buffer before testing any load.
- [ ] With USB-A empty, enter Settings -> USB keyboard host, turn it on/off ten times, and leave/re-enter ten times; the UI remains responsive and Type-C flashing/remote desktop still works.
- [ ] Connect a low-power wired USB keyboard, type shifted letters, punctuation, Backspace, arrows, Home/End, and Enter; unplug/replug it ten times and continue typing without rebooting.
- [ ] Leave the USB keyboard page while connected and verify USB-A 5 V turns off; return, enable it, and verify the keyboard re-enumerates. Repeat on both panel families before starting CDC/VCP work.
- [ ] Scope or meter external 5 V through cold boot, soft reset, I2C entry/exit, and Servo entry/exit; it stays off without any pulse.
- [ ] Transition I2C -> Servo -> GPIO -> Scope -> I2C, stopping each tool first; the backlight does not change and G0/G53/G54 are not left driven. Verify Servo PWM on G0 and the LED driver signal on G54, including GPIO G0 -> Servo -> GPIO G0 transitions.
  - ST7121, 2026-09-19: `v0.5.1-1-ga7ccd42-dirty` (ELF SHA-256 prefix `3ae6fc9c0`) built and flashed over USB COM7 with a verified flash hash. Reboot loaded the matching image from `0x20000` and passed startup self-checks; USB screen capture showed Servo Toy running with `G0: servo signal` and `G54: LED driver signal`. External wiring, waveform measurements, and G0 handoff checks remain unverified.
- [ ] In Servo Toy, select Sine and verify each full-cycle frequency (0.05/0.10/0.20/0.25/0.50/0.75/1.00 Hz) and all three ranges. Change frequency during a run without a phase reset, then verify Random mode, mode-change stop, Stop/Home, and the five-minute timeout. Measure the G0 waveform independently; the 50 Hz servo PWM carrier is separate from the selected motion frequency.
  - ST7121, 2026-09-19: `v0.5.1-1-ga7ccd42-dirty` (ELF SHA-256 prefix `b8f10098a`) passed native and startup sine-math checks and a verified COM7 flash. USB control cycled the frequency/range/random-speed settings; a short Sine run showed changing commanded positions, accepted a live 0.25 -> 0.50 Hz change, and stopped when switched to Random. System retained USB reset, 62 KB free/57 KB minimum internal heap, and 23427 KB free PSRAM before/after. Left stopped in Sine at 0.25 Hz. Boot and screen evidence is in local `build/servo-sine-*` files. External servo wiring/power, electrical timing, physical tracking, and the five-minute timeout were not measured in this check.
- [x] Repeat mixed launcher enter/exit transitions 100 times and record initial/final free and minimum heap.
  - ST7121, 2026-08-10: after app-entry scroll reset was added, 50 opens and 50 Home returns across Files, Notes, Counter, GPIO, Settings, Chat, Browser, Ebooks, Clock, and System completed in 42 seconds. The launcher returned to row zero, Reset remained USB, internal heap stayed 70 KB free/65 KB minimum, and PSRAM stayed 23516 KB free. Repeat on ILI9881C and complete the eight-hour soak.

## Update and recovery

- [ ] Publish a disposable stable release containing `tab5_os.bin`, `tab5_os.json`, and `SHA256SUMS`; confirm all three describe the same bytes and the embedded application version.
- [ ] From a clean Windows ESP-IDF 5.4.2 setup, verify `SHA256SUMS`, write `tab5_os_factory.bin` at offset zero, and confirm bootloader, partition table, initial OTA state, launcher, and expected blank NVS/SPIFFS state. Repeat on both panel families.
- [ ] Install through the stable manifest and confirm the updater rejects, without changing the boot partition: malformed/oversized JSON, beta channel, wrong hardware, non-newer version, unsupported predecessor, URL outside the tagged release path, wrong Content-Length/byte count, wrong embedded version, and wrong SHA-256.
- [ ] A valid OTA image boots, passes its health window, and preserves settings and SD data.
- [ ] An intentionally invalid candidate rolls back to the previous working image.
- [ ] System records the successful install or rollback result after reboot rather than only the current partition state.
- [ ] Complete one documented USB recovery flash and cold boot.

Pass only when every applicable item is checked on both hardware families and failures include logs plus reproduction steps.
