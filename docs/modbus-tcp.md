# Modbus TCP inspector

Open **Modbus TCP** in the launcher after connecting Wi-Fi in Settings. This app reads a single device at a time and provides a TCP connection test. It has no write functions, automatic polling, or file logging.

The **Network** launcher tile opens the existing link details, DNS, ping, and mDNS tools when you need to diagnose reachability before reading a device. These tools remain available through Settings as well.

## Read a device

1. Enter the device's full decimal IPv4 address and TCP port (normally 502). Addresses require four octets from 0 to 255, without leading zeros or whitespace. Hostnames, shortened/alternate-base addresses, and IPv6 are not supported in this version.
2. Set the unit identifier expected by the device or gateway, from 0 to 255. The initial value is 1.
3. Select coils (function 01), discrete inputs (02), holding registers (03), or input registers (04).
4. Enter a **zero-based** start address and a count of 1 to 16. For a manual using conventional holding-register reference 40001, use function 03 and address 0; check the device's own addressing convention. The final address must not exceed 65535.
5. Tap **Read**, then confirm the unchanged request within five seconds. Modbus TCP here uses an unencrypted connection. Changing a field or leaving the app clears confirmation.

Numeric fields accept up to five decimal digits within their stated ranges. Signs, exponent notation, suffixes, pasted line breaks, and excess length are retained as invalid drafts, including after Home/re-entry. For example, `-1` cannot become `1`, and `1e2` cannot become `12`. Invalid input starts no worker or connection.

The result identifies the request it belongs to. The default register view includes hexadecimal, unsigned decimal, and signed 16-bit values; coils and discrete inputs show individual bit values. A protocol exception is displayed as an error instead of decoded data. Returned transaction, protocol, unit, function, length, and byte count are checked before values are displayed.

**Test TCP** connects to the selected address and port without sending a Modbus request. Success proves that the TCP connection opened, not that the endpoint implements Modbus. It can also check an explicitly selected port on another network device.

Each exchange has a five-second deadline covering connection, transmission, and response. **Stop** or Home requests cancellation; the worker closes its socket before becoming idle. Socket cleanup can add a short scheduler delay beyond the exchange deadline. Firmware installation is blocked while it is still active. There is no retry or reconnect loop. Form values remain in RAM until restart.

The framing and function handling follow the Modbus Organization's [application protocol specification](https://www.modbus.org/file/secure/modbusprotocolspecification.pdf) and [TCP/IP implementation guide](https://www.modbus.org/file/secure/messagingimplementationguide.pdf).

## Interpret register pairs

After a successful function 03/04 read, use **Saved reply: value view** to select unsigned 32-bit, signed 32-bit, or IEEE 754 Float32. Each value uses two adjacent registers, starting at the original read address. A final register without a partner remains visible as **unpaired**; no zero or extra read is supplied. Every pair shows its original raw words and the resulting 32-bit pattern.

Choose the byte order documented by the device. If `A B` are the first register's high/low bytes and `C D` are the second register's high/low bytes, the selector orders them from most to least significant in the interpreted value:

| Order | Conversion | Received words that decode as Float32 123456 |
| --- | --- | --- |
| ABCD | Preserve register and byte order | `47F1 2000` |
| CDAB | Swap the two registers | `2000 47F1` |
| BADC | Swap bytes within each register | `F147 0020` |
| DCBA | Swap both registers and their bytes | `0020 F147` |

These examples match the libmodbus references for [ABCD](https://libmodbus.org/reference/modbus_get_float_abcd/), [CDAB](https://libmodbus.org/reference/modbus_get_float_cdab/), [BADC](https://libmodbus.org/reference/modbus_get_float_badc/), and [DCBA](https://libmodbus.org/reference/modbus_get_float_dcba/). Modbus's 16-bit register representation alone does not establish the device's multi-register type, pairing, units, or scale; use its register map. The app applies no scale or offset.

Float32 uses nine significant digits and explicitly shows signed zero, infinity, and NaN. Switching views or byte order reinterprets the saved reply without opening a connection or sending a request. Editing the next request cannot change the earlier reply's address labels or pairing. The selected view/order and reply survive Home/re-entry in RAM until restart. View controls are disabled while reading and for bit replies, errors, or TCP tests. Dismiss the keyboard to see the result panel.

## Reproducible test device

The repository includes a Python fixture with deterministic values and no hardware connection:

```powershell
python tools/modbus_test_server.py --self-test
python tools/modbus_test_server.py --bind <PC-LAN-IPv4> --port 1502
```

Use the PC's address and port 1502 in Tab5. The PC and tablet must be reachable on the same network, and the PC firewall must allow the selected port. The default bind address is loopback (`127.0.0.1`), which is useful for host checks but is not reachable from the tablet.

- Read two holding registers at address 10: expect 10 / `000A` and 11 / `000B`.
- Read nine coils at address 0: expect alternating 0 and 1, ending in 0.
- Start the fixture with `--fragment 1` to exercise responses arriving one byte at a time.
- Use `--exception 2` to return an illegal-address exception, or `--delay 6` to exercise the tablet's timeout and Stop/Home paths.
- Stop the fixture with Ctrl+C and repeat Test TCP to check the failed-connection path.

This fixture is intended for a local development network. It serves one connection at a time and supports the app's 16-value limit.

## Validation status

Protocol host tests pass for request encoding, all four read functions, range limits, exception responses, mismatched headers, and malformed/truncated frames. The ESP-IDF 5.4.2 firmware build passed. A UI-only host check verified default/keyboard layout, invalid IPv4 input, offline refusal, and 100 open/close cycles without allocator loss.

The actual app source also passed real loopback TCP tests with LVGL, native Windows sockets and threads, and a local Python fixture. Coverage includes all four read functions, one-byte fragments, connect-only probing, exceptions, mismatched/oversized headers, premature EOF, refused connections, slow-stream and silent-peer deadlines, Stop/Home cancellation followed by a successful read, and 100 consecutive reads. Socket/task counts returned to zero; native handle counts stayed stable after socket-provider warmup. These adapters verify application behavior rather than ESP-IDF's scheduler, lwIP internals, or Wi-Fi hardware.

Pure view tests cover all four byte orders, integer limits, Float32 special values, single/odd register counts, maximum addresses, and bounded output. An independent Python reference checks all three 32-bit views for 4,096 register/order cases.

Run `./tools/test_modbus_network.ps1` on Windows to repeat all 17 scenarios, or add `-Modes ui,views` for a focused display run. They also check that invalid IPv4/numeric/Unicode drafts survive editing and Home, and that invalid input, an expired confirmation, or a changed field sends no request. The view scenario verifies actual replies, order changes without additional traffic, original request identity, and unpaired registers; the UI scenario checks 100 lifecycle cycles and writes PPM previews under `build/modbus-network/`. The runner needs Python, the downloaded managed LVGL source, a native C compiler, CMake, and Ninja; it builds the host LVGL library if missing. Its server binds only to loopback and stops after the tests.

The 2026-09-29 ST7121 pass used USB-injected touches and a PC peer over Wi-Fi. It passed the connect-only probe, FC01-04 reads fragmented into single bytes, saved integer/Float32 and word-order views without extra traffic, popup/Home retention, exception `0x06`, the five-second timeout, and Stop/Home cancellation. A stopped PC listener timed out rather than returning an immediate refusal. See the firmware identity and exact scope in the [smoke checklist](hardware-smoke-checklist.md#usb-only-development-pass-2026-09-29). Physical device maps, Wi-Fi loss, memory-pressure faults, task stack high-water and the second panel family remain open.
