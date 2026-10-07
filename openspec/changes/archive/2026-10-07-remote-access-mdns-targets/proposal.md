## Why

Hosts on a small LAN (a Raspberry Pi running SSH, a PLC's web UI, another
tedge board) usually get their address from DHCP and are known by their
`<hostname>.local` mDNS name, not a fixed IP. Today a remote-access
connect for `rpi5-d83add9f145a.local:22` fails on the device with
"cannot resolve": the bridge hands the name to `getaddrinfo()`, but the
tedge-zephyr builds have no mDNS resolver, so only IP literals and names in
unicast DNS work. The operator has to look up the target's current IP and
edit the Cumulocity remote-access endpoint whenever its lease changes.

The same on-device `.local` lookup was already seen failing elsewhere in
this repo (the SNMP trap manager logs `not resolvable (-11)` with
`CONFIG_MDNS_RESOLVER=y`), so turning the resolver on is not enough by
itself: the cause of that failure has to be found and fixed too.

Phase 3 (tedge-zephyr). Protocol: none; this is the Cumulocity remote-access
feature. Boards: every build that includes `CONFIG_TEDGE_REMOTE_ACCESS` —
the `full` profile on the ESP32-C6-DevKitC-1, ESP32-S3-DevKitC-1 and QT Py
ESP32-S3, and the `remote-access` extra on the ota builds (see
`release/devices.yml`).

## What Changes

- A remote-access target named `<name>.local` is resolved over mDNS
  (multicast query on the device's LAN) before the policy check, and the
  session proceeds with the address it resolves to. Unicast DNS names and
  IP literals behave as before.
- New Kconfig option `CONFIG_TEDGE_REMOTE_ACCESS_MDNS` (default `y`,
  depends on `TEDGE_REMOTE_ACCESS`). The module sends its own one-shot mDNS
  query: Zephyr 4.4.2's resolver cannot receive mDNS answers while the
  application runs the mDNS responder (see design). Turning it off restores
  today's footprint for a board that has no room.
- New Kconfig option `CONFIG_TEDGE_REMOTE_ACCESS_RESOLVE_TIMEOUT_MS`
  bounding how long each lookup waits for an answer (default 5000).
- A lookup that times out or hits a transient error is retried, up to
  `CONFIG_TEDGE_REMOTE_ACCESS_RESOLVE_ATTEMPTS` lookups in all (default 3,
  `1` turns retrying off), with a short pause between them
  (`CONFIG_TEDGE_REMOTE_ACCESS_RESOLVE_RETRY_DELAY_MS`, default 500). mDNS
  runs over lossy Wi-Fi multicast, and a sleeping host or a dropped
  packet should not fail the session on the first try. This applies to
  unicast DNS names too. The worst case with the defaults is about 16 s.
- Resolution failures name the cause and the number of attempts in the
  failed operation: "`<name>` did not answer over mDNS after 3 attempts"
  (timeout) vs "cannot resolve `<name>` after 3 attempts" (any other
  error), and a `.local` target on a build without the
  option fails with "mDNS is not built in".
- The open and close events and the logs name both the host as requested and the address it resolved to
  (`rpi5.local (192.168.68.72):22`), so the audit trail says which machine
  was actually reached.
- Find and fix why an on-device `.local` lookup returns `DNS_EAI_SYSTEM`
  (-11). Found: Zephyr adds no mDNS server with DHCP-supplied DNS, and with
  the responder running never reads the answers; worked around in the
  module.

## Non-goals

- DNS-SD service browsing (finding "the SSH server" without a name). The
  operator still names the host.
- Caching resolved names across sessions. Each session resolves afresh,
  so a host whose lease changed is found at its new address.
- IPv6 targets or AAAA answers; the bridge and the policy are IPv4-only and
  stay so.
- LLMNR or NetBIOS name resolution.
- Changing the SNMP trap manager's resolution in `lib/snmp`. If the root
  cause found here also fixes it, that is a follow-up for the app.
- Changing the target policies themselves. A name is still judged by the
  address it resolves to and an allow-list entry still matches the host
  string as requested; the only addition is that a `.local` answer must be
  on the device's own subnets.

## Capabilities

### New Capabilities

_None._

### Modified Capabilities

- `tedge-remote-access`: the "targets are checked before any connection is
  opened" requirement gains mDNS resolution of `.local` names, bounded
  resolution time, distinct failure reasons, and a link-local check on any
  `.local` answer (mDNS is unauthenticated); the audit requirement gains the
  resolved address in the open and close events.

## Impact

- **Code**: `tedge-zephyr/src/tedge_remote_access.c` (resolution step,
  reasons, event text); `tedge-zephyr/Kconfig` (four new options);
  `tedge-zephyr/tests/kconfig/cases/` (new cases); unit tests for the
  `.local` detection and reason text.
- **Public `tedge_*` API**: no change. `struct tedge_remote_target` already
  carries the resolved `sockaddr` to the application's hook.
- **Dependencies**: none beyond `NET_UDP`; Zephyr's `MDNS_RESOLVER` is not
  used. Nothing from `lib/` or `apps/`.
- **Resources** (measured 2026-10-07, modbus-server `full`, mDNS on vs off):
  | Board | Flash | Static RAM |
  |---|---|---|
  | ESP32-C6-DevKitC-1 | +1,264 B content | 0 B |
  | QT Py ESP32-S3 | about +1 KB code (the query builder and parser are 613 B); the image total moves by under 20 B because of segment alignment | 0 B |

  At run time a `.local` lookup holds one UDP socket and a 512 B buffer on
  the session's stack, only while it runs; no board needed more sockets or
  poll entries. The whole change against the release baseline (retries,
  policy check, longer event text, mDNS), from the rebuilt baseline of the
  nine `full` builds: C6 +2,401 B content and +176 B RAM; S3 DevKitC and
  QT Py +207 to +225 B image and +168 B dram (their image size hides most
  code growth in segment alignment). The RAM is mostly the 200-byte event
  text. `release/size-baseline.json` is updated for those nine builds.
- **Docs**: `tedge-zephyr/README.md` remote-access section; `DEVICES.md`
  if a board needs a tuned option.
