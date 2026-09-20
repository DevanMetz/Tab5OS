# Security policy

## Supported versions

Before 1.0, security fixes target the latest stable release and the current `main` development line. Older tags may be useful for recovery but do not receive routine fixes. After 1.0, the project will publish an explicit support window in release notes before retiring a stable line.

## Reporting a vulnerability

Use GitHub's private vulnerability-reporting flow at <https://github.com/DevanMetz/Tab5OS/security/advisories/new>. Include the affected version/commit, threat model, reproduction, impact, and a minimal proposed mitigation if known. If private reporting is unavailable, contact the maintainer through the repository owner's GitHub profile and request a confidential channel; do not publish exploit details or secrets in an issue.

Do not include real Wi-Fi/MQTT/relay credentials, raw NVS dumps, private health/capture data, or nearby BLE identities. Revoke any credential you believe was exposed; deletion from git history or an SD file is not revocation.

## In scope

- OTA manifest/image substitution, downgrade, rollback, signing, or recovery flaws.
- Authentication/token handling in firmware or the relay.
- Network input that causes memory corruption, resource exhaustion, secret disclosure, or unsafe output.
- USB remote-control abuse beyond the documented physical-access boundary.
- BLE/Wi-Fi behavior that connects, scans, logs, or exposes identities without the documented opt-in.
- Pin, bus, external-power, PWM, storage, or lifecycle defects that can damage hardware or destroy user data.

## Current security boundary

TLS certificate verification, exact OTA image hashing, bounded parsers, explicit radio/network controls, rollback, and ignored local secret files are implemented. Independent release signatures are not yet implemented, so GitHub repository control and Web PKI remain trusted. Developer builds do not enable NVS encryption, flash encryption, secure boot, anti-rollback eFuses, or locked debug; those irreversible controls belong to a separately tested hardened-device profile.

Public documentation of these limitations is intentional and is not itself a vulnerability. A bypass, unsafe default, secret leak, or behavior outside this boundary is reportable.
