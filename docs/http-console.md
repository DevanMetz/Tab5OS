# HTTP Console

Open **HTTP Tool** from the launcher to inspect one HTTP exchange over Wi-Fi. Select GET, POST, PUT or DELETE, enter an HTTP/HTTPS URL, optionally add up to four `Name: value` headers and a POST/PUT body, then tap **SEND**. HTTPS uses the certificate bundle and hostname verification. Plain HTTP requires an unchanged second SEND within five seconds; edits, Cancel, Clear, Home or expiry disarm confirmation. Redirects are displayed and never followed automatically.

Requests accept at most 255 URL bytes, 511 header bytes and 1,023 body bytes. Unicode characters can occupy several bytes; oversized drafts stay visible and are rejected before confirmation or network activity instead of being truncated. The worker uses a snapshot, so editing the form cannot change a running request. The response shows the captured method and endpoint separately from the editable form, plus status, received body bytes and elapsed time with a 4 KiB printable preview. The endpoint omits query strings and fragments and labels that omission; the actual request still sends its query. Binary bytes appear as dots.

An accepted SEND clears the previous preview and identifies the new request. A result arriving after Home identifies its original request even though the reopened form has different values. If a completed result is waiting for the UI, tapping the stale **WORKING** control shows that result before another SEND is accepted; it does not arm or start another request. **CLEAR** clears the displayed or waiting result when idle, or clears the preview while an active request continues. It also dismisses plain-HTTP confirmation. Cleared results do not reappear on the next UI tick, and a waiting storage error is still reported with logging turned off.

