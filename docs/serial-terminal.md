# Serial terminal

Open **Serial** from the launcher for UART1 on M5-Bus G47 TX/G48 RX, or select **Link: RS-485** for the onboard J7 A/B interface. Select the link, baud, parity, and stop bits while stopped, then tap **START**. The terminal uses eight data bits and no flow control. Follow the existing [pin and voltage guidance](pin-safety.md); UART is 3.3 V logic, and J7 VIN should remain disconnected when powered over USB.

## Prepare and send

**Transmit format** controls the bytes sent. **VIEW** separately controls how new RX/TX lines appear in the transcript; it does not reinterpret the transmit draft or reformat old transcript lines.

- **Literal ASCII** sends exactly the entered 7-bit characters. Newlines and tabs count as bytes; backslash escapes are not expanded. Non-ASCII input is rejected. Use Hex for arbitrary binary bytes or text encoded using another character set.
- **Append to ASCII** selects None, LF (`0A`), CR (`0D`), or CRLF (`0D 0A`). For example, `AT` with CRLF sends `41 54 0D 0A`. An empty draft with LF sends one byte; an empty draft with None is invalid.
- **Hex byte pairs** requires pairs separated by ASCII whitespace, such as `A5 00 FF`. Contiguous `A500FF`, lone nibbles, `0x` prefixes, and punctuation are rejected. Newlines between complete pairs are allowed; a newline splitting a pair is invalid. Hex always sends without an added suffix, and the suffix control is disabled.

The limit is **256 transmitted bytes, including the selected suffix**. Hex input is limited to 767 characters, enough for 256 spaced byte pairs. The UI retains one excess character for rejection rather than silently shortening an overlong paste. Separate ASCII/Hex drafts and the ASCII suffix survive Home and format changes, including invalid drafts. A restart clears them.

The ready line shows the resulting byte count and suffix. **SEND** transmits once after validating the current draft, even if a display refresh is pending. Invalid input disables the button. A successful full write clears the current draft. A short or failed write keeps the draft; any accepted prefix is shown and logged, and it is not added to send history. Check the target before retrying; a prefix may already have reached it.

Tap the draft to show its keyboard. Hex has a dedicated byte keypad. The transcript is hidden while typing, but receive processing continues. Done/Cancel restores it without sending.

## Clipboard and history

