# Modbus RTU Frames

**RTU Frames** is an offline request builder and reply inspector. It builds one eight-byte read request for function 01 (coils), 02 (discrete inputs), 03 (holding registers), or 04 (input registers), with a low-byte-first CRC-16/MODBUS. It opens no serial port, sends no traffic, and writes no files. Request fields, reply text, and view choices survive Home in RAM and reset on reboot.

## Prepare a read

1. Choose a read function, unit **1-247**, a **zero-based address 0-65535**, and count **1-16**. The last address must stay at or below 65535. Broadcast unit 0, reserved units, writes, and automatic polling are unsupported.
2. Tap **COPY REQUEST**. The preview shows the exact eight bytes and copies them to the shared byte clipboard. Invalid fields leave the previous clipboard untouched.
3. Open [Serial](serial-terminal.md), select UART or onboard RS-485 and the device's baud/parity/stop settings, and tap **PASTE**. Paste selects Hex with no extra line ending. Review the bytes, tap **START**, then **CAPTURE RX** before **SEND**. After the expected reply arrives, tap **FREEZE RX** and **COPY RX**, then return here to **PASTE REPLY** and **DECODE**.

For example, unit 1, function 03, address 0, count 10 produces `01 03 00 00 00 0A C5 CD`. A device manual's holding-register label `40001` commonly maps to protocol address `0`; follow that device's addressing convention rather than entering the label automatically.

The frame tool does not implement a timed RTU transaction or detect frames in the Serial receive stream. Match serial framing to the device; the serial specification uses even parity with one stop bit by default, or two stop bits without parity. RTU also requires inter-character and inter-frame timing that this offline app cannot assess. Use the tablet as the only requester on a bus you manage. See the [Modbus serial specification, sections 2.1 and 2.5.1](https://www.modbus.org/file/secure/modbusoverserial.pdf) and [pin safety](pin-safety.md).

## Inspect a reply

Enter the entire captured reply as Hex, including the unit, function and final two CRC bytes. Complete byte pairs may be contiguous or separated by ASCII whitespace. **PASTE REPLY** accepts 1-37 bytes from the shared clipboard without decoding or changing request fields. Empty/absent or oversized copies preserve the current draft. Use Serial's frozen **COPY RX** window or prepare known bytes in Byte Lab's Hex mode. Serial collects raw bytes across reads without detecting frames: verify that the copied window includes exactly one whole reply and excludes any request echo.

Saved UART/RS-485 logs in Files also offer **COPY ROW** for a selected record of at most 128 bytes. A whole RTU read reply fitting the 37-byte inspector limit can be pasted here. Rows are saved chunks and are not combined automatically: a split reply needs all of its bytes, and a row containing echo or multiple frames needs inspection before use.

Tap **DECODE** to validate the reply against the current request. Normal replies must have the expected unit, function, count, exact length and CRC. Unused high bits in a final coil/discrete-input byte must be zero. Exception replies show the device's exception code and no old values. Corrupt, incomplete, oversized, concatenated, or mismatched replies produce an error. A bad reply does not stop you from copying a valid request.

The inspector shows raw unsigned/hex/signed 16-bit words, or paired unsigned/signed 32-bit and Float32 values in ABCD, CDAB, BADC or DCBA byte order. A/B are the first register's bytes; C/D are the next. An odd final register remains visible as unpaired. Coil/discrete-input replies always show bits. Longer results scroll inside the result box.

Read replies do **not** echo their starting address or carry a transaction ID. Displayed addresses come from your request fields; a matching CRC/unit/function/count cannot establish when, where, or in response to which request the bytes were captured. Editing any field clears the decoded values. **DECODE** interprets the bytes again using the edited request, and changing a value view never sends traffic.

**EXAMPLE** replaces the request and reply drafts with an offline function-03 response containing Float32 `1.5` in ABCD order: `01 03 04 3F C0 00 00 F6 1B`. It does not change the shared clipboard. **CLEAR** clears only the reply draft. Tap a field for numeric/Hex keys; Done or Cancel hides the keyboard without copying or sending.

## Verification and remaining work

The pure `modbus_rtu_test` checks a known wire request and exception, an independent CRC calculation, every supported unit/function, address boundaries, all read quantities, every single-bit mutation in generated replies, truncation, trailing bytes, unused bits, and output clearing on failure. TCP and RTU share the read PDU decoder, with direct cross-envelope comparisons. Existing Modbus TCP and Byte Lab pure tests also pass after that refactor.

`tools/test_modbus_rtu_ui.ps1 -Snapshots` runs the real app and LVGL against the already-built Debug host library. It checks exact clipboard bytes, malformed fields, CRC/exception responses, maximum replies, bit views, result invalidation, keyboards, Unicode/excess-length retention, popup teardown, and eight Home/reopen cycles with unchanged free heap and live allocations. It compiles sequentially, starts no background workers, and refuses to rebuild a missing LVGL library. PPM snapshots are saved under `build/modbus-rtu-ui/`.

The ESP-IDF 5.4.2 firmware compiled and linked with one build job on 2026-09-29. On ST7121, the example decoded Float32 `1.5`, and COPY REQUEST -> Serial PASTE preserved `01 03 00 00 00 02 C4 0B` while the port remained stopped. This was an offline handoff, with no serial transmission. The remaining panel, input-boundary and wired-fixture checks are in the [hardware checklist](hardware-smoke-checklist.md). An automatic RTU transport remains separate work: it needs exclusive UART ownership, precise frame/error handling, bounded cancellation and turnaround checks on physical hardware.
