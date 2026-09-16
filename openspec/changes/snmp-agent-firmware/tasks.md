## 1. Switch/router simulation (`lib/common`)

- [x] 1.1 Add a `sim_switch` entry to the simulation Kconfig `choice` in `lib/common/Kconfig`, with an interface-count option (default 5, max 8).
- [x] 1.2 Implement `sim_switch.c`/`.h` behind the shared data-model interface: a fixed interface table with per-interface admin/oper status, nominal speed, and in/out octet & packet counters.
- [x] 1.3 Implement the periodic step: advance counters only for up interfaces, and flap oper-status over time so transitions occur; keep all state in RAM.
- [x] 1.4 Expose accessors the frontend needs (interface count, per-index status/speed/counter reads) and wire `sim_switch` into the `lib/common` build so it compiles when selected.
- [x] 1.5 Build `native_sim` with `sim_switch` selected and log a step or two to confirm counters advance and links flap.
- [x] 1.6 Make the flap cadence configurable (`CONFIG_APP_SIM_SWITCH_FLAP_PERIOD_STEPS`, default 15 steps, `0` = never flap) instead of a hard-coded constant — each transition costs a trap and an alarm state change downstream, ~5,700 events/day at the default.
      _(Verified on `native_sim`: default 15 → transitions at 15.15 s intervals; `=3` → every 3.03 s; `=0` → zero transitions in 25 s with all five ports `up` and counters still advancing, and no compiler warning from the now-unreachable modulo.)_

## 2. SNMP frontend scaffold (`lib/snmp`)

- [x] 2.1 Copy `lib/frontend-template` to `lib/snmp` (CMakeLists.txt, Kconfig, zephyr/module.yml) and rename to the SNMP frontend; add `snmp_agent_start()` per the protocol-frontend contract.
- [x] 2.2 Add `lib/snmp/Kconfig`: agent UDP port (161), read community (`public`), trap manager IPv4/port (162)/community, and any resource caps.
- [x] 2.3 Stub `snmp_agent_start()` to bring up a bound UDP socket on the agent port and log received datagram sizes (no PDU parsing yet).

## 3. ASN.1/BER codec

- [x] 3.1 Implement BER decode helpers for the types the MIB needs: SEQUENCE, INTEGER, OCTET STRING, OID, NULL (into small fixed buffers, bounds-checked).
- [x] 3.2 Implement BER encode helpers for INTEGER, OCTET STRING, OID, NULL, Counter32, Gauge32, TimeTicks, IpAddress into a single work buffer.
- [x] 3.3 Add unit-test-style checks on `native_sim` (round-trip encode/decode of each type, including length edge cases and OID compression).

## 4. MIB view & request dispatch (polling endpoint)

- [x] 4.1 Define the static, lexicographically-sorted leaf table `{oid, type, accessor}` for the system group and the `ifTable` columns (expanded per interface), sourced from `sim_switch`.
- [x] 4.2 Parse incoming request PDUs (version, community, request-id, PDU type, varbinds); reject non-matching community silently; refuse SET (no writable objects).
- [x] 4.3 Implement `GetRequest` (binary-search exact-match lookup) and build the `Response` PDU with matching request-id.
- [x] 4.4 Implement `GetNextRequest` (next entry in table order) and `GetBulkRequest` (bounded `max-repetitions`), returning `endOfMibView` past the end.
- [x] 4.5 Enforce bounds: cap varbind/repetition output to the work buffer, return `tooBig`/`genErr` where needed, and drop malformed PDUs without crashing.
- [x] 4.6 Wire `sysUpTime.0` to time since agent start (TimeTicks) and confirm counters/status read live from `sim_switch`.
- [x] 4.7 Expose firmware name/version/build timestamp as their own OCTET STRING objects on the enterprise arc `sysObjectID.0` names (`1.3.6.1.4.1.99999.1.{1,2,3}.0`), appended after the `ifTable` so walk order holds.
      _(Verified on `native_sim`: 66 MIB objects, `snmpget` returns "zephyr-snmp-agent" / "0.1.0" / the build time individually, `snmpwalk` from `1.3.6.1` enumerates all 66 in ascending order and ends on `endOfMibView`, and `snmpbulkwalk 1.3.6.1.4.1.99999` walks the new subtree.)_