Copy bytes in [Byte Lab](offline-tools.md#move-bytes-between-apps), or copy a complete [UDP reply](udp-console.md#byte-clipboard), then use **PASTE** here. This replaces the Hex draft, selects Hex transmit format, and adds no suffix. It does not start UART/RS-485 or transmit. The ASCII draft and its suffix stay available. An absent or intentionally empty clipboard leaves the serial draft unchanged. The shared clipboard currently holds at most 128 bytes; manually prepared serial messages can still use all 256 bytes.

[RTU Frames](modbus-rtu.md) can also prepare an eight-byte Modbus read request for **PASTE**. To inspect the reply without retyping, capture and copy received bytes as described below.

**PREV** cycles through the last eight completely accepted sends, newest first. Each entry retains its exact bytes, transmit format, and suffix. Recall removes the previously appended ASCII suffix from the editable draft so sending it again does not append that suffix twice. Hex recall uses normalized spaced pairs. Editing a recalled draft or pasting bytes starts the next history search at the newest entry.

History is shared across UART/RS-485 during the current boot. Recall leaves the link and line settings unchanged; check them before SEND. History, drafts, and clipboard are RAM-only. Clearing the clipboard does not erase serial drafts or history. **CLEAR** clears only the visible transcript.

## Capture received bytes

1. **START** the configured serial interface. Tap **CAPTURE RX** before sending the request whose reply you want to inspect. Already queued bytes go to the transcript/log first; the new window starts after that drain. If the backlog cannot drain within four bounded reads, or a read fails, the new capture does not start and the previous frozen capture remains available.
2. Send your request and wait for the expected reply. The capture collects raw received bytes across multiple driver reads, independently of ASCII/Hex display and transmitted bytes. It holds at most **128 bytes** and shows the link and line settings used when capture began.
3. Tap **FREEZE RX**. Queued receive bytes are collected before the window freezes. **STOP** or Home also freezes an active window before releasing the driver. Bytes arriving after that boundary are outside the window; freezing alone leaves Serial running.
4. Tap **COPY RX** to place the frozen bytes on the shared clipboard. Use **Paste hex** in Byte Lab or **PASTE REPLY** and **DECODE** in RTU Frames. These capture/copy actions do not transmit.

Only a frozen, nonempty window can be copied. A 129th byte, a receive API error, or a backlog that cannot drain while freezing invalidates the capture and clears its bytes. COPY RX stays unavailable and the previous clipboard remains untouched. Serial display and any active SD logging continue after capture overflow; start a new capture to retry with a smaller window.

The window is **not a detected protocol frame**. It contains bytes delivered by the UART driver between manual boundaries, and may include echo, multiple responses or only part of a response. It does not establish serial timing, detect parity/framing errors or prove that the driver/FIFO lost no bytes. Check the expected length and protocol checksum before interpreting it. RTU Frames checks the captured frame's CRC and shape but cannot recover missing data.

The frozen window and its original link/baud/parity/stop identity survive Home and line-setting changes in RAM until restart, **CLEAR RX**, or the next successful capture start. **CLEAR RX** stops and clears only this window, leaving the shared clipboard, transcript, TX draft and connection alone. Transcript **CLEAR** and Byte Lab's **Clear copy** do not erase the frozen window. A window copied to the clipboard remains independent of future captures.

## Stop and logs

Published logs open in Files with a selector for each page's records. **COPY ROW** copies the selected record's original bytes, including controls and bytes above `7F`, regardless of Hex/ASCII display. It supports 1-128 bytes; larger records stay viewable and leave the clipboard unchanged. Record numbers count loaded valid rows and stay stable under filtering. Paste a copied row into Byte Lab or RTU Frames for inspection. See the [serial log viewer contract](data-formats.md) for paging and file limits. A saved row is a driver/log chunk rather than an automatically detected protocol frame.

**STOP** and Home publish an active log and drain pending transmission before releasing the interface. The drain timeout is calculated from a 256-byte message at the selected baud/framing plus a small scheduling margin, up to 340 ms at 9600 baud with parity and two stop bits. A drain failure is reported, and the last message may be incomplete. Driver acceptance or a completed drain does not establish that a peer received or acted on a message.

**START LOG** remains an explicit SD action while the interface is running. It records the actual RX/TX byte values, including line endings, using the existing [serial CSV format](data-formats.md). Saved logs open in Files. These input changes do not alter the file format or the existing SD publication/recovery behavior.

## Validation

`tests/uart_data_test.c` checks every input length through the limit for all four suffix choices, all 256 Hex byte values, exact history reconstruction, literal controls, invalid Unicode/hex/options, output clearing, and destination-buffer bounds. These pure checks run in CI and small known values run at startup.

On Windows, `./tools/test_uart_ui.ps1 -Snapshots` runs the actual terminal and LVGL with focused UART/GPIO/time/allocation adapters. It checks exact transmitted bytes, independent display/transmit formats, maximum and overlong drafts, Unicode retention, clipboard paste without activity, populated-history wrap, partial/failing writes, receive display, keyboard actions, missing SD, allocation failure, drain timing/error handling, and 100 alternating UART/RS-485 lifecycle cycles. Rendered PPMs go under `build/uart-ui/`. It requires a native C compiler, CMake, Ninja, and the managed LVGL sources; it reuses the offline UI library.

The host adapter does not measure electrical timing, UART FIFO behavior, RS-485 turnaround, SD fault recovery, physical touch, or either panel. Those remain in the [hardware checklist](hardware-smoke-checklist.md).

The added RX capture checks cover split reads, queued bytes at each boundary, empty/128/129/256-byte windows, source identity, Stop/Home retention, read failures, undrained backlog and disabled-button revalidation. The same host process runs RTU Frames request copy -> Serial transmit/capture -> RTU reply decode -> Byte Lab inspection. `-Quick` runs all functional checks with eight lifecycle cycles instead of 100; that mode passes for this addition with unchanged free heap and live allocation count. The runner builds with one job and requires a cached Debug LVGL library, without rebuilding it automatically. The single-job firmware build passed on 2026-09-29. ST7121 passed the internal LINE TEST, an empty capture/freeze with copy disabled, source retention through Home, receive-only RS-485 Start/Stop, and RTU request paste while stopped. Populated physical receive windows, electrical timing and fault recovery remain open.

`tools/test_serial_log_ui.ps1 -Snapshots` separately runs the saved-log reader/viewer with real fixture files and LVGL. Its checks cover original-byte copying from Hex/ASCII, 128-byte limits, filter/page selection, the 512-record cap, unchanged files, both buffer-allocation failures, clipboard handoff into RTU Frames/Byte Lab, and eight reopen cycles without heap loss. It compiles sequentially against cached LVGL and adds no background workers. Native checks and the 2026-09-29 firmware build pass; physical SD behavior and panel validation for the new record-copy controls remain pending.
