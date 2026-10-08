# OTA manifest contract

Stable OTA checks `tab5_os.json` from the latest non-prerelease GitHub release. The bounded schema is:

```json
{
  "schema": 1,
  "version": "v0.6.0-01234567",
  "hardware": "m5stack-tab5",
  "size": 2087008,
  "sha256": "64 lowercase hexadecimal characters",
  "url": "https://github.com/DevanMetz/Tab5OS/releases/download/v0.6.0/tab5_os.bin",
  "channel": "stable",
  "minimum_predecessor": "v0.5.1"
}
```

The updater accepts only schema 1, `m5stack-tab5`, the stable channel, a newer semantic version, a compatible minimum predecessor, an image no larger than the 6 MiB OTA slot, and the repository's immutable tagged-release URL prefix. It caps the JSON response at 1536 bytes and validates the entire received byte count. Schema 1 requires exactly the eight fields above; duplicate or extra members, arrays and nested objects are rejected. Nesting is rejected before cJSON recursion. Raw NUL/control bytes, escaped U+0000 and non-JSON whitespace cannot truncate or disguise values. Ordinary JSON whitespace, escaped ASCII and literal backslashes remain supported.

Before changing the boot partition, Tab5 OS verifies the server's image length when available, the exact downloaded byte count, SHA-256 of the complete published `.bin`, and the version embedded in the ESP-IDF application description. Any mismatch aborts the OTA handle and leaves the running boot partition selected. A verified image still remains pending until the existing 30-second display/storage/NVS health window passes.

Tag builds publish `tab5_os.bin`, `tab5_os.json`, and `SHA256SUMS` from the same CI build. Tags containing `-` are GitHub prereleases with channel `beta`, so the stable updater does not consume them through GitHub's latest-release endpoint.

## Native validation

Run `./tools/test_ota_manifest.ps1` with native Clang and the pinned ESP-IDF 5.4.2 sources; use `-Compiler` and `-IdfPath` when needed. The runner compiles the actual `ota_manifest.c` and SDK cJSON with strict warnings. Its 372 checks exercise every valid-response fragment size, the exact 1536-byte cap, redirects, request/init errors, malformed or ambiguous metadata, 32-bit size bounds, cJSON allocation failures/recovery and version/slot/URL gates. Invalid parsed results are cleared, and every request releases its client and JSON allocations. Four private mutations of the bounds, member count, size and output cleanup are rejected.

The native CI job runs these checks using cJSON source/header/license files in its pinned artifact. Controlled HTTP events provide the response; all image/hash APIs in this fixture fail closed if called. These checks do not exercise image downloads, boot selection, rollback, TLS or physical hardware. Those validations remain in the [hardware checklist](hardware-smoke-checklist.md).

## Trust boundary

This contract prevents truncation, substitution after manifest retrieval, accidental cross-hardware installs, and unstructured `latest` downloads. It does not yet provide an independent release signature: GitHub account/repository control and Web PKI remain trusted. Phase 4 still requires signed manifests or images with an embedded public verification key before v1.0. Secure boot, flash encryption, and irreversible eFuses remain a separate opt-in hardened-device profile.
