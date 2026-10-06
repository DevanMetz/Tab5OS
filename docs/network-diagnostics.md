# Network Diagnostics

Open **Network** from the launcher or **Tools** in Wi-Fi settings. **REFRESH** updates the connected access point, channel/RSSI, IPv4 address, gateway, mask and primary DNS address, and updates which actions are available. Enter a hostname or literal address for **LOOK UP** or **PING x4**. Use at most 127 ASCII bytes, including any surrounding whitespace, and omit scheme/path. The form retains the first excess character and rejects overflow instead of looking up a shortened target. Use a unicast target for ping. **mDNS** lists up to eight discovered service types.

**CANCEL** requests a stop; Home requests the same stop and detaches the UI immediately. DNS and ping check cancellation cooperatively. An already-running mDNS query finishes its three-second discovery window and frees its results before cancellation completes. New operations remain disabled and OTA restart remains blocked until cleanup finishes. Re-entry shows the pending cancellation or completed result with its original target. Each operation releases its worker task after cleanup.

## DNS lookup

Diagnostics and [HTTP Console](http-console.md) share `network_resolver.c`. Numeric addresses bypass DNS. Names are dispatched to the lwIP TCP/IP thread using its IPv4-first/IPv6-fallback policy. Diagnostics applies a ten-second cooperative wait budget from the accepted action, polling cancellation every ten milliseconds. The ping budget below then begins after lookup; DNS plus ping can therefore take about sixteen seconds under cooperative scheduling.

The resolver supports the two apps concurrently, returns up to four addresses within the SDK's configured answer capacity, and keeps copied names and unique lookup IDs isolated from subsequent requests. Late replies cannot reach a returned worker stack or replace another operation's result. Platform DNS retries may continue after the app stops. The pinned configuration stores one address per name; increasing that SDK limit does not overflow the cached-answer buffer or a caller that requests only one address.

## Bounded ping

Four echo probes share a six-second cooperative budget after DNS. Each probe uses one absolute one-second wait for sending and receiving, with a half-second gap before the next probe. Nonblocking socket operations and polls of at most 100 ms keep unrelated traffic, repeated empty reads, interrupted calls and send/receive retries from restarting the wait. Cancellation is checked around polls and reads and throughout retries and gaps. A silent target normally completes the four probes in about 5.5 seconds, with loss reported. SDK calls and scheduling still require latency measurement on hardware; this is not a strict real-time guarantee.

A counted reply must have the selected source address, complete IP/ICMP framing, valid checksums, echo-reply type/code, matching identifier/sequence and all 32 echoed payload bytes. Each run puts a fresh random value in that payload so late replies from an earlier run cannot match by identifier/sequence alone. Echo data is returned unchanged by [ICMPv4](https://www.rfc-editor.org/rfc/rfc792.html) and [ICMPv6](https://www.rfc-editor.org/rfc/rfc4443.html#section-4.2). Invalid, truncated or unrelated packets remain lost probes and cannot extend the deadline. The socket is closed after success, loss or any failure.

IPv4 options up to the full 60-byte header are accepted when the complete echo reply fits the 128-byte receive buffer. The pinned lwIP raw path delivers an IPv6 header with bare ICMPv6; extension-header replies are not counted by this path. IPv6 destination scope is preserved for scoped addresses. Reported elapsed reply times include the local send/wait operations.

## Host checks

```powershell
./tools/test_network_ping.ps1
./tools/test_network_ui.ps1
```

The ping runner requires native Clang and compiles the actual `network_ping.c` with small deterministic clock/socket adapters. It uses no real network, raw socket privilege, tablet or LVGL service. Forty-six cases run with IPv6 disabled and 74 with IPv6 enabled. They check request bytes/checksums, reply filtering, IPv4 options, prior-run payload rejection, timing/statistics, silence/continuous noise, empty/retry/late reads, interrupted I/O, setup/transfer faults, scheduler delays, cancellation at ten points, single socket closure and unsupported IPv6 without opening a socket. Compiler intermediates and logs go under `build/network-ping/`. The release workflow runs both configurations.

The UI runner requires Windows/native Clang, CMake/Ninja and the cached Debug LVGL library from `tools/test_offline_ui.ps1`; it never rebuilds that library. Thirty-four scenarios run with both one-address and four-address DNS configurations, for 68 checks. They use the actual app/resolver and LVGL with native threads and synthetic DNS/ping/mDNS services. Cases cover cached/delayed/failed lookups, ten-second timeout, queue/memory errors, Cancel/Home, stale callbacks, concurrent resolver clients, bounded answer copies, original target after edits, pending completions, mDNS ownership through blocked cleanup, input rejection, task failure, link refresh and 25 app/worker lifecycles with unchanged LVGL allocation and Windows handle counts. Logs and PPM previews go under `build/network-ui/`. Use `-Modes cancel-dns,mdns-cleanup,resolver-concurrent` for selected cases.

These checks validate the application lifecycle and packet interpretation. They do not emulate lwIP packet delivery, physical Wi-Fi, routing or the display. Once the recorded power issue is resolved, repeat DNS timeouts/Cancel/Home, mDNS cancellation/cleanup and unicast IPv4/IPv6 pings with loss and unrelated traffic on the tablet. Use the [controlled DNS fixture](dns-fixture.md) to deliver an old reply during the next lookup, record timing/heap/reset evidence and exercise both panel families in the [hardware checklist](hardware-smoke-checklist.md). Physical cancellation latency, resolver-table recovery and IPv6 behavior remain open validation work.