Informational responses such as `100 Continue` and `103 Early Hints` are consumed while waiting for the final response; their fields cannot replace its type or redirect target. Protocol switching (`101`) is unsupported and rejected. Each response header block and chunked trailer section is validated within 8 KiB. Trailer fields are discarded after validation and do not appear in the preview or metadata log. Short Content-Length bodies, invalid/unfinished chunks and interrupted trailers are incomplete exchanges. Close-delimited bodies complete only when the peer closes normally, never on a timeout. Bodyless `204` and `304` responses show zero bytes, including a `304` with a representation Content-Length. These rules follow [HTTP/1.1 message framing](https://www.rfc-editor.org/rfc/rfc9112.html#section-6.3).

## Stop a request

**CANCEL** requests cooperative stopping; **Home** also cancels before detaching the screen. Cancel can also dismiss the plain-HTTP confirmation before any request starts. The worker retains ownership until the connection and any optional metadata write finish cleanup. SEND becomes available after cleanup, and OTA continues to block active work.

A 15-second request budget starts before worker creation. The transport checks it around connection and I/O operations, with socket polls of at most 100 ms; continuously arriving headers or body chunks cannot keep the socket exchange running indefinitely. The original ten-second I/O wait remains within that budget.

Hostnames are resolved through [lwIP's asynchronous DNS API](https://www.nongnu.org/lwip/2_1_x/group__dns.html) on its TCP/IP thread. The worker checks Cancel and deadlines while waiting and passes a numeric peer to ESP-TLS, with the original hostname retained for certificate verification and SNI. The HTTP Host header also keeps the original hostname. DNS and connection setup share the ten-second connect wait inside the total budget; silent lookup failures can therefore stop before 15 seconds.

The platform DNS backend may finish or retry a lookup after the request stops. Its callbacks carry lookup IDs instead of request pointers; late results and queued work are ignored after detachment and cannot open an HTTP connection or change the next request. The budget remains cooperative around SDK operations. Physical Wi-Fi, DNS and TLS latency still need hardware validation.

Cancelled/deadline results retain any body preview received before stopping and clearly report the interruption. Stopping a connection cannot undo a POST, PUT or DELETE already processed by its peer; inspect the result before retrying.

## Optional metadata log

Enable **SD LOG** explicitly for each app entry to append `/HTTP/HTTPLOG.CSV`. Rows contain timestamp, method, a query/fragment-free URL, status, received body bytes, duration and outcome. Headers, authorization values, bodies and previews are excluded. Cancelled and budget-expired requests use `cancelled` and `request_deadline`; malformed/oversized headers or trailers and unsupported upgrades use `invalid_response_headers`. Invalid/unfinished body framing uses `incomplete_response`. Existing columns remain compatible. See [Data formats](data-formats.md).

Every result reports the logging choice captured when that request started, including interrupted and failed HTTP exchanges. **SD LOG was OFF for this request** describes an unlogged request even if logging was enabled while it ran. **Metadata appended** appears after flush, durable sync and close succeed. A log failure keeps the network result and preview visible, reports the storage error and says that saving was not confirmed, then turns **SD LOG OFF**. Re-enter with a writable card and opt in again before further logging. A reported failure can leave bytes on the card; the message does not promise that the row is absent or durable.

The next append repairs an unfinished final CSV row while preserving earlier complete rows; an incomplete initial header is rewritten. The request remains busy until file cleanup finishes, including when Home closes the screen. The worker keeps the captured network result until metadata cleanup finishes, then publishes it to the UI. Editing the form or toggling logging during a request cannot change that request's captured log choice, method or URL. The elapsed time stored in the row covers the network exchange through connection cleanup, before the metadata write.

## Host checks

Firmware requires `CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS=y` and `CONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT=y`, both set in the checked-in defaults. The pinned SDK only assigns custom transports when HTTPS support is enabled, including for plain HTTP requests. If an existing `sdkconfig` disables either option, enable **Component config > ESP HTTP client > Enable HTTPS** and **Enable custom transport** in `idf.py menuconfig` before rebuilding; compile guards report the required options.

Run `./tools/test_http_network.ps1` with the pinned ESP-IDF source checkout and cached Debug LVGL library available. It runs the actual app, guarded transport and SDK HTTP client/parser against real loopback TCP peers, including informational replies, trailers, bodyless responses, close-delimited bodies, continuously trickled headers/body/trailers, cancellation, short responses, redirects and repeat cleanup. Logging cases use actual `storage_io.c` against temporary files with injected directory/open/write/flush/sync/close failures, independent CSV parsing and tracked file closure. Request/SDK allocation counts, files, sockets and tasks must return to zero. See [Testing](testing.md#network-fixtures-and-ui).

The native CI job invokes all 79 HTTP scenarios after the MQTT suites, using the SDK source bundle and the LVGL library built within that job. It retains case/compiler logs and HTTP metadata CSVs, and stops before Diagnostics if HTTP fails. Missing required HTTP sources fail before CMake starts. The TLS and DNS adapter limits below also apply in CI.

The host TLS endpoint intentionally fails closed and cannot establish HTTPS. Its DNS/TLS case checks that the original hostname remains configured when the numeric peer is passed to TLS and that failure sends no cleartext; it does not validate the firmware's certificate/handshake implementation. A separate host thread supplies synthetic DNS replies and queued callbacks, including replies after cancellation and during the next request. This checks the app's resolver boundary and lifetime handling, not the real lwIP UDP exchange. IPv6 cases check address formatting and lookup dispatch; the native TCP endpoint only connects to IPv4 loopback.

## Hardware validation

Run the synthetic peer on a PC connected to the tablet's LAN:

```powershell
python tools/http_test_server.py --bind <PC-LAN-IPv4> --port 18080 --mode stream --seconds 25 --interval 0.05
```

Enter `http://<PC-LAN-IPv4>:18080/` on the tablet and confirm SEND. Check a budget expiry near 15 seconds, the retained partial preview, cleared busy state and a fresh successful request. Repeat with `slow-headers` and `silent`, then Cancel and Home early in each mode. In header waits no body preview is expected. Stop each fixture before choosing another mode.

Mode `informational` sends `103`, waits for `--seconds`, then sends `100` and a final `200` with body `OK`. Check that only the final status is reported, and Cancel during the wait. Mode `trailers` sends body `OK`, then trickles a trailer value until `--seconds`; check completion for a short wait, and Cancel/Home or the request budget for a 25-second wait. Trailer bytes must not enter the body preview. Repeat these exchanges while watching heap recovery.

Verify HTTPS success and certificate/hostname rejection through the guarded transport, DNS failures and cancellation latency, SD log outcomes/faults, repeated request/cancel heap stability and both display families. Record image identity, power source and reset/heap evidence in the [smoke checklist](hardware-smoke-checklist.md). Host success and firmware builds do not establish these hardware results.

Use the [controlled UDP DNS fixture](dns-fixture.md) on an isolated test LAN to exercise the tablet's actual resolver with immediate/delayed/dropped replies, NXDOMAIN and SERVFAIL. Its setup uses the LAN's DHCP DNS setting and fresh names under `fixture.test`. The procedure covers Cancel/Home during lookup, a late reply while another request waits, the shared connect budget and recovery to a fresh successful request. The fixture defaults to loopback; native DNS adapter checks remain separate from this device work.
