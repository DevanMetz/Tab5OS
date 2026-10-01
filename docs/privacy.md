# Privacy

Tab5 OS has no built-in analytics, advertising identifier, crash-upload service, or background cloud account. Network and radio features are user-visible, but some tools necessarily disclose data to the service or device the user selects.

## Data stored on the tablet

NVS can contain saved Wi-Fi credentials, display/Scope/alarm/weather settings, relay URL and revocable device token, OTA status, and an explicitly saved MQTT TLS/WSS profile. Developer builds do not encrypt NVS. A clean factory image or `erase-flash` removes these values; app-only, ordinary full-source, and OTA installs preserve them.

The internal SPIFFS partition contains built-in state. User documents, captures, and evidence normally live on the removable microSD card. [Data formats](data-formats.md) lists exact paths and fields. Removing or factory-erasing internal flash does not erase the card.

The bench apps and saved serial log viewer share an optional 128-byte payload clipboard in RAM. It survives Home but clears on restart or Byte Lab's Clear copy action. Copy/paste changes only app data and does not transmit. UDP replies copied into it retain the received bytes; no clipboard data is written to flash or SD. Clearing the clipboard does not clear separate app drafts, serial history, or the previous UDP exchange. Serial retains its two transmit drafts and eight sent messages in RAM until restart; opt-in serial SD logs continue to contain RX/TX bytes.

I2C Inspector retains the last successful read of 1-32 bytes and its original address/register pointer/speed in RAM through Home. COPY READ exports those bytes to the clipboard. CLEAR READ, a failed read, a confirmed write attempt, or restart clears the saved read; the clipboard remains independent. Watch replaces it with one byte on each successful sample and can refill it after CLEAR READ. Saved blocks are not written to storage; explicitly enabled one-byte I2C CSV capture remains separate. The selected one-shot length also stays only in RAM.

SPI also retains its transmit draft and its last completed transfer's TX/RX bytes (up to 32 each), mode and clock in RAM through Stop/Home. CLEAR TX clears the draft, including any invalid text; CLEAR RESULT clears the saved transfer. A new bus transfer replaces the saved result on success or clears it on failure. Editing or clearing the draft, or clearing the clipboard, leaves the saved result intact. Only COPY RX exports its receive bytes to the shared clipboard. Restart clears both the draft and result; SPI writes no file.

RTU Frames also uses that clipboard to copy read requests and paste captured replies. It retains request fields, reply text and display choices only in RAM. It has no UART/network connection or file writer; transmitting a prepared request requires explicit actions in Serial. Clearing the clipboard does not clear the RTU reply draft.

Serial optionally retains one frozen RX window of at most 128 bytes and its source line settings in RAM. It survives Home until CLEAR RX, the next successful capture start, or reboot. Only COPY RX exports it to the shared clipboard. Clearing the clipboard or visible transcript does not clear this separate window. Overflow and capture read/drain failures clear its bytes; opt-in SD logging remains separate.

The saved serial log viewer can copy one selected record of up to 128 original bytes into that clipboard. This is an explicit local action; it changes no file and sends nothing. The loaded viewer rows are freed on exit, while the independent clipboard copy remains until replaced, cleared or rebooted.

BLE GATT Explorer keeps its latest received value and source handle in RAM. COPY LATEST VALUE explicitly copies a complete payload of at most 64 bytes into the shared clipboard without radio traffic or a file write. Disconnect clears the explorer's value, but a prior clipboard copy remains until replaced, cleared or rebooted. The separate SAVE EVIDENCE action can export the received value, including a labeled truncated preview or unavailable-buffer marker.

## Network disclosures

- Weather sends the configured location query or coordinates to Open-Meteo geocoding/forecast services.
- AI Chat and transcription send prompts, prior response IDs, or recorded WAV audio plus a device token to the configured HTTPS relay. The relay sends request content to OpenAI and holds the upstream API key.
- Ebooks downloads three public-domain texts from Project Gutenberg only when requested/needed.
- OTA requests the stable manifest and firmware assets from this project's GitHub releases.
- Browser, HTTP Console, MQTT Console, ping, DNS, and mDNS contact user-selected Internet or LAN endpoints. Plain HTTP/MQTT is visibly gated because intermediaries can read it.
- Modbus TCP contacts the entered IPv4 address and port only on request. Reads require cleartext confirmation; Test TCP opens and closes a connection without sending application data. Requests/results remain in RAM and are not logged or exported. Electronics, Byte Lab, Subnet Lab, and Resistor Lab perform calculations locally without network access or saved files; Subnet Lab does not contact either entered address.
- NTP Lab sends four unencrypted time queries to the entered IPv4 endpoint after an explicit measurement action. Wake-on-LAN sends a single unencrypted magic packet containing the entered MAC address to the selected IPv4 destination after unchanged confirmation; broadcasts are visible across the local subnet. Both retain form values/results only in RAM, with no automatic operation, logging, or export.
- UDP Console sends the confirmed custom hex/ASCII payload unencrypted to the entered unicast IPv4 address and port. It accepts the first reply from that endpoint, which is not authenticated, and closes within the bounded exchange. Inputs, request bytes, and the reply preview stay only in RAM until restart; there is no profile storage, logging, export, automatic transmission, or idle listener.

Review the privacy and retention policy of each endpoint before sending personal, confidential, health, or production data. Tab5 OS cannot control a remote server's logs.

## Radio and evidence

BLE product profiles and generic scanning are off until explicitly enabled. A BLE evidence export can include nearby device names and addresses, service/characteristic identifiers, and bounded values. MQTT metadata can include topic names. HTTP metadata includes a query-free URL. Logging is opt-in per app entry and excludes configured credentials, HTTP bodies/headers/content, MQTT payloads, and passwords as documented.

Health and cycling tools can write heart rate, activity, device identity, power, and cadence to SD. Treat those files as sensitive. Review before sharing and securely erase or physically control the card when no longer needed.

## USB access

USB remote desktop is a local physical-access interface: a connected host can request framebuffer images and inject pointer events. It is not a network service and has no account authentication. Disconnect USB or use a trusted host when the visible screen or controls are sensitive.

## Secrets and reports

Never commit `main/chat_secrets.h`, relay `.dev.vars`, MQTT passwords, Wi-Fi credentials, raw NVS images, or private captures. Use independently revocable relay/broker credentials. Report a suspected secret exposure or security flaw through the confidential process in [SECURITY](../SECURITY.md), not a public issue.