## 5. Trap notifications (publish path)

- [x] 5.1 Implement a trap sender: build a `SNMPv2-Trap` PDU with `sysUpTime.0` + `snmpTrapOID.0` leading varbinds and send it to the configured manager on 162.
- [x] 5.2 Emit one `coldStart` trap after connectivity + agent start.
- [x] 5.3 Observe `sim_switch` oper-status transitions and emit `linkDown`/`linkUp` traps (with the affected `ifIndex`) only on an actual transition.

## 6. Application (`apps/snmp-agent`)

- [x] 6.1 Create `apps/snmp-agent` (CMakeLists.txt listing `lib/common` + `lib/snmp` as extra modules, Kconfig, VERSION, `CONFIG_APP_FIRMWARE_NAME`).
- [x] 6.2 Set `prj.conf`: select `sim_switch`, enable networking/UDP sockets, and set the SNMP Kconfig defaults; add `points.d` if used.
- [x] 6.3 Write `src/main.c`: init connectivity, wait for network, call `snmp_agent_start()`, then run the simulation step loop.
- [x] 6.4 Add board overlays/conf for `esp32_devkitc`, `adafruit_feather_esp32s2_tft`, and `native_sim_native_64` mirroring the existing apps.
- [x] 6.5 mDNS/DNS-SD discovery: add a `CONFIG_APP_DNSSD_UDP` option to the shared `lib/common` DNS-SD helper (register `_<type>._udp` instead of `_tcp`), enable `CONFIG_DNS_SD` on the hardware boards, and advertise `_snmp._udp` on 161. Verified on the WROOM: `dns-sd -B _snmp._udp` lists the instance, `dns-sd -L` resolves to `<hostname>.local:161`, and `snmpwalk <hostname>.local` polls by name.

## 7. Verification (client/collector side — standard SNMP tools)

- [x] 7.1 On `native_sim`, run `snmpget`/`snmpwalk` against the agent and confirm the system group + full `ifTable` walk completes in order and ends cleanly.
- [x] 7.2 Run `snmpbulkwalk` and confirm `GETBULK` returns multiple rows and matches the `snmpwalk` result set.
- [x] 7.3 Run `snmptrapd` and confirm `coldStart` on boot and `linkUp`/`linkDown` traps appear as interfaces flap, carrying the correct `ifIndex`.
- [x] 7.4 Flash `esp32_devkitc` (ESP32-WROOM), repeat the poll + trap checks over Wi-Fi, and record free heap; note the minimum viable board profile.
      _(Flashed to a physical WROOM at 0x1000 (host esptool). Boots, joins Wi-Fi (SSID "iota"), DHCP 192.168.68.74, agent listening on UDP 161 with 63 MIB objects and coldStart sent. Verified over Wi-Fi from the host: `snmpget` scalars + full `snmpwalk` (63 objects, clean endOfMibView) with live counters. Footprint: FLASH 13.16%, DRAM 63.13% — the WROOM is the minimum viable profile with ample headroom (far lighter than the OPC-UA/open62541 build). Trap origination verified on native_sim (7.3); over-Wi-Fi traps target `CONFIG_APP_SNMP_TRAP_MANAGER` (default 192.168.68.10) — point it at a real manager to observe.)_

## 8. Docs

- [x] 8.1 Add `lib/snmp/README.md` describing the MIB view, OID map, and Kconfig options.
- [x] 8.2 Update top-level `README.md` and `SCOPE.md` to list SNMP as an available Phase-2 protocol with example `snmpget`/`snmpwalk`/`snmptrapd` commands.
- [x] 8.3 Add `apps/snmp-agent/points.d/snmp/zephyr-snmp-switch.toml` — the tedge-dot point library for this firmware (measurements, per-port link alarms, trap events, firmware identity as a parameter set) — and a top-level `README.md` section documenting the point-library convention across the three apps.
