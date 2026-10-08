# NTP Lab and Wake-on-LAN

These apps are on the eighth launcher row. Connect Wi-Fi in Settings first. Both use the IPv4 address and UDP port you enter, keep inputs/results only in RAM, and operate only after an explicit action. Neither needs an SD card.

IPv4 addresses require four decimal octets from 0 to 255, with no leading zeros, whitespace, shortened forms, or hostnames. Ports accept up to five decimal digits in the range 1-65535. Invalid characters, pasted line breaks, and excess length remain invalid, including after Home/re-entry; the fields do not silently remove them to produce a different destination or port.

For custom hex/ASCII request-and-reply exchanges, see [UDP Console](udp-console.md), which shares the same PC fixture and optional native test runner.

## NTP Lab

NTP Lab helps diagnose a time server and compare it with the tablet's system clock. It does not set the clock or change the Clock app's synchronization settings.

1. Enter the server's full decimal IPv4 address and UDP port (normally 123). Use Network's DNS lookup first if you only know a hostname.
2. Tap **SAMPLE 4**. The app takes four samples, spaces requests at least two seconds apart, and waits at most two seconds for each reply. The ten-second session budget starts when SAMPLE 4 is accepted and includes worker scheduling. A delayed start uses only the remaining budget; an already expired worker opens no socket and sends no request.
3. Inspect each sample's round-trip time, estimated network delay, and clock offset. The summary selects the successful sample with the lowest delay and reports its corresponding offset. Positive offset means the server is ahead of the tablet.
4. **STOP** or Home cancels the run. Reopening the app displays the completed/cancelled request identity and results; changing the input does not relabel an earlier result.

The tablet needs a plausible system date before measuring. An unset clock is rejected, and a clock step during a request invalidates that sample. The supported date range is 2020 through 2099, including the NTP seconds rollover in 2036. This is a diagnostic estimate: network asymmetry, tablet scheduling, and the local clock affect the result. The selected server and UDP replies are not authenticated.

The elapsed session duration includes scheduling time. The displayed tablet clock reference and clock-step checks start when measurement begins, so a scheduling delay does not become a false clock-step report. Stop/Home keep the operation busy until its worker finishes cleanup; delayed execution or cleanup can delay the final result beyond the exchange budget.

The parser checks the request's echoed transmit timestamp, response length, version, server mode, leap state, stratum, and receive/transmit timestamps. An unsynchronized server is reported as an error. A stratum-zero Kiss-o'-Death response stops the burst, including a server asking the client to reduce its request rate. This version supports the base 48-byte NTP message; extension fields and authentication trailers are rejected rather than silently ignored.

Packet handling and delay/offset calculations follow [RFC 5905](https://www.rfc-editor.org/rfc/rfc5905.html). This app implements a bounded diagnostic exchange, not the RFC's clock-selection and clock-discipline system.

## Wake-on-LAN

Wake-on-LAN sends the standard magic packet for a device whose firmware, network adapter, operating system, and power state support remote wake.

1. Enter the target adapter's MAC address as `02:11:22:33:44:55`, `02-11-22-33-44-55`, or `021122334455`. All-zero, multicast, and broadcast MAC addresses are rejected.
2. Enter the destination IPv4 address and UDP port (normally 9). For a subnet broadcast, calculate the address from the network mask using [Subnet Lab](offline-tools.md#subnet-lab): for example, `192.168.1.255` is the broadcast address of `192.168.1.0/24`. It is not the broadcast address for every network using `192.168.1.x`. Broadcast forwarding and Wi-Fi client isolation depend on the network.
3. Tap **SEND**, then **CONFIRM** within five seconds without changing any field. Exactly one 102-byte datagram contains six `FF` bytes followed by sixteen copies of the MAC address.
4. The result identifies the MAC, destination, and port. A successful send means the local network stack accepted the packet; it does not prove delivery or that the device woke. Use Network's ping or a service connection to check the device separately.

**CANCEL** and Home can stop work that has not yet sent. A transmitted packet cannot be recalled. The app does not discover devices, retry automatically, store profiles, or support SecureOn passwords. Wake traffic is unencrypted.

The two-second send budget starts when the confirmed request is accepted and includes worker scheduling. An already expired worker reports timeout without creating a socket or sending a packet. The displayed duration includes the wait; cancellation retains worker ownership until cleanup finishes.

See the [AMD magic-packet format](https://docs.amd.com/r/en-US/am011-versal-acap-trm/Magic-Packet-Events) and [Intel remote-wake requirements](https://www.intel.com/content/www/us/en/support/articles/000005793/ethernet-products.html) for protocol and adapter background.

## PC fixtures

The bundled Python tool provides an NTP responder and a receiver for wake packets. Its default bind address is loopback, so tablet testing requires an explicit reachable PC address:

```powershell
python tools/udp_device_fixture.py --self-test
python tools/udp_device_fixture.py ntp --bind <PC-LAN-IPv4> --port 19123 --offset-ms 100
python tools/udp_device_fixture.py wol --bind <PC-LAN-IPv4> --port 19009
```

Run one command per terminal as needed and stop with Ctrl+C. Allow only the selected UDP port through the PC firewall. For Wake-on-LAN fixture testing, use the PC's unicast address and port 19009 with the sample MAC above; the receiver validates the payload and does not forward it to a target.

The NTP fixture uses the PC wall clock plus the configured offset, not an independently verified time reference. Use `--outcome kod`, `unsynced`, `mismatch`, `short`, or `silent` to exercise failures, or `--delay 3` to force a reply beyond the per-sample timeout. Its `TEST` reference ID labels synthetic replies.

## Verification

Pure host tests cover protocol encoding, malformed inputs, timestamp calculations, and packet construction. The optional Windows runner `./tools/test_device_network.ps1` exercises the actual LVGL apps and network workers against loopback peers with native threads and sockets. Invalid-input cases verify preserved drafts and zero workers, sockets, and packets. It needs Python, a native C compiler, CMake, Ninja, and the managed LVGL sources fetched by the firmware build.

The complete suite has 56 scenarios: the original 39 plus 17 delayed-start controls across all three UDP apps. A held native worker and controlled monotonic clock verify expired requests send nothing, remaining budgets are not renewed, duration includes scheduling, NTP keeps its measurement reference separate, and Stop/Home prevent stale admission while cancellation is pending. Each app completes 25 timeout/retry pairs with stable native handles and LVGL memory after closing the UI. All 17 controls fail against the preceding sources. These tests do not establish ESP-IDF scheduling, physical Wi-Fi timing or actual wake behavior. CI retains source snapshots, hashes, logs and independent peer packet bytes as `device-network-logs`.

On 2026-09-29, ST7121 with USB-injected touches and a PC peer passed NTP's four-sample exchange, minimum-delay selection, `RATE` refusal, mismatched-timestamp rejection, timeout, Stop and Home/re-entry. Wake-on-LAN produced exactly six valid 102-byte packets for a synthetic MAC at the unicast PC receiver, with zero packets after one tap, expiry, Cancel or Home. The receiver forwarded nothing. The corrected firmware also passed actual worker TLS cleanup on both CPU cores. These checks do not establish clock accuracy, independent physical touch behavior, actual wake support or the second panel family; the exact tested scope remains in the [hardware smoke checklist](hardware-smoke-checklist.md#usb-only-development-pass-2026-09-29).
