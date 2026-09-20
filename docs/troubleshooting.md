# Troubleshooting

Start with a serial log and the System page. Record the firmware version/commit, reset reason, panel, NVS/internal/SD state, free/minimum heap, PSRAM, Wi-Fi, OTA state, and the first error. Do not repeatedly erase flash before preserving that evidence.

## Build fails before compilation

- Delete the ignored generated `sdkconfig` and rebuild if the compile-time message says Tab5 requires SDIO CLK12/CMD13/D0-D3 11/10/9/8/RESET15 active-low. Defaults seed only a new config; they do not override an old generated one.
- Use ESP-IDF 5.4.2 and `idf.py set-target esp32p4`. A different IDF or target is not a supported recovery environment yet.
- If the console guard fails, regenerate config with USB Serial/JTAG as the sole console. Hardware UARTs are reserved for tools.
- Run `git diff --check`, relay tests, storage tests, Python checks, then a clean firmware build before blaming CI.

## Device or serial port is missing

- Use a data-capable USB-C cable and direct host port. Close the serial monitor or remote desktop; only one process can own the COM port.
- Hold Reset until the green LED flashes rapidly to enter download mode, then locate the Espressif USB Serial/JTAG port in Device Manager.
- A disappearing port during recovery does not require another erase. Return to download mode and repeat the write.

## Boot loop, blank display, or touch failure

- Capture ROM-to-failure logs. `Starting Tab5 OS` proves the pure startup checks passed, not that hardware passed.
- Confirm the log selects a known ILI9881C/GT911 or ST7121/ST7123 path. Treat fallback/unknown touch-controller messages as a hardware gate failure.
- Do not re-enable LVGL software rotation or experimental PPA drawing to work around another problem; both were disabled for measured memory/rendering faults on the native portrait display.
- Use an app-only flash only with the current partition table. Otherwise use a complete source flash; use the factory image only when deliberate credential/internal-data erasure is acceptable.

## Wi-Fi or BLE unavailable

- Offline boot is supported. Settings should report hardware unavailable rather than block the launcher.
- A generated SPI ESP-Hosted configuration is wrong for Tab5; regenerate the fixed SDIO configuration.
- Saved Wi-Fi credentials may need to be entered again after a factory erase or explicit Forget action. Forget requires two taps within five seconds.
- Generic BLE and Govee/Ring/KICKR profiles are opt-in and mutually exclusive. Turn the other profiles off before opening the GATT Explorer.

## Storage unavailable

- Missing SD is valid. Insert a known FAT-formatted card and reboot; Tab5 has no card-detect pin and does not hot-remount writers safely.
- On full/removal errors, stop writing and preserve `.TMP`/`.BAK` evidence. Do not repeatedly retry against an unresponsive card.
- A blank internal `storage` partition may initialize automatically. Nonblank corrupt SPIFFS is deliberately left unavailable until explicit recovery; never claim it was formatted automatically.
- See [Data formats](data-formats.md) before manually renaming or deleting recovery files.

## OTA fails

- Confirm Wi-Fi and stop every active capture, output, BLE session, audio recording, and background network operation.
- Manifest errors mean the release is missing, oversized, malformed, wrong channel/hardware/version/predecessor, or outside the trusted tagged URL path.
- Image errors mean Content-Length, downloaded byte count, embedded application version, or SHA-256 did not match. The updater aborts before selecting that boot partition.
- A candidate that boots but fails NVS/internal-storage/event-loop health remains unvalidated and should roll back. Preserve both boots' logs.

## External tool behaves unexpectedly

Disconnect the target first. Re-read [Pin and interface safety](pin-safety.md), verify 3.3 V logic and common ground, and confirm the correct app owns the pins. Use a meter/logic analyzer before connecting a load. Stop/Home must release pins; a backlight change during Servo/PWM or a driven shared pin after exit is a release-blocking defect.

If the problem remains reproducible, file a bug using the details requested in [CONTRIBUTING](../CONTRIBUTING.md). Do not attach passwords, tokens, raw NVS dumps, private MQTT payloads, or nearby BLE identities without redaction.
