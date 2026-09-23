# Install and recovery

Tab5 OS uses ESP-IDF 5.4.2 and an ESP32-P4 target. Tagged releases contain an app-only `tab5_os.bin`, a destructive 16 MiB `tab5_os_factory.bin`, the OTA manifest, checksums, licenses, notices, and the pinned dependency inventory.

## Before flashing

- Disconnect external hardware from G53/G54 and all M5-Bus pins.
- Remove the microSD card if its contents are important. Flash commands do not target it, but removal avoids confusion during recovery.
- Install ESP-IDF 5.4.2 and open its PowerShell environment.
- Put the Tab5 in download mode by holding Reset until the green LED flashes rapidly.

Replace `COM7` below with the port shown by Windows Device Manager.

## Check the installed partition table

Read the table before an app-only update if the tablet's flash history is unknown. The read is non-destructive:

```powershell
python -m esptool --chip esp32p4 -p COM7 read-flash 0x8000 0x1000 .\partition-table.bin
python (Join-Path $env:IDF_PATH 'components/partition_table/gen_esp32part.py') .\partition-table.bin
```

Compare the result with the [current flash layout](compatibility.md#hardware-and-flash-layout). Some earlier tablets have a `factory` app at `0x20000`, 4 MiB OTA slots at `0x420000` and `0x820000`, and `storage` at `0xc40000`. The current layout has no factory app, uses 6 MiB OTA slots, and places `storage` at `0xc20000`.

Do not use the app-only wrapper to migrate a legacy layout: writing `0x20000` updates its factory slot, which may not be the selected boot slot. A complete source flash changes the partition table but does not relocate old internal SPIFFS files. If those files matter, retain the old layout until you have a verified backup and migration plan. The factory image installs the current layout by erasing the entire internal flash.

## App-only update

Use this only when the installed device already has Tab5 OS's current partition table. It preserves NVS settings and internal storage.

```powershell
.\tools\flash_idf.ps1 -Port COM7
```

The wrapper builds the project and writes `build\tab5_os.bin` at the current app offset (`0x20000`). Do not use an app-only flash after changing the bootloader, partition table, flash settings, or OTA layout.

## Complete source flash

This writes the bootloader, partition table, initial OTA metadata, and application. With the same partition layout, it does not deliberately erase NVS or internal storage. A changed layout can make prior internal data inaccessible; check the installed table first.

```powershell
idf.py set-target esp32p4
idf.py build
idf.py -p COM7 flash monitor
```

The repository wrapper performs the same complete build and flash:

```powershell
.\tools\flash_idf.ps1 -Port COM7 -Full
```

Use `Ctrl+]` to exit the serial monitor.

## Released factory image

Use `tab5_os_factory.bin` only for a clean installation or recovery. It replaces the complete 16 MiB flash, including NVS, both OTA slots, and internal SPIFFS. It permanently erases saved Wi-Fi/MQTT/relay credentials, settings, alarms, OTA history, and internal files. It does not target the removable microSD card, but remove that card before recovery.

Download `tab5_os_factory.bin` and `SHA256SUMS` from the same release. Verify the checksum, enter download mode, and write the merged image at offset zero from an ESP-IDF 5.4.2 PowerShell environment:

```powershell
Get-FileHash .\tab5_os_factory.bin -Algorithm SHA256
Get-Content .\SHA256SUMS
python -m esptool --chip esp32p4 -p COM7 -b 460800 --before default_reset --after hard_reset `
    write_flash 0x0 .\tab5_os_factory.bin
```

The computed factory hash must exactly match its `SHA256SUMS` row. Do not interrupt the write. Re-enter Wi-Fi and other credentials manually afterward; do not restore an unknown or incompatible raw NVS image.

## Clean factory recovery

Use this only when normal flashing still leaves a boot loop, incompatible settings, or a damaged partition. `erase-flash` permanently removes both OTA slots, Wi-Fi credentials, saved MQTT broker credentials, alarms, relay settings, and internal SPIFFS data. It does not erase the removable microSD card.

```powershell
idf.py set-target esp32p4
idf.py build
idf.py -p COM7 erase-flash
idf.py -p COM7 flash monitor
```

Do not interrupt power between the erase and completed flash. If the port disappears, return the Tab5 to download mode and repeat the `flash` command; another erase is unnecessary.

## Verify recovery

1. Confirm the launcher appears without a reset loop and touch works at the corners.
2. Open System and record the firmware version/commit, reset reason, panel type, NVS state, and storage state.
3. Reconnect Wi-Fi manually after a clean erase.
4. Run the applicable items in the [hardware smoke checklist](hardware-smoke-checklist.md), especially both display variants and OTA rollback.

If the device still cannot boot, save the complete serial log and the output of `idf.py flash`; do not repeatedly erase a device that may have a power or hardware fault.

See [Storage and compatibility contract](compatibility.md) before choosing an app-only, source-full, or factory install.
