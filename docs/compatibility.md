# Storage and compatibility contract

This is the pre-1.0 compatibility contract. User-visible formats become stable only after their roadmap hardware gates pass; changes before then must still preserve data where practical and be called out in release notes.

## Hardware and flash layout

Tab5 OS targets the 16 MiB M5Stack Tab5 with ESP32-P4 and the onboard ESP32-C6 hosted over the fixed SDIO bus. Both supported display/touch families use the same firmware image: ILI9881C/GT911 and ST7121/ST7123.

| Region | Offset | Size | Compatibility rule |
| --- | ---: | ---: | --- |
| Bootloader | `0x2000` | through `0x7fff` | Update only through a complete source flash or factory image. |
| Partition table | `0x8000` | `0x1000` | App-only installs require this current layout. |
| OTA metadata | `0xf000` | `0x2000` | Managed by ESP-IDF. |
| OTA app 0 | `0x20000` | 6 MiB | Current app-only USB offset. |
| OTA app 1 | `0x620000` | 6 MiB | Alternate rollback slot. |
| Internal SPIFFS `storage` | `0xc20000` | `0x3e0000` | Preserved by app-only and full source flashes when the layout is unchanged. |

An app-only image is compatible only when the installed partition table, flash mode, bootloader expectations, and application offset already match. A 16 MiB factory image contains bootloader, partition table, initial OTA data, and app at those offsets and fills every other byte with erased `0xff`; writing it at offset zero deliberately removes every prior flash setting and internal file.

Some earlier tablets have a factory app and 4 MiB OTA slots, with internal `storage` beginning at `0xc40000`. One ST7121 tablet installed v0.6.0 by OTA on that legacy table, but its older bootloader did not advance the image from `new` to `validated` until the v0.6.0 bootloader region was installed. This observation does not make the layouts interchangeable. Use the [partition-table check and migration guidance](install-recovery.md#check-the-installed-partition-table) before a USB flash; changing the layout does not migrate files or settings automatically.

## NVS and settings

NVS initialization errors never trigger an automatic whole-partition erase. A failed settings load keeps compiled defaults and leaves stored data available for explicit recovery or a factory erase.

Tab5 OS currently owns these namespaces:

- `tab5`: display brightness/timeout, Scope calibration, alarms, relay configuration, weather location/migration marker, and pending/last OTA status.
- `mqtt`: one versioned TLS/WSS broker profile containing URI, username, password, and topic. Invalid or old-size profile blobs are not loaded and remain explicitly deletable without affecting other namespaces.
- ESP-IDF/ESP-Hosted owns Wi-Fi driver state and saved network credentials; application code does not rewrite that private schema.

Unknown keys are preserved. New settings must use a new key or a versioned blob; firmware must validate type, length, version, and range before use. A release that cannot migrate a stored value must ignore that value and preserve it unless the user explicitly deletes or factory-resets it.

Developer builds do not encrypt NVS. Use independently revocable relay and broker credentials. NVS encryption belongs to the separately documented hardened-device profile because its key provisioning and recovery behavior must be tested before use.

## Filesystems and removable data

Internal `storage` is SPIFFS. A provably blank partition may initialize automatically; nonblank invalid data is never silently formatted. Current user captures and documents live on a FAT-formatted microSD card. Tab5 OS does not auto-format or hot-remount a removed card and uses 8.3-safe generated names for compatibility with the current FatFs configuration.

Published paths, CSV headers, units, `.TMP`/`.BAK` rules, and recovery behavior are defined in [Data formats](data-formats.md). App-only updates, OTA, and ordinary complete source flashes preserve the card because it is not a flash partition. Remove important cards during factory recovery to eliminate operator error.

## Version and migration policy

Stable OTA accepts only a newer semantic version whose manifest names `m5stack-tab5`, channel `stable`, the exact image size/SHA-256, and a `minimum_predecessor` no newer than the running firmware. See [OTA manifest contract](ota-manifest.md).

Each stable release note must state whether it changes the partition table, NVS keys/blob versions, internal filesystem behavior, SD paths/headers, or minimum predecessor. Partition changes require a factory or complete source flash; they must never be shipped as an app-only/OTA-compatible change. Removed settings and data formats receive at least one stable release of read/migrate support after 1.0 unless a security issue makes that unsafe.
