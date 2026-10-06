# SD-card data formats

Tab5 OS keeps user-owned documents and logs on the removable microSD card. Built-in writers use FAT 8.3-safe names so they work with the firmware's conservative FatFs configuration.

## Paths

| Data | Path | Publication rule |
| --- | --- | --- |
| Note | `/DOCS/NOTE.TXT` | Written through `NOTE.TMP`; the previous complete note is retained as `NOTE.BAK`. |
| Ride samples | `/RIDES/YYMMDD/HHMMSSNN.CSV` | Recorded as `.TMP` and renamed to `.CSV` only after flush, media sync, and close succeed. `NN` is `00`-`99` for same-second collisions. |
| Ride index | `/RIDES/SUMMARY.CSV` | Rebuilt through `SUMMARY.TMP`; the previous index is `SUMMARY.BAK`. The individual ride CSV is authoritative. |
| Heart rate | `/HEALTH/HR.CSV` | Append-only rows; an incomplete final row is discarded before later data is read or appended. |
| I2C watch | `/I2C/YYMMDD/HHMMSSNN.CSV` | Logged at 1 Hz as `.TMP`, synced every ten seconds, and renamed only when capture stops successfully. |
| Scope chart | `/SCOPE/YYMMDD/HHMMSSNN.CSV` | The visible 300 calibrated points are written to `.TMP`, synced, closed, and renamed before success is reported. |
| UART terminal | `/UART/YYMMDD/UHHMMSSN.CSV` | RX/TX chunks are recorded as hex in `.TMP`, synced every ten seconds, and renamed only when logging stops successfully. |
| RS-485 terminal | `/RS485/YYMMDD/RHHMMSSN.CSV` | Uses the same durable RX/TX hex format through the onboard transceiver. |
| HTTP metadata | `/HTTP/HTTPLOG.CSV` | Opt-in append-only rows; an incomplete final row is discarded before the next append. Headers, request/response bodies, query strings, and fragments are never stored. |
| MQTT metadata | `/MQTT/MQTTLOG.CSV` | Opt-in append-only rows; an incomplete final row is discarded before the next append. Credentials and payloads are never stored in this file. |
| BLE evidence | `/BLE/YYMMDD/BHHMMSSN.CSV` | An explicit snapshot is written through `.TMP`, synced, closed, and renamed. It includes the bounded advertisements, discovered characteristics, and most recent read/notification value. |
| Ebooks | `/BOOKS/*.TXT` | User-supplied or downloaded text files. |

Files ending in `.TMP` are unpublished: they may be incomplete after power loss, or fully synced but retained because publication failed. They must not be reported as finished captures without validation. A `.BAK` file is the previous complete generation and can be used for recovery when its corresponding final file is absent.

Files opens published Scope and I2C `.CSV` captures as graphs when their headers match the formats below. The viewer shows up to the first 2,048 well-formed rows, leaves the file unchanged, and shows failed I2C reads as gaps. Other files retain the text preview.

Files also opens published UART and RS-485 `.CSV` logs as a timestamped RX/TX timeline. The viewer loads up to the first 512 well-formed rows, shows eight rows per page, and lets you filter by direction or switch between hex and escaped ASCII without changing the file. Unpublished `.TMP` files retain the text preview.

Choose a record from the current page and tap **COPY ROW** to put its original 1-128 bytes on the shared RAM clipboard. The selector and timeline use the same record numbers: positions among loaded valid rows, not physical CSV line numbers. Page/filter changes select the first matching record on the new page; changing Hex/ASCII preserves the selection. Rows of 129-256 bytes remain viewable but cannot be partially copied. Empty filters and failed copies preserve the clipboard. Record bytes can then be pasted into Byte Lab or RTU Frames; a logged RX/TX chunk may split or combine protocol frames, so a record is not a guarantee of a complete message. Copy changes neither the CSV nor the displayed byte values.

## CSV conventions

- UTF-8/ASCII text, comma separator, decimal point `.` and LF line endings.
- `unix_time` and `start_unix` are whole seconds since the Unix epoch in UTC.
- A separate human-readable timestamp is local device time when present.
- Units are encoded in headers: `_s`, `_km`, `_kmh`, `_kj`, `_w`, `_rpm`, and `_bpm` or `bpm`.
- Unknown numeric sensor values are written as `0` only where the existing format has a corresponding availability flag in memory; consumers should not infer laboratory accuracy.

