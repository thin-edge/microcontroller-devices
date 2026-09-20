## Why

Remote access is the reason the direct transport exists: Cumulocity reaches
an SSH server, web UI or PLC **on the device's own network**, tunnelled by a
Wi-Fi microcontroller that costs a few euros. A thin-edge.io gateway cannot
offer this for its child devices, and a brownfield site gets remote access to
its machines with nothing else installed.

`c8y-direct-spikes` proved it on hardware (Spike F, unknowns U11 and U12):
SSH to a Raspberry Pi through an ESP32-C6 answers in 5.6 s, echoes in ~100 ms
and moves 40–56 KB/s, with the target policy, the session cap and the failure
reasons all behaving. `c8y-direct-core` then put the module's connection,
onboarding and operations in place. This change moves the feature itself out
of `apps/c8y-spike` and into `tedge-zephyr` (roadmap step P2).

## What Changes

- **`CONFIG_TEDGE_REMOTE_ACCESS` becomes real.** On `530,<serial>,<host>,
  <port>,<key>` the client checks the target policy, opens TCP to the target,
  opens `wss://<tenant>/service/remoteaccess/device/<key>` with
  `Sec-WebSocket-Protocol: binary` and a Bearer JWT, and bridges bytes both
  ways until either side closes or the session goes idle.
- **Target policy** (`TEDGE_REMOTE_ACCESS_TARGETS_*`, already in Kconfig): the
  device's own IPv4 subnets (default), an explicit allow-list, or the device
  itself. The application narrows it further through the
  `remote_access_allow` hook, which the public header already declares.
- **Session cap** (`TEDGE_REMOTE_ACCESS_MAX_SESSIONS`, default 1). A request
  over the cap is failed immediately with a reason naming the limit, rather
  than left to the UI's own timeout.
- **Audit and capacity:** `c8y_RemoteAccessOpened` / `c8y_RemoteAccessClosed`
  events naming the target and the bytes moved, and `tedge_RemoteAccess` twin
  data (limit, active sessions, free seats, policy, and optionally the open
  targets) published on connect and on every session change.
- **Operation lifecycle:** `501` when the request is accepted, `503` once the
  tunnel is up, `502` with the reason when it is refused or fails.
- **Telnet targets:** when the target port is 23, the bridge offers
  `WILL ECHO` / `WILL SGA` to the client, without which Cumulocity's web
  terminal shows no echo (a spike finding).
- **`apps/c8y-spike` keeps its own copy** until firmware update (P3) also
  lands; the module's implementation is the one that ships.

## Non-goals

- Firmware update, telemetry, logs, configuration and certificate renewal
  (P3–P7).
- A safe remote shell on the device itself (P10): Zephyr's `shell_telnet`
  binds every interface with no authentication, so this change bridges to
  **other hosts**, and to the device's own services only through the policy.
  A WebSocket-fed shell backend is its own change.
- Bulk-transfer tuning (P11) beyond the buffer size already in Kconfig.
- The classic ESP32 (WROOM) as a supported target for this feature (P13):
  it works but stalls, and the profile documents it as demo-grade.

## Resource constraints

From the spikes, on top of what the client already uses:

| Item | Cost |
|---|---|
| Text | ~15.7 KB (bridge, WebSocket client, policy) |
| TLS heap, MQTT + one tunnel | 86.2 KB peak / 69.2 KB connected (16 KB records); 53.4 KB / 36.4 KB with 8 KB records |
| Static RAM | one thread (8 KB stack in the spike), 2 × `TEDGE_REMOTE_ACCESS_BUF_SIZE` buffers, ~1 KB WebSocket scratch |
| Sockets | one TLS context and one TCP context per session, plus one HTTP request for the upgrade |

Boards: the ESP32-C6 (one tunnel; two with 8 KB records) and the ESP32-S3
with the TLS heap in PSRAM. The feature requires certificate authentication,
because the WebSocket needs a JWT.

## Capabilities

### New Capabilities

- `tedge-remote-access`: the tunnel itself — the operation, the policy, the
  WebSocket bridge, the session cap, the events and the twin data.

### Modified Capabilities

- `device-management-features`: the baseline already requires the forwarding
  behaviour, the policy, the cap and the `tedge_RemoteAccess` twin data. It
  is updated only where the spikes changed the picture: the failure must be
  reported as soon as the device refuses, and the WROOM is demo-grade.
- `tedge-client-module`: the `remote_access_allow` hook stops being a
  declaration and becomes a requirement with a scenario.

## Impact

- `tedge-zephyr/src/`: new `tedge_remote_access.c` (bridge and policy) and
  its dispatcher entry in `tedge_c8y.c`; `TEDGE_HTTP` and the WebSocket
  client selected by the feature.
- Zephyr facilities: `websocket_client` (which needs `PSA_WANT_ALG_SHA_1`,
  a spike finding), `http_client`, sockets, `tls_credentials`.
- Profiles: `full.conf` and `remote-access-enabler.conf` enable the feature;
  the enabler profile is what makes a bare WROOM or C6 useful.
- Cumulocity: the `cloud-remote-access` microservice, and PASSTHROUGH or
  TELNET configurations on the device.
- Carries the spike's open problems P10 (safe local shell), P11 (throughput)
  and P13 (classic ESP32 stalls).
