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

`url` retains the scheme, host, port, and path but removes the query string and fragment. Embedded URL credentials are rejected before a request starts. `outcome` is `complete`, `preview_truncated`, or the ESP-IDF transport error name; a non-2xx HTTP response is still a completed HTTP exchange and its status remains authoritative.

MQTT metadata header:

```text
unix_time,direction,topic,qos,retained,payload_bytes,outcome
```

`direction` is `TX` or `RX`; `retained` is `0` or `1`. A TX row is recorded after ESP-MQTT accepts the publish for queuing, while an RX row records a delivered message or capped preview. `outcome` is `queued`, `received`, or `preview_truncated`. Topic names are evidence and may be sensitive, so logging is disabled again whenever the app is opened.

An explicitly saved MQTT TLS/WSS profile is a versioned NVS setting, not an SD file or export. It contains the broker URI, username, password, and topic. Cleartext profiles cannot be saved. Standard developer builds do not encrypt NVS, so use a disposable or independently revocable broker credential; deleting the profile requires two taps within five seconds. A clean flash erase removes the profile.

BLE evidence header:

```text
unix_time,event,address,name,rssi,service_uuid,characteristic_uuid,handle,properties,value_hex
```

`event` is `advertisement`, `characteristic`, `read`, or `notification`. Properties use `R` for read, `W` for write with response, `w` for write without response, `N` for notify, and `I` for indicate. The snapshot is deliberately bounded to eight advertisers, 16 services, 32 characteristics, and 64 displayed value bytes. It includes nearby device names and addresses, so saving is explicit and the UI discloses this privacy tradeoff.

These formats are still pre-1.0. Add fields at the end when possible, never silently change a unit, and document any incompatible migration in release notes.
