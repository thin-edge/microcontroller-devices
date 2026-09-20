## Context

SmartREST's `501`/`502`/`503` name a fragment, and Cumulocity applies them
to the oldest operation in the matching state. The device's own view of
which operation it is running never reaches the platform, so the mapping
from "what the device did" to "which operation was updated" is a guess that
is usually right and occasionally, permanently, wrong.

Templates `504`, `505` and `506` take an operation id instead. The only
thing missing is the id itself: static templates on `s/ds` do not carry it.
Cumulocity's documented ways to learn it are the Device Control REST API, a
**JSON subscription** on `devicecontrol/notifications`, or a SmartREST 2.0
response template collection. The JSON subscription needs no setup, no
tenant-wide state and no version to keep in step with the firmware, so that
is the one this client takes.

## Goals / Non-Goals

**Goals:** every status names the operation it belongs to; a stale operation
from an earlier boot cannot absorb a later result; a restart is completed by
id; nothing else about the features changes.

**Non-Goals:** custom request templates, JSON operation delivery, concurrent
operations.

## Decisions

### D1: Operations arrive as JSON, because that is where the id is

The client subscribes to `devicecontrol/notifications` and stops
subscribing to `s/ds`: one delivery of each operation, and the JSON carries
`id`.

*Alternative: a SmartREST 2.0 response-template collection.* It also carries
the id, but it is tenant-wide state with a version in its name, registered
and checked at every connect, and a device running older firmware breaks if
anyone edits it. The JSON is delivered with no setup at all.

*Alternative: the Device Control REST API.* Rejected: an HTTPS round trip
per operation, on a device with one spare TLS session.

### D2: The JSON becomes the line the handlers already parse

Each operation is turned into the same comma-separated line the static
templates delivered, **with the id where the device serial used to be**:

| Operation | Line built from the JSON |
|---|---|
| `c8y_Restart` | `510,<id>` |
| `c8y_Command` | `511,<id>,<text>` |
| `c8y_Firmware` | `515,<id>,<name>,<version>,<url>` |
| `c8y_LogfileRequest` | `522,<id>,<logFile>,<dateFrom>,<dateTo>,<searchText>,<maximumLines>` |
| `c8y_RemoteAccessConnect` | `530,<id>,<hostname>,<port>,<connectionKey>` |

Every feature's parser keeps working unchanged, because the serial it
ignored is now an id it still ignores. Only the dispatcher reads the id, and
only the status messages change.

Fields are quoted as SmartREST requires, so a command containing a comma
survives the trip.

### D3: The client reads the fields it knows, not arbitrary JSON

An operation's JSON is read with the same small helpers used for twin data:
find the fragment by name, then read the values inside it. A value may be a
number (`port`, `maximumLines`), so `tedge_json_value()` joins the existing
string reader. This is not a parser and does not pretend to be one — it
reads the fields of the five operations this client supports.

The payload buffer grows to `CONFIG_TEDGE_C8Y_PAYLOAD_BYTES` (2048 with this
feature on), because an operation's JSON carries a delivery log alongside
its own fields. A payload that still does not fit is dropped with a warning,
as today.

**Nothing of the payload is logged.** A remote-access operation carries a
connection key; only the fragment name and the id go in the log.

### D4: Every status is by id

`504,<id>` to start, `506,<id>[,<result>]` to succeed, `505,<id>,<reason>`
to fail, including the path that refuses an operation whose feature is not
built in. When no id is known — the static fallback — the client publishes
`501`/`502`/`503` exactly as before.

### D5: Operations that outlive the session keep their id in settings

A restart and a firmware update finish after a reboot, so their ids are
stored beside the markers those features already keep, and the operation is
completed by id when the device comes back. Everything else finishes inside
the session, where the queue guarantees there is exactly one operation in
flight and one id to remember.

### D6: The queue stays, for a different reason

With ids, two operations executing at once would be reported correctly. They
still are not run at once: a second file transfer or tunnel needs a second
TLS session and another thread, and `CONFIG_NET_SOCKETS_TLS_MAX_CONTEXTS` on
these boards is 2, one of which is MQTT. The queue is now a resource bound,
and the code says so.

### D7: A way back

`CONFIG_TEDGE_C8Y_OPERATION_JSON` (default y) can be turned off, which
restores `s/ds` and `501`/`502`/`503` for a tenant or a transport where the
notification topic is not available. Both paths are exercised by the Kconfig
cases.

## Risks / Trade-offs

- [The notification topic might not be served on the MQTT Service] → the
  first task is to prove it on hardware, on both ports, before the rest is
  written; the static path stays as the way back.
- [An operation's JSON is bigger than the payload buffer] → the buffer grows
  with the feature, and an oversized payload is dropped with a warning
  naming the size, as it is today.
- [Reading fields by name could pick the wrong one] → the search starts at
  the fragment, not at the top of the document, and the fields are read from
  inside it.
- [A connection key in a log] → the payload is never logged; only the
  fragment name and the id are.

## Results (ESP32-C6, Modbus application, 2026-09-20)

**Cost**, the same image with the feature on and off:

| Build | text | bss |
|---|---|---|
| Static templates (`s/ds`, 501–503) | 984,040 | 434,584 |
| Operations as JSON (504–506) | 985,192 (+1,152) | 434,808 (+224) |

The payload buffer doubles to 2 KB with the feature on, and the SmartREST
line building it replaces goes away, so the net RAM cost is a few hundred
bytes.

**Verified on hardware**, against the tenant, on the MQTT Service:

| Check | Result |
|---|---|
| An operation arrives with its id | `operation c8y_Command (214925)`, and the command ran |
| A stale EXECUTING operation | left EXECUTING, while the operation that followed got its own result — the failure this change exists to fix |
| Two operations created together | a log upload and a command, each with its own result |
| Restart | completed for the operation that asked, after the reboot |
| Firmware update | 0.2.0 → 0.3.0, completed by the id kept in settings across the swap |
| Remote access | failed with its reason, by id, rather than hanging |
| Log upload, shell command | unchanged, now reported by id |
| The static path (feature off) | still works: a command ran and was reported the old way |

### What the hardware taught us

**The notification topic is served on the MQTT Service too**, which was the
open question this change started with: the same `devicecontrol/notifications`
subscription works on 9883 as on 8883, so no transport needs the fallback
today.

**The queue is no longer load-bearing.** Two operations created together now
end correctly whichever order they finish in; the queue stays because a
second file transfer or tunnel needs a TLS session this board does not have
spare, which is a resource decision the code now states as such.


## Migration Plan

- Devices already in the field keep working: they use static templates until
  they are updated, and the two paths can coexist in a tenant.
- Stale EXECUTING operations left by earlier firmware stay stale; they no
  longer steal results, but an operator still has to clear them once.
