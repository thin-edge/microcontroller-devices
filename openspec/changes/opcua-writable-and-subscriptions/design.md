## Context

The Phase 1 server is read-only and built with `UA_ENABLE_SUBSCRIPTIONS=OFF`; all
nodes are `CurrentRead`. open62541 supports both subscriptions and writes; the
work is re-enabling the feature in the vendored amalgamation and adding writable
nodes with a write path — within tight MCU RAM (ESP32-WROOM ≈ 68 KB free heap
after Wi-Fi; ESP32-S2 has more; open62541 already uses an 8 KB shared RX buffer,
MINIMAL namespace, single-threaded).

## Goals / Non-Goals

**Goals:**
- Clients can create subscriptions + monitored items and receive value-change
  notifications for the measurement nodes.
- Clients can write a `setpoint` (Double) and an `enabled` (Boolean); the
  firmware observes the new values and they read back.
- Stay within the WROOM RAM budget, or clearly gate subscriptions to boards that
  fit; writes must work on all boards.

**Non-Goals:**
- Methods, history, events/alarms, PubSub, secured writes, persistence,
  real actuation.

## Decisions

### Decision: Re-enable subscriptions via the regen script, with caps
Add `-DUA_ENABLE_SUBSCRIPTIONS=ON` to `scripts/regen-open62541.sh` and
regenerate the amalgamation. Cap resources through the server config
(`maxSubscriptions`, `maxMonitoredItemsPerSubscription`, publish/notification
limits) and Kconfig-exposed sampling bounds, so a client cannot exhaust RAM.

- **Alternative**: keep polling only — rejected; the user needs change-driven
  updates. Subscriptions are the standard OPC-UA mechanism.
- **RAM gating**: if the WROOM cannot fit subscriptions after capping, gate
  `UA_ENABLE_SUBSCRIPTIONS` behind a Kconfig/board profile so the WROOM keeps a
  read+write server and the S2 (PSRAM) enables subscriptions. Decide by
  measuring, not assuming.

### Decision: Writable nodes with a write value-callback
Add two nodes under the Device object: `setpoint` (Double) and `enabled`
(Boolean), each with `CurrentRead | CurrentWrite`. Register a
`UA_ValueCallback` (onWrite) so the firmware is notified when a client writes,
and mirror the written value into app state (logged; shown on the status
display where present). Values live in RAM (not persisted).

- **Alternative**: data-source-backed writes with a dedicated writer interface —
  deferred; a value-callback that updates app state is sufficient for now and
  keeps the data-source (read/sample) abstraction unchanged.

### Decision: Keep the existing measurements read-only
`temperature`/`humidity`/`pressure` stay `CurrentRead` (they are sampled inputs).
Only the new control points are writable, so the read model is unchanged and the
sampler keeps overwriting measurements.

## Risks / Trade-offs

- **[Subscriptions push WROOM over its RAM budget]** → Cap subscription/monitored
  counts and publish queue; measure free heap and `BadOutOfMemory` at startup;
  if it does not fit, gate subscriptions to the S2/PSRAM profile and keep the
  WROOM read+write only. Document the outcome.
- **[A client writes out-of-range setpoint]** → Clamp/validate in the write
  callback; reject with a bad status or clamp to a documented range.
- **[Write races with the sampler thread]** → The server runs single-threaded
  (`UA_MULTITHREADING=0`) and writes/callbacks run in the same server loop as the
  sampler, so no locking is required; keep it that way.
- **[Larger open62541 build]** → subscriptions add flash/RAM; acceptable given
  measured headroom, and gated if not.

## Open Questions

- Does the WROOM fit subscriptions after capping, or do we gate them to the S2?
- Sensible `setpoint` range/units and `enabled` semantics for the demo.
- Default monitored-item sampling interval vs the data-source sampling interval.
