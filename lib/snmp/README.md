# lib/snmp — SNMPv2c agent frontend

Maps the shared switch/router data model (`lib/common`, `sim_switch`) onto SNMP.
It is a **minimal, self-contained SNMPv2c implementation** — Zephyr ships no SNMP
stack, so the BER/ASN.1 codec and PDU handling live here. UDP only; no external
dependencies beyond Zephyr sockets. Conforms to the protocol-frontend contract
(see `lib/common/README.md`): entry point `snmp_agent_start()`, reads the shared
model, defines no competing data source.

## Files

| File | Responsibility |
|------|----------------|
| `snmp_ber.{c,h}` | ASN.1/BER — a backward encoder (TLV nesting is trivial) and a bounds-checked forward decoder, plus `oid_cmp()`. |
| `snmp_mib.{c,h}` | The MIB view: a compile-time, lexicographically-sorted array of leaf descriptors; `mib_get_exact` (GET), `mib_get_next` (GETNEXT/GETBULK) and per-leaf value encoding. |
| `snmp_agent.{c,h}` | UDP listener on port 161; parses GET/GETNEXT/GETBULK, plans the response varbinds, and encodes a Response. Read-only (SET → `notWritable`). |
| `snmp_trap.{c,h}` | Trap originator: coldStart on start; a watcher thread emits linkUp/linkDown on interface oper-status transitions. Built only when `CONFIG_APP_SNMP_TRAP`. |

## MIB view (read-only)

**System group** — `1.3.6.1.2.1.1`:
`sysDescr.0`, `sysObjectID.0` (`1.3.6.1.4.1.99999.1`, a placeholder enterprise
arc), `sysUpTime.0`, `sysContact.0`, `sysName.0` (device hostname),
`sysLocation.0`, `sysServices.0` (6 = L2+L3).

**Firmware info** — `1.3.6.1.4.1.99999.1`, the arc `sysObjectID.0` names:

| OID | Object | Source |
|-----|--------|--------|
| `…99999.1.1.0` | firmware name | `CONFIG_APP_FIRMWARE_NAME` |
| `…99999.1.2.0` | firmware version | `APP_VERSION_STRING` (the app's `VERSION`) |
| `…99999.1.3.0` | build timestamp | compile time of the image |

All three are OCTET STRINGs from `lib/common`'s `identity.h`, the same strings
`sysDescr.0` packs into one sentence — separate objects so a collector can read
one without parsing prose. They sort after the whole `mib-2` subtree, so they are
built last and a walk ends on them.

**Interfaces group** — `1.3.6.1.2.1.2`:
`ifNumber.0`, then an `ifTable` (`…2.2.1.<col>.<row>`) with columns

| Col | Object | BER type |
|-----|--------|----------|
| 1 | ifIndex | INTEGER |
| 2 | ifDescr | OCTET STRING |
| 3 | ifType | INTEGER (6 = ethernetCsmacd) |
| 4 | ifMtu | INTEGER |
| 5 | ifSpeed | Gauge32 |
| 7 | ifAdminStatus | INTEGER (up=1, down=2) |
| 8 | ifOperStatus | INTEGER (up=1, down=2) |
| 10 / 16 | ifInOctets / ifOutOctets | Counter32 |
| 11 / 17 | ifInUcastPkts / ifOutUcastPkts | Counter32 |

The leaf array is built in walk order (system scalars, `ifNumber`, ifTable
column-major, then the firmware-info scalars), so `snmpwalk`/`snmpbulkwalk`
enumerate everything once and terminate on `endOfMibView`. With the default five
interfaces that is 66 objects.

## Traps

SNMPv2-Trap PDUs (first varbinds `sysUpTime.0` + `snmpTrapOID.0`) to the
configured manager:

- `coldStart` (`1.3.6.1.6.3.1.1.5.1`) once after connectivity.
- `linkDown` (`…5.3`) / `linkUp` (`…5.4`) with the affected `ifIndex`, on an
  actual oper-status transition only.

## Discovery (mDNS / DNS-SD)

On hardware the app advertises itself as **`_snmp._udp`** and answers to its
unique `<hostname>.local`. Discovery uses the shared `lib/common` helper — the
app sets `CONFIG_APP_DNSSD_SERVICE_TYPE="_snmp"`, `CONFIG_APP_DNSSD_PORT=161` and
`CONFIG_APP_DNSSD_UDP=y` (the UDP variant of the DNS-SD registration), and the
board configs enable `CONFIG_DNS_SD`. Browse with `dns-sd -B _snmp._udp local.`,
then `snmpwalk … <hostname>.local`.

## Kconfig (`CONFIG_APP_SNMP_*`)

| Option | Default | Meaning |
|--------|---------|---------|
| `APP_SNMP_PORT` | 161 | Agent UDP port |
| `APP_SNMP_READ_COMMUNITY` | `public` | Required community (mismatch → silent drop) |
| `APP_SNMP_SYS_CONTACT` / `APP_SNMP_SYS_LOCATION` | | sysContact.0 / sysLocation.0 |
| `APP_SNMP_TRAP` | y | Enable trap origination |
| `APP_SNMP_TRAP_MANAGER` | `192.168.68.10` | Trap destination IPv4 (override per site) |
| `APP_SNMP_TRAP_PORT` | 162 | Trap destination UDP port |
| `APP_SNMP_TRAP_COMMUNITY` | `public` | Community in trap PDUs |

Interface count/behaviour comes from the switch simulation (`lib/common`):
`CONFIG_APP_SIM_SWITCH_IF_COUNT` sets the number of ifTable rows, and
`CONFIG_APP_SIM_SWITCH_FLAP_PERIOD_STEPS` how often the flapping port toggles —
i.e. how often this library originates a linkUp/linkDown trap (default every 15
sampling steps; `0` stops flapping altogether).

## Resource notes

One bound UDP socket for the agent (+ one for traps), fixed ~1.5 KB request and
response work buffers, a bounded interface count (≤ 8) and a compile-time cap on
emitted varbinds — no per-request heap allocation. On the ESP32-WROOM the whole
firmware uses ~13 % flash and leaves ample RAM (far lighter than the open62541
OPC-UA build).
