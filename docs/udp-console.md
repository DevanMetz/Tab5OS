# UDP Console

UDP Console sends one custom datagram and displays the first reply from the selected IPv4 address and port. It is the last tile on the launcher's eighth row. Connect Wi-Fi in Settings first; no SD card is required.

## Send and inspect

1. Enter the peer's full decimal IPv4 address and UDP port (1-65535). Hostnames, shortened/octal addresses, leading zeros, multicast, limited broadcast, and the current Wi-Fi subnet's directed broadcast are rejected. Use a unicast peer; the app cannot infer a remote network's mask.
2. Leave **Source port** at `0` for an automatically assigned port, or enter a required local port (1-65535). An occupied port fails instead of enabling address reuse. The result shows the port actually assigned to the exchange.
3. Choose **Hex byte pairs** or **Literal ASCII**, with at most 128 transmitted bytes. Hex accepts complete pairs such as `00 01 FF`, optionally separated by ASCII whitespace; `0x` prefixes and punctuation are invalid. ASCII sends literal 7-bit characters without escape expansion: typing `\n` sends a backslash and `n`. Use Hex for arbitrary bytes, including `00`. Empty input sends a valid zero-byte datagram.
4. Tap **SEND** to review the endpoint, source-port choice, and byte count. Tap **CONFIRM** within five seconds with unchanged inputs. Editing a field, changing format, cancelling, leaving Home, or letting the confirmation expire disarms it. Keyboard Done only dismisses the keyboard.
5. Inspect the original request and the response in the scrollable hex/ASCII preview. Non-printable bytes appear as dots in the ASCII view. The request identity stays attached to its result even after you edit the next request or leave and reopen the app.

Address and port fields preserve invalid characters and pasted line breaks for validation. They do not join split lines or remove signs/suffixes into a different destination. Overlong drafts remain invalid after Home/re-entry. Payload whitespace follows the selected Hex/ASCII rules above.

There is one send and no automatic retry. The three-second exchange budget starts when the confirmed request is accepted and includes worker scheduling, socket setup, sending and waiting for a reply. A delayed start uses only the remaining time; an already expired worker creates no socket and sends no datagram. The displayed total duration includes scheduling time. Replies from another address or port are ignored. The app closes its socket after the first matching reply, timeout, error, Wi-Fi loss, or cancellation. No socket remains listening when idle. Scheduling and cleanup can delay the final result beyond the exchange budget.

Replies of up to 512 bytes appear in full, including an empty reply. Larger replies show the first 512 bytes and an explicit truncation message; the full datagram length is unknown. Receive time measures the interval after the send call, not one-way network latency. A successful send does not prove delivery.

**CANCEL** and Home stop waiting without blocking the interface. They cannot recall an already transmitted datagram. The app keeps inputs and the last exchange in RAM until restart, with separate Hex and ASCII drafts. It does not save profiles or log/export payloads. Traffic is unencrypted and replies are unauthenticated. Choose a peer and payload whose effects you understand; the console does not interpret the device protocol.

## Byte clipboard

Prepare bytes in [Byte Lab](offline-tools.md#move-bytes-between-apps), tap **Copy bytes**, then return here and tap **PASTE BYTES**. This switches to Hex, replaces only that draft, hides the keyboard, and disarms any pending confirmation. Peer and port fields and the separate ASCII draft remain unchanged. Review them and use SEND/CONFIRM to transmit.

After the exchange closes, **COPY REPLY** copies a complete 0-128-byte reply to the shared RAM clipboard. It copies the original reply even if the next request's fields have changed. Missing, larger, or truncated replies cannot overwrite the previous copy. Return to Byte Lab and **Paste hex** for checksums and numeric interpretation. Clipboard controls are disabled while an exchange is active. Neither action sends traffic; the clipboard clears on restart or Byte Lab's **Clear copy**.

## PC fixture

The bundled fixture echoes the exact request, including binary or empty payloads. It defaults to loopback; tablet testing needs an explicit reachable PC address:

```powershell
python tools/udp_device_fixture.py --self-test
python tools/udp_device_fixture.py udp --bind <PC-LAN-IPv4> --port 19007
```

Enter that PC address and port in UDP Console, keep source port `0`, and send `00 01 02 03`. Both previews should show those four bytes. The fixture reports the observed source port and packet lengths. Stop it with Ctrl+C; allow the selected UDP port through the PC firewall if necessary.

Run the fixture again with `--reply-bytes 0`, `512`, `513`, or `2048` to check empty, exact-limit, and truncated replies. Synthetic replies repeat bytes `00` through `FF`. Use `--outcome silent` for a timeout or `--delay 4` for a late reply. Cancel or go Home while waiting, then reopen the app and send a fresh request. The fixture does not forward requests to other devices.

## Verification

The `udp-clipboard` case encodes Float32 `1.5` in the actual Byte Lab UI, pastes it into UDP, verifies confirmation was disarmed, checks exactly one four-byte request, copies the reply, and decodes it back in Byte Lab. It also checks a 128-byte round trip, busy controls, unchanged ASCII/peer drafts, and no traffic from clipboard actions. Empty/oversized/truncated reply cases verify clipboard limits without truncation or overwriting the previous copy.

`./tools/test_device_network.ps1` runs the real LVGL app and worker with native Windows sockets/threads against local Python peers. UDP cases cover malformed/overlong input, confirmation changes/expiry, empty and maximum requests, exact and oversized replies, wrong-peer filtering, source-port selection/conflicts, timeout, Wi-Fi state changes, worker allocation failure, cancellation, immediate re-entry, and 25 repeated exchanges. It also runs the NTP Lab and Wake-on-LAN regressions. Logs and rendered PNGs are written under `build/device-network/`.

The 56-scenario suite adds 17 controlled delayed-start cases across the three apps. UDP expiry, a remaining one-second receive budget, Cancel/Home before execution, stale admission and 25 timeout/retry pairs check zero packets from expired requests, fresh retries and stable resources. All 17 preceding-source controls fail. The runner executes frozen sources and its copied peer fixture; CI retains source/library/executable hashes, logs and packet bytes. Actual ESP-IDF scheduling and physical Wi-Fi latency remain separate checks.

To focus a run, use `./tools/test_device_network.ps1 -Modes udp-echo,udp-truncated,udp-home`. The runner needs Python, a native C compiler, CMake, Ninja, and the firmware's managed LVGL sources. Its Windows adapter accounts for [WinSock's oversized-datagram return convention](https://learn.microsoft.com/en-us/windows/win32/api/winsock/nf-winsock-recv) and uses a wildcard reservation for the port-conflict case because [Windows permits some specific-address/wildcard bind pairs](https://learn.microsoft.com/en-us/windows/win32/winsock/using-so-reuseaddr-and-so-exclusiveaddruse).

The 2026-09-29 ST7121 pass used USB-injected touches and a PC peer over Wi-Fi. After correcting an ESP-IDF task-cleanup crash, 25 four-byte echo exchanges, reply-to-Byte-Lab copy, three-second timeout, Cancel/Home, and 0/512/513-byte reply cases passed. Heap remained stable across the repeated batch; oversized replies preserved the clipboard and an empty reply copied as empty. See the exact scope and firmware identity in the [hardware smoke checklist](hardware-smoke-checklist.md#usb-only-development-pass-2026-09-29). Independent touch/electrical measurements, further device/network faults and the second panel family remain open.
