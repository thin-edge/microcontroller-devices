## Context

`bridge()` in `tedge-zephyr/src/tedge_remote_access.c` resolves the target
on the session's own thread: an IP literal goes straight to the policy; any
other string goes to `zsock_getaddrinfo(host, NULL, AF_INET)`. Zephyr's
resolver sends a name ending in `.local` to the mDNS server entry
(224.0.0.251:5353) only when `CONFIG_MDNS_RESOLVER` is set. tedge-zephyr
selects `DNS_RESOLVER` (through `TEDGE_HTTP`) but nothing selects
`MDNS_RESOLVER`, so a `.local` target goes to the DHCP-supplied unicast DNS
server, which does not know it, and the session fails with
"cannot resolve".

The SNMP app already sets `CONFIG_MDNS_RESOLVER=y` together with
`CONFIG_MDNS_RESPONDER=y`, and its `.local` trap manager still fails with
`DNS_EAI_SYSTEM` (-11) on every retry, while IP literals work and the
responder answers for the device's own name. The cause has not been found.
So enabling the resolver is necessary but not known to be sufficient, and
every app here that builds tedge also runs the responder.

Constraints: the module must not depend on `lib/` or `apps/`; every
feature is a build-time choice; the C6/S3 `full` builds and the ota builds
with the `remote-access` extra are tight on RAM and on socket/poll slots
(`CONFIG_ZVFS_POLL_MAX`, `NET_MAX_CONTEXTS`).

## Goals / Non-Goals

**Goals:**
- A `.local` target resolves on every board that builds remote access,
  with or without the application's mDNS responder.
- Resolution time is bounded per session and the failure reason tells the
  operator whether the name timed out or failed.
- The audit trail names the address the session actually reached.
- A spoofed mDNS answer cannot steer a tunnel off the device's LAN.

**Non-Goals:**
- DNS-SD browsing, a name cache, IPv6, LLMNR (see the proposal).
- Fixing the SNMP trap manager; it may benefit from the root-cause fix
  but is not changed here.

## Decisions

### 1. Root-cause the `-11` before writing code

The first task reproduces the failure on a tedge build (S3 DevKitC, which
keeps a console and stays on the network — the C6 console resets on open
and the classic ESP32 boards stall) with `net dns <host>.local` from the
Zephyr shell, with and without the responder. Candidates, in the order
they are cheapest to rule out:

1. **The DHCP reconfigure drops the mDNS server.** DHCPv4 calls
   `dns_resolve_reconfigure()` with only its unicast servers; if the
   mDNS entry is not re-added, `.local` queries find no matching server
   and return `DNS_EAI_SYSTEM`. Check with `net dns` (lists servers)
   before and after the lease.
2. **The socket service / poll table is full.** In Zephyr 4.x the
   resolver's sockets register with the socket service (dispatcher) the
   responder also uses; the boot log `net_sock_svc: ... 3 poll entries`
   already shows the table is too small. A failed registration surfaces as
   a system error. Check by raising `CONFIG_ZVFS_POLL_MAX` and
   `CONFIG_NET_SOCKETS_SERVICE_*`/`DNS_RESOLVER_MAX_SERVERS` together.
3. **Port 5353 conflict or multicast not received.** The resolver's
   query socket and the responder both want 5353 / the 224.0.0.251 group;
   or the Wi-Fi driver drops multicast RX without IGMP membership. Check
   with a capture on the LAN (`tcpdump -i en0 port 5353`) to see whether
   the query leaves and the answer arrives.

The fix goes where the cause is: a `select`/`imply` or default in
tedge-zephyr's Kconfig if it is configuration; a documented requirement in
`tedge-zephyr/README.md` (and the board confs here) if only the
application can set it; a contained workaround in the module, with a
comment naming the Zephyr version, if it is a Zephyr bug. The finding is
recorded in `tedge-zephyr/docs/debugging-against-real-devices.md`.

*Alternative considered:* skip Zephyr's resolver and send our own one-shot
mDNS query on a raw UDP socket. Rejected unless the root cause is a Zephyr
bug that cannot be worked around: it duplicates packet parsing the
resolver already has and costs more flash.

**Finding (2026-10-07, S3 DevKitC, modbus-server `full`, Zephyr 4.4.2).**
Two Zephyr bugs, one behind the other:

