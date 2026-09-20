## Why

Everything the client does today it does by talking to Cumulocity itself:
its own TLS, its own certificate, its own clock, its own JWT, its own
SmartREST. That is the right answer for a device on a network with a route
to the internet, and the wrong one for the devices this repository started
with — an OPC-UA server on an ESP32-WROOM-32 deep in a plant network, next
to a thin-edge.io gateway that already has all of that.

Connecting through the gateway instead makes the device dramatically
cheaper: **no TLS session, no certificate, no enrollment, no JWT, no SNTP,
no HTTPS** — the gateway owns every one of those. What is left is plain
MQTT to a broker on the local network, in thin-edge.io's own JSON, which is
the shape this client already publishes: its telemetry, twin and health
messages are `te/` messages today because of this step.

It is also what makes the WROOM interesting again. Today it can only be a
remote-access enabler, because a TLS session plus Wi-Fi plus a protocol
server does not fit. Without TLS it can carry a protocol server *and*
device management.

This is phase 3, roadmap step P8 — the last one — and the first change that
touches the transport seam (`struct tedge_transport`) rather than a feature.

## What Changes

- **`CONFIG_TEDGE_TRANSPORT_GATEWAY` becomes real**: a second transport
  behind the interface the direct one already sits behind, chosen at build
  time.
- **The device registers itself as a child device** of the gateway and
  publishes its telemetry, twin data and health on the `te/` topics it
  already uses — with the gateway's topic prefix rather than Cumulocity's.
- **Operations become thin-edge.io commands.** Where the direct transport
  receives SmartREST and answers `504`/`505`/`506`, the gateway transport
  receives a command on `te/device/<id>///cmd/<command>/<request-id>` and
  moves it through thin-edge.io's states (`init` → `executing` →
  `successful` / `failed`). The features themselves do not change: restart,
  firmware update, log upload and parameters keep their handlers.
- **File transfer goes to the gateway's file-transfer service** over plain
  HTTP instead of HTTPS to Cumulocity, so firmware downloads and log uploads
  keep working without TLS.
- **The gateway is found with mDNS** (`_thin-edge_mqtt._tcp`), with a
  configured address as the fallback, so a device does not need to be told
  where its gateway is.
- **What the gateway cannot do stays direct-only.** Remote access into the
  device's LAN is the headline feature of this client and thin-edge.io has
  no child-device equivalent; the Kconfig says so rather than pretending.

## Non-goals

- Running both transports at once. A device belongs to a gateway or to
  Cumulocity, chosen at build time.
- Certificate-authenticated MQTT to the gateway. The local broker is on the
  plant network; if that network is untrusted, the gateway's own TLS
  configuration is the place to fix it, and that is a later change.
- Re-implementing the features. If a feature needs changing to work through
  a gateway, that is a finding for this change, not a rewrite.
- Cumulocity-specific operations that have no thin-edge.io equivalent
  (remote access, certificate renewal): they are unavailable on this
  transport, and Kconfig enforces it.

## Resource constraints

| Item | Cost |
|---|---|
| Text | the second transport: the command topics, their state machine, and the plain-HTTP file transfer |
| RAM | **far less than the direct transport**: no TLS session (35–52 KB), no mbedTLS heap, no certificate, no JWT |
| Flash | mDNS discovery, minus everything TLS the image no longer needs |
| Network | local, plain MQTT; the gateway does the talking to the cloud |

## Capabilities

### New Capabilities

- `tedge-gateway-transport`: how a device finds its gateway, registers as a
  child device, and carries out commands through it.

### Modified Capabilities

- `device-management-features`: which features are available on which
  transport, and what a build does when a feature has no equivalent.
- `tedge-client-module`: the application's integration is unchanged by the
  transport — the same calls, the same hooks, a different cloud path.

## Impact

- `tedge-zephyr/src/`: a new `tedge_gateway.c` (session, registration,
  commands) and `tedge_gateway_http.c` (plain-HTTP transfer), plus mDNS
  discovery; `tedge_c8y.c` is untouched.
- The transport interface gains whatever the command path needs that the
  SmartREST one did not — the first task is to find out what, and to keep it
  small.
- Kconfig: the gateway transport stops depending on
  `TEDGE_EXPERIMENTAL_FEATURES`; the features that cannot work through a
  gateway depend on the direct transport.
- A WROOM profile that was impossible before: protocol server plus device
  management, with no TLS in the image.
- The repository gains a gateway to test against (a thin-edge.io instance on
  the Raspberry Pi that already hosts the test targets).
