---
name: Bug report
about: Report a reproducible Tab5 OS defect
title: "[Bug] "
labels: bug
---

## Version and hardware

- Firmware version/commit:
- Panel/touch family shown in System:
- Reset reason:
- Power source:
- microSD model/format/presence:
- External wiring, level shifting, and instruments:

## Reproduction

1.
2.
3.

Expected result:

Actual result:

## Evidence

Paste the relevant boot/runtime log and System free/minimum heap, PSRAM, NVS/internal/SD, Wi-Fi, and OTA states. Attach screenshots or measurements when useful.

- [ ] I removed passwords, tokens, raw NVS, private payloads, health data, and unrelated BLE identities.
- [ ] I disconnected external hardware or documented its voltage/current limits and common ground.
- [ ] I stated whether this is a visual/USB observation or an instrumented electrical/timing result.
- [ ] I checked the applicable item in `docs/hardware-smoke-checklist.md`.

For a vulnerability or exposed secret, stop and use `SECURITY.md` instead.