1. *No mDNS server.* `dns_resolve_init_default()` adds 224.0.0.251:5353 to
   the default resolver only under `CONFIG_DNS_SERVER_IP_ADDRESSES`. With
   DNS from DHCP, `net dns` lists only the DHCP server; a `.local` query has
   no server, `dns_get_addr_info()` returns `-ENOENT` and `getaddrinfo()`
   reports `DNS_EAI_SYSTEM` (-11) — the SNMP trap manager's error. Setting
   `DNS_SERVER_IP_ADDRESSES=y` with empty servers puts mDNS in slot 0, and
   the resolver sends *every* non-`.local` name to the first server with a
   socket, so `pool.ntp.org` timed out and the client could not connect.
   Adding the mDNS server from the module behind the DHCP one works.
2. *Answers never read.* With the application's mDNS responder on 5353, the
   resolver's mDNS socket is paired with it by the DNS dispatcher and never
   bound or polled. The query leaves from an ephemeral port; a capture on
   the Pi shows the query and its answer (`A 192.168.68.84`, unicast back
   to that port within 0.1 ms, as RFC 6762 6.7 requires), and the device
   times out. No configuration fixes this while the responder runs, and
   every app here runs it.

So the fix is a contained workaround in the module (decision 3).

### 2. `CONFIG_TEDGE_REMOTE_ACCESS_MDNS`, default `y`

A bool under `TEDGE_REMOTE_ACCESS` that selects only `NET_UDP`: the module
sends its own query (decision 3), so Zephyr's `MDNS_RESOLVER` is not
needed. Default on, because finding LAN hosts
by name is what the remote-access-enabler role is for, and the cost is
expected to be small. A board without room turns it off; it then fails
`.local` targets with "mDNS is not built in" instead of asking the unicast
server — a clearer reason than "cannot resolve", and it avoids leaking
internal names to an upstream resolver.

*Alternative:* select `MDNS_RESOLVER` and lean on Zephyr's resolver.
Rejected after the finding above: it cannot receive answers while the
responder runs.

### 3. `.local` names: the module's own one-shot query; others: `dns_get_addr_info()`

A `.local` lookup opens a UDP socket, sends one A query for the name to
224.0.0.251:5353 from its ephemeral port, and reads the answers on that
socket until the per-lookup timeout, taking the first that answers its
random id for that name (`mdns_once()`; the packet builder and parser are
pure and unit-tested in `tedge_ra_target.c`). This is RFC 6762's "legacy
unicast" exchange: avahi and systemd-resolved answer it directly to the
sending port, so it works whether or not the application runs the
responder, joins no multicast group, and leaves Zephyr's resolver list
alone. Cost: one socket for the lookup's duration, a 512-byte buffer on the
session stack, about 1 KB of code.

*Alternatives:* add the mDNS server to Zephyr's resolver (works only
without the responder; tried); patch Zephyr's dispatcher to read the paired
socket (fixes SNMP too, but the module would need a patched Zephyr). The
dispatcher bug is worth reporting upstream; when a release fixes it this
can go back to the resolver.

For unicast names, `zsock_getaddrinfo()` uses the global `CONFIG_NET_SOCKETS_DNS_TIMEOUT` and
collapses all failures into a few `DNS_EAI_*` codes. The bridge calls the
resolver API on the default context instead, waiting on a semaphore with
`CONFIG_TEDGE_REMOTE_ACCESS_RESOLVE_TIMEOUT_MS` per lookup (default 5000;
mDNS hosts normally answer in well under a second, but a host waking from
Wi-Fi power save, or a busy Pi, can take seconds, and a generous window
means fewer retries are spent on a slow answer).
This gives a per-feature bound and distinguishes a timeout
(`DNS_EAI_CANCELED`) from other errors for the reason text. It applies to
unicast names too, so both kinds of name share one path. Resolution stays
on the session thread, never the client thread that owns MQTT, and the seat
is held only for the bounded time.

The resolver allows one query at a time by default
(`DNS_NUM_CONCUR_QUERIES=1`), so a lookup can find the slot taken by the
client's own (`-EAGAIN`); that maps to `DNS_EAI_SYSTEM` and is retried.

### 3a. Retry failed lookups a bounded number of times

A single mDNS query is one multicast UDP packet over Wi-Fi: no ARQ for
multicast, no retransmission by the AP, and a host in power save may miss
it. One lost packet should not fail a session the operator has to reopen
by hand. So `resolve_target()` loops: up to
`CONFIG_TEDGE_REMOTE_ACCESS_RESOLVE_ATTEMPTS` lookups (default 3, range
1–5), each bounded by the per-lookup timeout, with
`CONFIG_TEDGE_REMOTE_ACCESS_RESOLVE_RETRY_DELAY_MS` (default 500) between
them, and returns on the first answer.