Ride sample header:

```text
unix_time,elapsed_s,power_w,cadence_rpm,speed_kmh,heart_rate,resistance,distance_km,work_kj
```

Ride summary header:

```text
start_unix,duration_s,distance_km,work_kj,avg_power_w,max_power_w,avg_hr,max_hr
```

Heart-rate header:

```text
unix_time,local_time,bpm
```

I2C watch header:

```text
unix_time,address,register,value,status,speed_khz
```

I2C address, register, and successful values use `0xNN` notation. A failed transaction leaves `value` empty and records the ESP-IDF error name in `status`.

Watch/CSV always records one byte per 1 Hz sample, independently of the inspector's 1-32-byte one-shot setting. READ ONCE stops Watch and finishes the active CSV before its transaction. Multi-byte saved reads stay in RAM and may be copied into Byte Lab; they do not change this CSV schema.

Scope chart header:

```text
unix_time,elapsed_us,gpio,millivolts,sample_rate_hz,offset_mv,scale_permille
```

`millivolts` contains ESP-IDF ADC calibration followed by the selected per-input scale and offset, clamped to 0-3300 mV. `scale_permille` uses 1000 for 100.0%. A capture is the visible triggered chart, not an uninterrupted background recording, and remains an uncalibrated measurement until checked against a known reference.

UART terminal header:

```text
unix_time,direction,data_hex
```

`direction` is `RX` or `TX`. `data_hex` is a space-separated sequence of two-digit bytes, independent of the on-screen ASCII/hex view. UART and RS-485 use the same header.

HTTP metadata header:

```text
unix_time,method,url,status,response_bytes,duration_ms,outcome
```

`url` retains the scheme, host, port, and path but removes the query string and fragment. Embedded URL credentials are rejected before a request starts. `outcome` is `complete`, `preview_truncated`, `incomplete_response`, `cancelled`, `request_deadline`, `invalid_response_headers`, or the ESP-IDF transport error name; a non-2xx HTTP response is still a completed HTTP exchange and its status remains authoritative. `incomplete_response` means the client received fewer bytes than promised, encountered invalid/unfinished chunks or an unfinished trailer section, or timed out before a close-delimited body reached a normal EOF, even if the SDK's request function returned success. `cancelled` and `request_deadline` describe interrupted exchanges, not undoing a request at the peer. `invalid_response_headers` covers malformed headers/trailers, sections exceeding 8 KiB and unsupported protocol upgrades. Only the final response status is logged; informational and trailer fields are discarded. These are additive outcome values; the CSV columns are unchanged. The byte count and any preview describe only the body received.

`duration_ms` ends after network/connection cleanup, before writing metadata. The app repairs an incomplete final row before appending and reports success only after flush, durable sync and close succeed. A storage failure does not alter the exchange's `outcome`; it appears alongside the network result and disables logging until the next explicit opt-in on an available card. Bytes can remain after a failed sync or close, so failure means persistence was not confirmed. Previous complete rows survive partial-row repair.

MQTT metadata header:

```text
unix_time,direction,topic,qos,retained,payload_bytes,outcome
```

`direction` is `TX` or `RX`; `retained` is `0` or `1`. A TX row is recorded after ESP-MQTT accepts the publish for queuing, while an RX row records a delivered message or capped preview. `outcome` is `queued`, `received`, or `preview_truncated`. Topic names are evidence and may be sensitive, so logging is disabled again whenever the app is opened.

An RX row requires the complete declared payload, including contiguous SDK fragments. It retains the first event's timestamp, topic, QoS and retain flag. Invalid, interrupted or unfinished assembly and duplicate continuation events produce no row. Payload contents remain excluded from metadata; `payload_bytes` counts the complete payload, while the on-screen ASCII view is capped at 512 bytes.

Received `topic` values are bounded to 127 printable ASCII bytes. A shortened topic or any replacement of non-printable/non-ASCII bytes is labeled as a topic preview in the UI and sets `preview_truncated` in the CSV. That outcome therefore means the payload preview exceeded 512 bytes **or** the topic view changed; the payload may still be fully received. A changed topic is not the full original broker identifier. Control/line-break topic bytes become `.` so rows remain single lines; ordinary printable quotes are escaped by CSV quoting. The header and outcome names are unchanged.

