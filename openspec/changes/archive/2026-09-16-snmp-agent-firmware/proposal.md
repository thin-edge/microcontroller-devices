## Why

Network gear (switches, routers, firewalls) is overwhelmingly monitored over
**SNMP** — managers poll it and it pushes traps — yet the fleet so far only
speaks OPC-UA and Modbus. Adding an SNMP-agent firmware that presents itself as
an **industrial managed switch/router** gives us a realistic SNMP data source to
develop and demo collectors against, and extends the repo's protocol coverage
into the most common device-management protocol without needing real switch
hardware.

## What Changes

- **New `apps/snmp-agent` firmware** composing `lib/common` with a new SNMP
  frontend, defaulting to a new switch/router simulation. Built for
  `esp32_devkitc` (ESP32-WROOM), `native_sim` (dev/CI), and `adafruit_feather_esp32s2_tft`.
- **New `lib/snmp` frontend**: a minimal **SNMPv2c agent** (UDP port 161) that
  answers `GET`, `GETNEXT`, and `GETBULK` requests over a switch/router MIB view,
  and a **trap/notification sender** that emits SNMPv2c traps to a configured
  manager (UDP port 162). Zephyr has no in-tree SNMP stack, so the frontend
  implements the needed BER/ASN.1 encode/decode and PDU handling itself, sized
  for the MCU.
- **MIB view (polling endpoint)**: the standard **system group** (`sysDescr`,
  `sysObjectID`, `sysUpTime`, `sysContact`, `sysName`, `sysLocation`,
  `sysServices`) and a **MIB-II interfaces group** (`ifNumber` + an `ifTable`
  with per-port `ifIndex`, `ifDescr`, `ifType`, `ifOperStatus`/`ifAdminStatus`,
  `ifSpeed`, and in/out octet & packet counters), mapped from the simulation.
- **Traps (publish path)**: `coldStart` on boot and `linkUp`/`linkDown` as
  simulated ports change operational state, sent to a Kconfig-configured manager
  address/port with a configurable community.
- **New switch/router simulation** (`sim_switch`) in `lib/common`: a small set
  of interfaces with link state that flaps over time and monotonic traffic
  counters, exposed through the shared data-model interface so the SNMP frontend
  stays simulation-agnostic.

**Phase**: Phase 2 (additional industrial protocol as a source). **Protocol**:
SNMP (agent/notification-originator role, SNMPv2c). **Boards**: `esp32_devkitc`
(ESP32-WROOM), `adafruit_feather_esp32s2_tft` (ESP32-S2), `native_sim`.

### Non-goals

- **SNMPv3** (USM auth/privacy) — SNMPv2c community-based only for now.
- **SET requests** — the agent is read-only; no writable OIDs this change.
- **Full standard MIB compliance** — a representative, browsable subset of the
  system and interfaces groups, not every MIB-II object or a shippable custom MIB
  file.
- Real switching/routing, or driving real network hardware — the data is
  simulated.
- Phase 3 thin-edge.io / Cumulocity connectivity — out of scope.

### Resource constraints

The agent runs over UDP (no TCP session state) and must fit alongside the Wi-Fi
stack on the ESP32-WROOM (~500 KB SRAM, tightest target). BER encode/decode must
use small fixed/stack buffers and a single datagram work buffer (target ≤ ~1.5 KB
per PDU) with a bounded interface-table size (e.g. ≤ 8 ports) — no dynamic
per-request heap growth, no filesystem, and counters kept in RAM only.

## Capabilities

### New Capabilities
- `snmp-agent`: SNMPv2c agent on UDP 161 that answers GET/GETNEXT/GETBULK over a
  switch/router MIB view (system group + MIB-II interfaces group), conforming to
  the protocol-frontend contract.
- `snmp-trap-notifications`: SNMPv2c trap/notification originator that sends
  `coldStart`, `linkUp`, and `linkDown` traps to a configured manager on UDP 162.

### Modified Capabilities
- `simulation-model`: add a switch/router simulation (`sim_switch`) exposing
  per-interface link status and traffic counters through the shared data-model
  interface, selectable per firmware like the existing environment/pump
  simulations.

## Impact

- **Code**: new `lib/snmp/` frontend (`snmp_agent.*` PDU/dispatch, ASN.1/BER
  helpers, MIB/OID mapping, trap sender) following `lib/frontend-template`; new
  `apps/snmp-agent/` app (`CMakeLists.txt`, `prj.conf`, `Kconfig`, `VERSION`,
  `boards/`, `points.d`); new `sim_switch` in `lib/common` behind the existing
  simulation `choice`.
- **Config**: new `lib/snmp/Kconfig` (agent port, read community, trap
  manager IP/port + community, interface count); app selects `sim_switch`.
- **Dependencies**: Zephyr networking/UDP sockets only; no external SNMP library
  (BER handling is implemented in-repo). No open62541/Modbus involvement.
- **Docs**: README/SCOPE updated to list SNMP as an available Phase-2 protocol
  and how to poll it (`snmpget`/`snmpwalk`) and receive traps (`snmptrapd`).
- **Systems**: still a LAN industrial-protocol source; no cloud/thin-edge.io.