What is retried and what is not:

| Outcome of a lookup | Retry? |
|---|---|
| Timeout (`DNS_EAI_CANCELED`) | yes |
| Transient error (`DNS_EAI_AGAIN`, `DNS_EAI_SYSTEM`, `-ENOMEM`, no route yet) | yes |
| Authoritative "no such name" from a unicast server (`DNS_EAI_NONAME`) | no — asking again gives the same answer |
| `.local` without mDNS built in | no — not a lookup at all |
| Policy refusal of the resolved address | no — resolution succeeded |

Each failed attempt logs at WRN with the attempt number and the error
(never the connection key); only the final failure is posted as the
operation's reason, naming the attempt count. The loop runs on the
session thread and holds the seat while it runs, so the worst case is
bounded: attempts × timeout + (attempts − 1) × delay ≈ 16 s with the
defaults (3 × 5 s + 2 × 0.5 s). Task 5 checks that the Cumulocity
remote-access client waits that long for the device-side WebSocket; if it
does not, lower the attempts rather than the per-lookup timeout. A Kconfig `BUILD_ASSERT`
keeps that product under 30 s so a misconfiguration cannot hold a seat
indefinitely.

The same loop serves unicast names, which mostly benefit from the
`DNS_EAI_AGAIN` case (a resolver that was still being configured after a
reconnect).

*Alternatives:* rely on Zephyr's own retransmission inside one
`dns_resolve_name()` call — it retransmits only within one timeout window
and does not cover a socket-level error; or retry at the operation level
by failing and letting the operator reopen — that is exactly the
experience this avoids. Exponential backoff was considered and rejected:
with three attempts on a LAN, a fixed short pause is simpler and no
slower to recover.

### 4. A `.local` answer must be on the link

mDNS is unauthenticated: any host on the link can answer for any name. The
LAN policy already rejects an off-subnet address. The allow-list policy
matches names only, so `policy_check()` gains one extra rule: when the
host ends in `.local`, the resolved address must also be loopback, the
device's own, or on the device's subnets, whatever the policy. This keeps
"listing `rpi5.local:22`" meaning "the Pi on this LAN" and costs one
existing subnet check. The application hook still runs after and still
receives the resolved `sockaddr`, so the public API is unchanged.

### 5. Event and log text carry both forms

`"tunnel to rpi5.local (192.168.68.72):22 opened"` and the matching close
event; an IP-literal target keeps today's text. With a 63-character host,
an address and the close event's counters, the close text can exceed
`tedge_ra_event.text[144]` (about 192 characters at the extreme). The
event text grows to 200 bytes, and the client's quoting buffers with it
(about 112 B more RAM per queued event pair and on the client stack); a
unit test checks the longest case fits. The resolved address is stored in
the session (`char addr_s[NET_IPV4_ADDR_LEN]`). The twin data's session list keeps the host as requested.

## Risks / Trade-offs

- [Our own mDNS client diverges from Zephyr's] → it handles only A
  answers to its own legacy-unicast query; the parser is unit-tested
  against malformed and looping input. Revisit when Zephyr's dispatcher is
  fixed.
- [One more socket while a name resolves, on boards near their limits]
  → measure on the C6, S3 and QT Py `full` builds and the WROOM/ESP32-CAM ota
  builds; raise `ZVFS_POLL_MAX`/contexts in tedge-zephyr's defaults only if
  the measurement shows a shortfall.
- [Two hosts claim the same `.local` name] → the first answer wins; the
  event shows which address was reached, so the operator can tell.
- [Seat held while waiting for a dead name] → bounded by attempts ×
  timeout + delays (about 16 s with the defaults, asserted under 30 s at
  build time), and the reason names the timeout and the attempt count.
- [Retries hide a flaky network] → each failed attempt is logged at WRN,
  so a target that only resolves on the third try shows up in the logs
  even though the session succeeds.
- [Classic-ESP32 multicast reception is unreliable] → test there, but
  record the result as board-specific rather than block on it (those
  boards already have known Wi-Fi stalls).

## Migration Plan

No data or API migration. Existing endpoints with IP literals behave as
before. Rollback: set `CONFIG_TEDGE_REMOTE_ACCESS_MDNS=n`.

## Open Questions

- None blocking. Answered: the `-11` is "no mDNS server" (see the finding
  under decision 1), and the resolver callback API is usable from the
  module for unicast names.
- Follow-ups outside this change: report the dispatcher bug upstream; the
  SNMP trap manager can reuse `mdns_once()`'s approach.
