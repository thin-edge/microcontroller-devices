## Why

The client reports every operation's progress with SmartREST's `501`, `502`
and `503`, which name a *fragment* and act on the **oldest operation in that
state**. The operation the device was actually working on is never named,
and that has consequences it cannot fix from inside:

- Two operations executing at once collect each other's results. The
  diagnostics change worked around this by running one at a time and
  queueing the rest (`c8y-direct-diagnostics`, D8).
- An operation interrupted by a reset stays EXECUTING in Cumulocity for
  ever, and **every later result completes that stale operation instead of
  the real one**. This was seen on hardware: after a crash mid-upload, the
  next three log requests each completed the previous stale operation and
  left themselves executing.
- A restart is completed after the reboot by publishing `503,c8y_Restart`
  and hoping the oldest restart operation is the one that caused it.

Cumulocity has the answer already: templates **504, 505 and 506** take an
**operation id** and act on exactly that operation. The device only needs to
know the id, which static templates do not carry — but a SmartREST 2.0
**response template collection** does, and registering one is a few hundred
bytes at connect.

This is phase 3 and touches every feature that has an operation (restart,
firmware update, remote access, log upload, shell command) on both
transports, on the same ESP32-C6 and ESP32-S3 boards.

## What Changes

- **The client receives operations as JSON** on
  `devicecontrol/notifications`, which is where Cumulocity puts the
  operation id, instead of the static templates on `s/ds`.
- **Every status goes out by id:** `504,<id>` to start, `506,<id>[,<result>]`
  to succeed, `505,<id>,<reason>` to fail. `501`/`502`/`503` disappear from
  the client.
- **A restart is completed by id after the reboot**: the id is stored beside
  the restart marker, so the right operation is closed even if another was
  queued meanwhile.
- **An operation's result can no longer be stolen** by a stale EXECUTING
  operation from an earlier boot, so a device that crashed mid-operation
  recovers by itself.
- **The one-at-a-time queue stays**, but for the honest reason: a second
  operation may want a second TLS session and a second thread, and a
  constrained board has neither. It is no longer load-bearing for
  correctness.
- **`CONFIG_TEDGE_C8Y_OPERATION_JSON`** (default y) can be turned off to go
  back to `s/ds` and the old templates.

## Non-goals

- A general JSON parser. The client reads the handful of fields an
  operation it supports actually has, with the same few hundred bytes of
  JSON reading it already uses for twin data and the identity lookup.
- Publishing as JSON: statuses stay SmartREST on `s/us`.
- Running several operations at once. The queue stays.

## Resource constraints

| Item | Cost |
|---|---|
| Text | reading a few JSON fields, turning them into the line the handlers already parse, and the id plumbing; `501`–`503` go away |
| RAM | a larger MQTT payload buffer (an operation's JSON carries a delivery log), and one operation id per operation in flight |
| Network | nothing extra: the same operations, in JSON rather than CSV |
| Flash | the stored restart id, beside the marker already stored |

## Capabilities

### Modified Capabilities

- `device-management-features`: an operation's status SHALL be reported for
  the operation that was requested, not for whichever one the cloud
  considers oldest.

## Impact

- `tedge-zephyr/src/tedge_c8y.c`: the subscription, the JSON-to-line
  translation, the id carried through dispatch, and `504`/`505`/`506`.
- `tedge_json.c`: reading a value that may be a number as well as a string.
- The restart marker gains the operation id.
- README and the Kconfig gain the new option and the reason for it.
