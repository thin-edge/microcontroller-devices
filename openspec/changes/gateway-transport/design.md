## Context

The client was built with two transports in mind from the start: the direct
one talks SmartREST to Cumulocity, and everything that is not
Cumulocity-specific already goes out as thin-edge.io `te/` messages. The
seam exists (`struct tedge_transport`: connect, poll, disconnect,
publish_twin) and the direct transport lives behind it. This change puts a
second one there and finds out what the seam was missing.

What the gateway does that the device then does not have to: TLS, the
device certificate and its renewal, the JWT, SNTP, HTTPS, and every
Cumulocity-specific encoding. What the device keeps: its identity, its
telemetry, its operations, and the hooks its application already uses.

## Goals / Non-Goals

**Goals:** a device on a plant network with no route to the internet gets
the same device management through its gateway; an application changes
nothing but a Kconfig line; an image without TLS fits a board that could not
hold one.

**Non-Goals:** both transports at once, TLS to the gateway, changing the
features, inventing thin-edge.io equivalents for Cumulocity-only operations.

## Decisions

### D1: A second transport behind the interface that exists

`tedge_gateway.c` implements `struct tedge_transport`. The core's state
machine, its heap, its settings subtree, the telemetry buffer, the
operation queue and every feature stay as they are. Anything the command
path needs that the interface cannot express is a change to the interface,
kept as small as the evidence demands — and the fact that the direct
transport must keep working unchanged is the test of whether it stayed
small.

### D2: The device is a child device, registered by itself

The device publishes its own registration and then uses the child-device
topic prefix for everything:

```
te/device/<id>//                     (registration: name, type)
te/device/<id>///m/<type>            measurements
te/device/<id>///e/<type>            events
te/device/<id>///a/<type>            alarms
te/device/<id>///twin/<fragment>     twin data
te/device/<id>/service/tedge-zephyr/status/health
```

Which is what the client publishes today, with the prefix it already builds.
The gateway maps them to the cloud, as it does for its own children.

### D3: Operations become commands, with the features untouched

thin-edge.io drives an operation as a command with a request id, a payload
and a status that both sides move along:

```
te/device/<id>///cmd/restart/<request-id>          {"status":"init"}
te/device/<id>///cmd/firmware_update/<request-id>  {"status":"init","name":…,"version":…,"url":…}
te/device/<id>///cmd/log_upload/<request-id>       {"status":"init","type":…,"tedgeUrl":…,…}
```

The device answers on the same topic by republishing with
`"status":"executing"`, then `"successful"` or `"failed"` with a reason. A
device also advertises which commands it supports by publishing an empty
retained message on the command's topic at startup — the thin-edge.io way of
saying "I can do this".

The transport turns a command into the same internal request each feature
already takes, and turns the feature's result back into a status. The
features do not learn that a gateway exists.

*Alternative: teach each feature both protocols.* Rejected: five features
would each grow a second protocol, and the seam exists precisely so they do
not have to.

### D4: File transfer is plain HTTP to the gateway

The gateway runs a file-transfer service on its own network
(`http://<gateway>:8000/te/v1/files/…`). Firmware downloads and log uploads
use the HTTP client the client already has, without TLS and without a JWT,
which is most of what made those paths expensive.

`CONFIG_TEDGE_HTTP` keeps its meaning; what changes is the scheme, the
credentials (none) and the host (the gateway).

### D5: The gateway is discovered, not configured

mDNS (`_thin-edge_mqtt._tcp`) finds the gateway on the local network, with
`CONFIG_TEDGE_GATEWAY_HOST` as the fallback for a network without mDNS. A
device that is moved to another gateway needs no reconfiguration, which is
the point of discovery on a plant network.

### D6: Features that have no equivalent are not selectable

Remote access and certificate renewal are Cumulocity-specific: the first has
no thin-edge.io child-device equivalent, and the second is the gateway's job
because the certificate is the gateway's. Both depend on
`TEDGE_TRANSPORT_C8Y` in Kconfig, so a gateway build cannot select them, and
the Kconfig help says why rather than leaving an integrator to discover it.

### D7: No TLS in the image is the point

A gateway build drops mbedTLS, the certificate, the enrollment and the JWT.
The footprint table gains the row that justifies the whole change: what a
WROOM can hold with a protocol server *and* device management.

## Risks / Trade-offs

- [The command protocol may not fit the features' internal requests] → the
  first task is one command end to end (restart, the simplest) before the
  rest is written.
- [An unauthenticated local broker] → stated as a non-goal with the reason:
  the gateway owns the trust boundary. A plant network that needs more needs
  the gateway's TLS, and that is a later change.
- [Two transports to keep working] → the direct transport's hardware
  verification is re-run after this change, and the Kconfig cases cover both.
- [mDNS on a quiet network] → the configured fallback, and a log line saying
  which of the two was used.

## Migration Plan

- Existing devices are direct-transport builds and are untouched.
- An application moves by changing the transport choice and dropping the
  Cumulocity-specific configuration; its own code does not change.

## Open Questions

- Which thin-edge.io command names and payloads the gateway version in use
  actually serves, and which of them the gateway maps to Cumulocity
  operations today.
- Whether the device should also accept its identity from the gateway
  (thin-edge.io can name a child device), or keep the application's identity
  as the only source.
- Whether telemetry should keep using the client's own buffer or lean on the
  gateway being one hop away and always reachable.
