## 1. Root-cause the on-device `.local` failure (firmware, S3 DevKitC)

- [x] 1.1 Build `apps/modbus-server` (what the board runs) for the S3 DevKitC with the `full` profile plus `CONFIG_MDNS_RESOLVER=y` and `CONFIG_NET_SHELL=y`; flash it and keep a console logger running from boot
- [x] 1.2 From the shell, run `net dns` (server list) before and after the DHCP lease, then `net dns rpi5-d83add9f145a.local`; record the result and the boot log's `net_sock_svc` line
- [x] 1.3 Capture port 5353 on the LAN during the query (`tcpdump` on the Pi or the Mac) to see whether the query leaves the board and the answer comes back
- [x] 1.4 Isolate the cause: no mDNS server with DHCP DNS (`net dns`), static servers break unicast ordering, and with the responder running the resolver never reads the unicast answer (capture on the Pi)
- [x] 1.5 Write the finding and the fix into `tedge-zephyr/docs/zephyr-mdns-resolver-bugs.md`, and resolve the design's open question on the resolver API (callback API vs `getaddrinfo()`)

## 2. Kconfig (firmware, tedge-zephyr module)

- [x] 2.1 Add `CONFIG_TEDGE_REMOTE_ACCESS_MDNS` (default `y`, under `TEDGE_REMOTE_ACCESS`, selecting only `NET_UDP`)
- [x] 2.2 Add `CONFIG_TEDGE_REMOTE_ACCESS_RESOLVE_TIMEOUT_MS` (default 5000, range 500–10000) with help text
- [x] 2.3 Add `CONFIG_TEDGE_REMOTE_ACCESS_RESOLVE_ATTEMPTS` (default 3, range 1–5) and `CONFIG_TEDGE_REMOTE_ACCESS_RESOLVE_RETRY_DELAY_MS` (default 500, range 0–5000), plus a `BUILD_ASSERT` that the worst-case total stays under 30 s
- [x] 2.4 Add kconfig test cases: mDNS on by default with remote access, absent without it, and turned off explicitly; run `tests/kconfig/check-kconfig.sh`

## 3. Resolution and policy (firmware, `tedge_remote_access.c`)

- [x] 3.1 Add a `resolve_target()` helper: IP literal → as today; `.local` without the option → fail "mDNS is not built in"; `.local` → the module's own one-shot query (`mdns_once()`, builder/parser in `tedge_ra_target.c`); others → `dns_get_addr_info()` with the bounded timeout; return the address or a reason distinguishing timeout from failure
- [x] 3.2 Wrap the lookup in the retry loop: retry on timeout and transient errors, not on an authoritative no-such-name; WRN-log each failed attempt; name the attempt count in the final reason
- [x] 3.3 Replace the inline `getaddrinfo()` block in `bridge()` with the helper, keeping resolution on the session thread
- [x] 3.4 In `policy_check()`, require a `.local` answer to be loopback, the device's own address or on its subnets under every policy, with a reason naming the address
- [x] 3.5 Store the resolved address in the session and include `host (addr)` in the open/close events and logs when the host was a name; keep IP-literal text unchanged and the twin session list as requested

## 4. Tests (firmware, host-side unit tests)

- [x] 4.1 Unit-test the `.local` suffix detection (case-insensitive, trailing dot, `local` alone, `x.localhost` not matched)
- [x] 4.2 Unit-test the link-local rule for `.local` answers under the LAN, allow-list and local policies (factor the decision into a pure function if needed)
- [x] 4.3 Unit-test the mDNS query builder and answer parser (compression, case, wrong id, malformed and looping input)
- [x] 4.4 Unit-test the retry decision (which errors are retried) and the reason text with the attempt count; factor the classification into a pure function
- [x] 4.5 Unit-test that the longest close-event text (63-char host, address, max counters) fits the event text (now 200 bytes) without cutting the counters
- [x] 4.6 Run the unit test suite and the kconfig checks

## 5. Device verification (firmware, real boards)

- [x] 5.1 S3 DevKitC `full`: open SSH to the Pi by `.local` name from Cumulocity; confirm the session, both events with name and address, and that the device's own `.local` name still answers
- [x] 5.2 Same board: a non-existent `.local` name fails after the configured attempts (about 16 s) with the mDNS reason and attempt count, logs one WRN per attempt, and frees the seat; an allow-listed name still works
- [x] 5.3 Same board: block the Pi's mDNS answers for the first query (e.g. `avahi-daemon` stopped, restarted within 5 s of the connect) and confirm the session succeeds on a later attempt; also open a session whose name answers only on the third attempt (~11 s) and confirm the Cumulocity web client is still waiting when the tunnel comes up
- [x] 5.4 C6 `full` (on rpi5 `/dev/ttyACM1`): SSH to the Pi by `.local` name, events with name and address; flash/RAM deltas recorded for C6, S3 and QT Py. No release ota build includes remote access, so there was no ota image to repeat it on
- [x] 5.5 Build with `CONFIG_TEDGE_REMOTE_ACCESS_MDNS=n` and confirm the "not built in" reason

## 6. Docs

- [x] 6.1 Update the remote-access section of `tedge-zephyr/README.md`: `.local` endpoints, the four options (including retries), the link-local rule, and any application requirement found in task 1
- [x] 6.2 Add the measured per-board costs to the proposal's Resources note and to `DEVICES.md` if a board needed a tuned option