Publish captures its original topic, payload size, QoS and retained state for the worker; later form edits do not change the queued message or row. A successful QoS 0 enqueue has message ID zero and still produces a `queued` TX row. Its timestamp is taken after the SDK accepts the publish, including when an action finishes after Home. Failed, overlong or undispatched cancelled actions produce no TX row.

The log choice is sampled when the receive message completes or the SDK accepts a publish. Accepted metadata is queued independently of the UI preview and written by the worker, even if logging is switched off afterward. Its queue holds eight pending entries plus one in-flight write. Overflow discards older pending rows and reports the omitted count in the UI and at shutdown; UI-ring drops alone do not imply missing metadata.

Disconnect/Home cleanup retains MQTT's busy ownership through pending metadata writes, file close, buffer release and storage-error reporting. Active and shutdown writes stop at the first failure, discard remaining queued metadata and report the captured error callback once for that session. Flush/sync/close errors mean persistence was not confirmed; bytes may already exist. Logging can be enabled again after a new connection starts. The CSV header and outcome values are unchanged. See [MQTT lifecycle](mqtt-console.md).

MQTT PASTE BYTES prepares a separate 0-128-byte RAM draft, including empty payloads, for an explicit Publish. The selected Text/Bytes mode and prepared bytes survive Home until changed or restart; later clipboard changes do not alter the draft or an accepted action. Paste/mode changes record no SD row, and bytes are excluded from the unchanged version-1 connection profile. A byte publish records only its original size/topic/QoS/retain with the existing `queued` outcome.

MQTT COPY RX copies a complete 0-128-byte receive into the RAM byte clipboard for Byte Lab. It uses raw bytes rather than the ASCII preview and records no SD row. A newer oversized receive disables copy without changing the clipboard. Home retains the saved receive in RAM; a new session or Clear removes it. Clear preserves accepted metadata, and restart clears both receive and clipboard. The metadata header and outcomes are unchanged.

MQTT Connect and Save Profile validate broker URI syntax and the 255-byte endpoint limit before SDK/NVS calls. Username/password each have a 64-byte UTF-8 limit; accepted bytes are copied exactly and oversized values are rejected with their first excess character retained. The version-1 blob remains 515 bytes: version, URI, username, password and topic. Loading checks the reported length both before and after reading, then validates its version, terminated fields, normalized secure URI and topic filter. Existing invalid profiles remain present for explicit deletion or replacement by a valid save.

An explicitly saved MQTT TLS/WSS profile is a versioned NVS setting, not an SD file or export. It contains the broker URI, username, password, and topic. Cleartext profiles cannot be saved. Standard developer builds do not encrypt NVS, so use a disposable or independently revocable broker credential; deleting the profile requires two taps within five seconds. A clean flash erase removes the profile.

The setting uses only namespace `mqtt`, key `profile`. Wrong-type/read errors leave Delete available when the key's presence is uncertain; confirmed absence disables it. Set/erase failures report that saving/deleting was not confirmed because storage may already have changed. Re-entry checks the readable value, and a fresh two-tap deletion can safely finish removing an already absent key. DELETE PROFILE performs logical key deletion; physical erasure of older credential bytes is unverified. Native checks use a retained RAM model; actual NVS reboot and flash-fault recovery remain device gates.

BLE evidence header:

```text
unix_time,event,address,name,rssi,service_uuid,characteristic_uuid,handle,properties,value_hex
```

`event` is `advertisement`, `characteristic`, `read`, `notification`, or `indication`. Properties use `R` for read, `W` for write with response, `w` for write without response, `N` for notify, and `I` for indicate. The snapshot is deliberately bounded to eight advertisers, 16 services, 32 characteristics, and 64 displayed value bytes. A trailing ` ...` marks a truncated value; `[unavailable]` means receive-buffer copying failed and no bytes are exported. These markers are not hex bytes. Earlier firmware labeled indications as `notification`. It includes nearby device names and addresses, so saving is explicit and the UI discloses this privacy tradeoff.

These formats are still pre-1.0. Add fields at the end when possible, never silently change a unit, and document any incompatible migration in release notes.
