# Controlled DNS fixture

`tools/dns_test_server.py` provides a synthetic UDP DNS peer for testing HTTP Console's asynchronous lookup on the tablet. It uses Python's standard library, defaults to `127.0.0.1:15353`, and answers the configured `fixture.test` zone and its children. The `.test` suffix is [reserved for DNS testing](https://www.rfc-editor.org/rfc/rfc2606.html#section-2). Zone matching is case-insensitive; names outside that zone receive REFUSED.

| Mode | In-zone result |
| --- | --- |
| `answer` | A answer immediately; optional AAAA with `--answer-v6` |
| `delay` | The same answer after `--seconds`, independently for each query |
| `drop` | Receives queries and sends no reply |
| `nxdomain` | Authoritative name error |
| `servfail` | Server failure |

A and AAAA records use TTL zero by default; `--ttl` accepts 0-86400 seconds. Unsupported record types receive an empty successful answer. ANY returns the configured A and optional AAAA records. The listener uses IPv4 UDP even when returning an IPv6 address. It implements one question, an optional EDNS(0) OPT, and bounded 512-byte datagrams using [DNS wire framing](https://www.rfc-editor.org/rfc/rfc1035.html#section-4.1) and [EDNS(0)](https://www.rfc-editor.org/rfc/rfc6891.html#section-6). Unsupported EDNS versions receive BADVERS. Malformed queries receive FORMERR when a complete query header is available; short packets, incoming responses and oversized packets are dropped.

The peer performs no recursion, forwarding, TCP service or file access. It queues at most 64 delayed replies and drops further in-zone queries while full. Other queries remain responsive during a delay. Ctrl+C closes the socket and discards the queue. A client closing before its reply cannot stop the peer. Logs contain counts, record types, response codes and reply/query ordinals; query names, client addresses and packet data are omitted. The startup line shows the operator's configured listener, zone and answer.

## Host validation

```powershell
python tests/dns_fixture_test.py
```

Fourteen real-loopback tests compare replies with independent wire bytes and exercise A/AAAA/ANY, zone boundaries, TTL, error modes, EDNS, exact name/datagram limits, malformed inputs, delayed/drop behavior, closed peers, queue bounds, prompt shutdown and log redaction. A delayed DNS answer also feeds an independent HTTP client into the existing echo fixture with the original Host header. Invalid CLI settings fail before binding. These checks validate the PC fixtures; they do not run the tablet's lwIP UDP resolver or a TLS handshake. The release workflow runs the fixture checks with the other host tests.

## Tablet setup

Resume device testing only after the recorded USB/power issue is resolved; see the [hardware checklist](hardware-smoke-checklist.md). Use an isolated test LAN whose DHCP DNS setting points directly to the PC's LAN IPv4 address, with no alternate resolver or router DNS proxy. The tablet currently takes DNS settings from Wi-Fi/DHCP and has no custom DNS-server control. Router configuration and any PC firewall permission are manual setup steps. Use UDP port 53 for the tablet; port 15353 is a convenient host-test default.

In two PC terminals, replace `<PC-LAN-IPv4>` with the PC address on that LAN:

```powershell
python tools/http_test_server.py --bind <PC-LAN-IPv4> --port 18080 --mode echo
python tools/dns_test_server.py --bind <PC-LAN-IPv4> --port 53 --answer <PC-LAN-IPv4> --mode answer
```

Reconnect the tablet to the test Wi-Fi so it obtains those DNS settings. Open HTTP Tool, enter `http://run1.fixture.test:18080/`, and confirm SEND. Expect HTTP 200 and an echo containing the original Host name. The HTTP fixture's request counter should advance once. Use a fresh child name (`run2.fixture.test`, `run3.fixture.test`, etc.) for each request to avoid resolver/proxy caches, including after a mode change. Stop the DNS fixture with Ctrl+C before changing its mode; keep the HTTP echo peer running.

For cancellation and a late reply during another lookup:

```powershell
python tools/dns_test_server.py --bind <PC-LAN-IPv4> --port 53 --answer <PC-LAN-IPv4> --mode delay --seconds 6
```

Start a request to a fresh name and tap CANCEL within a second. Confirm interruption and that SEND becomes available after cleanup. Promptly send to a different fresh name. The first DNS reply should arrive while the new lookup waits; only the new request should reach the HTTP echo peer, with its captured Host name. Record the fixture counters, UI outcome, stop latency and heap recovery. Resolver retries can create additional delayed DNS replies; they must not create HTTP requests for the cancelled job. Repeat with Home and re-entry.

For a wait beyond the connect budget, use `--mode delay --seconds 25`, then `--mode drop`. DNS and connection setup share a ten-second wait within the HTTP request's fifteen-second budget; a resolver error can finish sooner. Check the observed interruption/failure, cleanup and fresh-send availability. Late replies after cancellation/timeout must not open HTTP connections. Repeat with `--mode nxdomain` and `--mode servfail`, checking that no HTTP request reaches the echo peer. Restore `--mode answer` and verify a fresh successful lookup afterward.

Record the image identity, power source, panel, timings, counters and initial/final internal/PSRAM heap in the hardware checklist. Original Host/SNI preservation, real Wi-Fi/DNS latency and certificate/name verification remain separate device gates; the native tests and this fixture do not establish a pass.
