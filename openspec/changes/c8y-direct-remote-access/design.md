## Context

Spike F (archived in `2026-09-19-c8y-direct-spikes`) answered how Cumulocity
Cloud Remote Access works from the device's side, and proved a bridge on the
ESP32-C6 and ESP32-S3. Its code is `apps/c8y-spike/src/spike_ra.c`: one file
with globals, measurement logging and a message queue back to the MQTT
thread.

`c8y-direct-core` gives this change what it needs: the client thread, the
SmartREST dispatcher, a maintained JWT (`tedge_c8y_jwt()`), the twin store
that republishes after a reconnect, and the operation helpers.

Facts from the spike that shape the design:
- `530,<serial>,<host>,<port>,<connectionKey>` arrives on `s/ds`; the key is
  36 characters and must never be logged.
- The WebSocket is `wss://<tenant>/service/remoteaccess/device/<key>` with
  `Sec-WebSocket-Protocol: binary`. **A client certificate alone gets 401**;
  a Bearer JWT works.
- Zephyr's `websocket_connect()` hashes `Sec-WebSocket-Accept` with PSA
  SHA-1 but does not select it: without `PSA_WANT_ALG_SHA_1` it fails with
  `-EPROTO` before sending anything.
- MQTT + one tunnel: 86.2 KB TLS heap peak, 69.2 KB connected (16 KB
  records), steady through 10 MB each way.
- Throughput 40–56 KB/s, echo ~100 ms, first command 5.6 s.
- A refused session is reported immediately, but Cumulocity's web terminal
  only shows it after its own timeout, so the reason has to be on the
  operation.
- Port 23 needs the bridge to offer `WILL ECHO` / `WILL SGA`.

## Goals / Non-Goals

**Goals:** the feature in the module, behind `CONFIG_TEDGE_REMOTE_ACCESS`,
with the policy, cap, events and twin data the baseline spec already
requires; no regression in the client's other behaviour; the same measured
throughput and latency as the spike.

**Non-Goals:** a safe device-local shell (P10), throughput work (P11), the
classic ESP32's stalls (P13), and more than one session on boards that
cannot afford it.

## Decisions

### D1: One bridge thread per session, owned by the feature

The bridge blocks on `zsock_poll()` of two sockets, so it cannot run on the
client thread (which owns the MQTT session). Each session gets a thread from
a small pool sized by `TEDGE_REMOTE_ACCESS_MAX_SESSIONS`, with
`TEDGE_REMOTE_ACCESS_STACK_SIZE` (default 4096; the spike measured its 8 KB
stack as comfortable, and the module's buffers are static rather than on the
stack).

Results travel back to the client thread through a `k_msgq`, exactly as in
the spike, because the MQTT client is not thread-safe. The client thread
turns them into `501`/`503`/`502`, the events and the twin update.

*Alternative: run the bridge on the client thread with a poll set that also
holds the MQTT socket.* It would save a thread and a queue, but one slow
target would stall the MQTT session, and the session cap would become a poll
set that grows with it.

### D2: The policy decides before any socket is opened

Order: parse the message, resolve the host, check the policy, check the cap,
then connect. A target outside the policy never sees a TCP SYN. The
application's `remote_access_allow` hook is called after the built-in policy
passes, never instead of it, so an application can only narrow the policy.

Resolution uses the DNS resolver the client already needs; a name that
resolves outside the policy is refused like a literal address.

### D3: The seat is taken when the request is accepted

`tedge_RemoteAccess` carries `maxSessions`, `activeSessions` (tunnels that
are up), `freeSessions` (seats a new request could take) and `policy`, plus
`sessions` with target and start time when
`TEDGE_REMOTE_ACCESS_TWIN_SESSIONS` is on. A seat counts as taken from the
accepted `530` until its thread has cleaned up, so two requests in quick
succession cannot both pass the cap.

The twin is published through the core's twin store, so it is republished
after every reconnect with no stale session left behind.

### D4: Bytes move in fixed buffers, one WebSocket frame per read

Two static buffers of `TEDGE_REMOTE_ACCESS_BUF_SIZE` (default 2048) per
session: target → WebSocket and back. Each TCP read becomes one masked
WebSocket frame, as in the spike. This is what limits throughput to
40–56 KB/s (P11); larger buffers and batching are the tuning work, and the
Kconfig option already lets a board with RAM to spare raise it.

The session ends when either side closes, on an error, or after
`TEDGE_REMOTE_ACCESS_IDLE_TIMEOUT_S` with no traffic. Every end publishes the
close event with the reason and the byte counts.

### D5: Telnet negotiation for port 23

When the target port is 23, the bridge sends `IAC WILL ECHO, IAC WILL SGA`
to the client as soon as the tunnel is up. Without it, Cumulocity's web
terminal and Zephyr's telnet backend both wait for the other to offer, and
nothing echoes. This is behaviour of the bridge, not of the target, so it
belongs in the module; it is skipped for other ports.

### D6: The feature's Kconfig dependencies

`TEDGE_REMOTE_ACCESS` already depends on `TEDGE_TRANSPORT_C8Y` and
`TEDGE_AUTH_C8Y_CA` (the JWT). It now also selects `TEDGE_HTTP`,
`WEBSOCKET_CLIENT` and `PSA_WANT_ALG_SHA_1`, and raises
`NET_SOCKETS_TLS_MAX_CONTEXTS` requirements in the README: one per session on
top of the MQTT session.

## Risks / Trade-offs

- [A slow or hostile target holds a seat] → the idle timeout ends it, and the
  cap means it can never take more than its seat.
- [The connection key could leak through logs] → it is never logged at any
  level; the spike's rule is kept and the unit test checks the log line.
- [TLS heap for a second session] → the profiles size it; a handshake that
  runs out fails the operation with a reason instead of dropping MQTT.
- [The application narrows the policy badly] → the hook can only refuse, not
  allow, so the worst case is a feature that never connects.
- [Throughput stays at 40–56 KB/s] → documented, with the buffer option as
  the lever; bulk transfer is not what this feature is for.

## Migration Plan

- `apps/c8y-spike` keeps `spike_ra.c` until P3 lands; the two implementations
  do not share code.
- Devices already using the spike need no migration: the operation, the
  events and the twin shape are the same.
- Boards without the RAM for a second TLS session keep
  `CONFIG_TEDGE_REMOTE_ACCESS=n`, which is the default.

## Open Questions

- Should a second session be allowed by default on the C6 with 8 KB records
  (the spike showed it fits), or stay at one until it is measured under load?
- Does the reference Smart Function present `tedge_RemoteAccess` as a
  fragment on the managed object, a measurement, or both (P12)?
