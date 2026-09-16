## Context

The repo is a Zephyr multi-protocol workspace: `lib/common` owns connectivity and
a pluggable device data model (environment / pump simulations behind a Kconfig
`choice`), each `lib/<protocol>` is a frontend mapping that model onto a wire
protocol, and each `apps/<protocol>` composes the two. OPC-UA (TCP, open62541)
and Modbus TCP (Zephyr in-tree `modbus`) exist today.

This change adds SNMP — the dominant management protocol for network gear — as a
Phase-2 source, presenting the device as an industrial managed switch/router.
Two distinct interactions are required: a **pollable agent** (manager → device
`GET`/`GETNEXT`/`GETBULK` on UDP 161) and **unsolicited traps** (device →
manager on UDP 162). Unlike Modbus, Zephyr ships **no** SNMP stack, so the
frontend must implement SNMP itself. The tightest target is the ESP32-WROOM
(~500 KB SRAM shared with the Wi-Fi stack); UDP with small fixed buffers is the
right fit.

## Goals / Non-Goals

**Goals:**
- A working SNMPv2c agent that standard tools (`snmpget`, `snmpwalk`,
  `snmpbulkwalk`) can walk over a switch/router MIB view (system + interfaces).
- SNMPv2c traps (`coldStart`, `linkUp`, `linkDown`) delivered to a configured
  manager, observable with `snmptrapd`.
- A `sim_switch` simulation feeding both, keeping the frontend simulation-agnostic
  per the protocol-frontend contract.
- Fits the ESP32-WROOM alongside Wi-Fi; portable to `native_sim` for CI.

**Non-Goals:**
- SNMPv3 (USM/auth/priv), SET/writes, and full MIB-II compliance.
- A shippable custom enterprise MIB file, or real switching/routing.
- Phase 3 thin-edge.io / Cumulocity.

## Decisions

### D1: Implement a minimal SNMPv2c engine in-repo (no external stack)
Zephyr has no SNMP subsystem, and porting net-snmp is far too heavy for the MCU.
The frontend implements just enough BER/ASN.1 (INTEGER, OCTET STRING, OID, NULL,
Counter32, Gauge32, TimeTicks, IpAddress, SEQUENCE) and the v2c PDU shapes
(`GetRequest`, `GetNextRequest`, `GetBulkRequest`, `Response`, `SNMPv2-Trap`).
*Alternatives:* net-snmp (rejected: footprint, POSIX assumptions); lwIP's
`snmp_agent` (rejected: Zephyr uses its own IP stack, not lwIP). Scope is bounded
by supporting only the OIDs the MIB view needs.

### D2: MIB modeled as a static, lexicographically-ordered OID table
The agent holds a compile-time-sorted array of `{ oid, type, accessor }` leaf
descriptors (scalars and `ifTable` columns expanded per interface). `GET` does
binary search; `GETNEXT`/`GETBULK` return the next entry in table order — the
sorted table makes lexicographic ordering (SNMP's core walk invariant) trivial
and O(log n)/O(1) without a live OID tree. *Alternative:* a dynamic tree
(rejected: heap + complexity for a fixed MIB).

### D3: SNMPv2c community strings, read-only
A configurable read community (default `public`) gates the agent; a separate
trap community is sent in trap PDUs. All writable/SET paths are rejected
(`GetRequest` only for reads; SET yields no writable objects). Keeps the attack
surface and code small; v3 security is a separate future change.

### D4: `sim_switch` owns link/counter state; frontend observes and emits traps
The simulation exposes, through the shared data-model interface, per-interface
oper-status plus monotonic in/out octet and packet counters, and flaps link
state on a timer. The frontend samples on its own cadence: it maps values into
`ifTable` for polls, and on an observed oper-status transition it originates a
`linkUp`/`linkDown` trap. State lives in the simulation so switching firmware to
another simulation needs no frontend edit. *Alternative:* counters owned by the
frontend (rejected: violates the frontend contract — the frontend must not define
a competing data source).

### D5: One UDP context, request-scoped work buffer
A single bound UDP socket on 161 serves requests; traps are sent on an ephemeral
socket to the manager on 162. Each datagram is encoded/decoded in a single
stack/static work buffer (~1.5 KB) with a bounded interface count (≤ 8), so there
is no per-request heap growth. Oversized/unsupported requests get a proper SNMP
error-status response rather than a crash.

## Risks / Trade-offs

- **Hand-rolled BER is error-prone / could mis-encode** → keep the type set
  minimal, unit-test the codec on `native_sim`, and validate interoperability
  against real `snmpget`/`snmpwalk`/`snmpbulkwalk` and `snmptrapd`.
- **RAM pressure on the ESP32-WROOM alongside Wi-Fi** → fixed buffers, capped
  interface count, no dynamic allocation; measure free heap and document the
  minimum viable board profile (fall back to S2/PSRAM if it does not fit).
- **`GETBULK` with large `max-repetitions` could inflate a response past one
  datagram** → cap repetitions to what the work buffer holds and truncate the
  varbind list at the MIB end (`endOfMibView`), as the spec allows.
- **Trap delivery is best-effort (UDP, no inform/ack)** → acceptable for v2c
  traps; a manager can also poll current state. `Inform` is out of scope.
- **Counter32 wrap on fast simulated links** → expected SNMP behavior; managers
  compute deltas. Keep widths spec-correct (Counter32) rather than widening.

## Migration Plan

Purely additive: a new app, a new frontend library, and a new simulation behind
the existing Kconfig `choice`. No existing firmware, spec, or data model changes
behavior. Rollback is dropping `apps/snmp-agent` / `lib/snmp` and the
`sim_switch` choice entry; nothing else depends on them.

## Open Questions

- Interface count/speeds to model by default (e.g. 4×1G access + 1×1G uplink?)
  and whether to expose an admin-status column now (read-only) or defer.
- Trap manager address configuration: Kconfig-only for now, or also a runtime
  overlay like Wi-Fi credentials?
- Whether to include a tiny enterprise sub-tree (`sysObjectID` under a private
  arc) or point `sysObjectID` at a generic value for the simulated switch.
