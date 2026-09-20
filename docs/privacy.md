# Privacy

Tab5 OS has no built-in analytics, advertising identifier, crash-upload service, or background cloud account. Network and radio features are user-visible, but some tools necessarily disclose data to the service or device the user selects.

## Data stored on the tablet

NVS can contain saved Wi-Fi credentials, display/Scope/alarm/weather settings, relay URL and revocable device token, OTA status, and an explicitly saved MQTT TLS/WSS profile. Developer builds do not encrypt NVS. A clean factory image or `erase-flash` removes these values; app-only, ordinary full-source, and OTA installs preserve them.

The internal SPIFFS partition contains built-in state. User documents, captures, and evidence normally live on the removable microSD card. [Data formats](data-formats.md) lists exact paths and fields. Removing or factory-erasing internal flash does not erase the card.

## Network disclosures

- Weather sends the configured location query or coordinates to Open-Meteo geocoding/forecast services.
- AI Chat and transcription send prompts, prior response IDs, or recorded WAV audio plus a device token to the configured HTTPS relay. The relay sends request content to OpenAI and holds the upstream API key.
- Ebooks downloads three public-domain texts from Project Gutenberg only when requested/needed.
- OTA requests the stable manifest and firmware assets from this project's GitHub releases.
- Browser, HTTP Console, MQTT Console, ping, DNS, and mDNS contact user-selected Internet or LAN endpoints. Plain HTTP/MQTT is visibly gated because intermediaries can read it.

Review the privacy and retention policy of each endpoint before sending personal, confidential, health, or production data. Tab5 OS cannot control a remote server's logs.

## Radio and evidence

BLE product profiles and generic scanning are off until explicitly enabled. A BLE evidence export can include nearby device names and addresses, service/characteristic identifiers, and bounded values. MQTT metadata can include topic names. HTTP metadata includes a query-free URL. Logging is opt-in per app entry and excludes configured credentials, HTTP bodies/headers/content, MQTT payloads, and passwords as documented.

Health and cycling tools can write heart rate, activity, device identity, power, and cadence to SD. Treat those files as sensitive. Review before sharing and securely erase or physically control the card when no longer needed.

## USB access

USB remote desktop is a local physical-access interface: a connected host can request framebuffer images and inject pointer events. It is not a network service and has no account authentication. Disconnect USB or use a trusted host when the visible screen or controls are sensitive.

## Secrets and reports

Never commit `main/chat_secrets.h`, relay `.dev.vars`, MQTT passwords, Wi-Fi credentials, raw NVS images, or private captures. Use independently revocable relay/broker credentials. Report a suspected secret exposure or security flaw through the confidential process in [SECURITY](../SECURITY.md), not a public issue.
