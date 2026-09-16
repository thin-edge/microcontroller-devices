# snmp-agent Specification

## Purpose
TBD - created by archiving change snmp-agent-firmware. Update Purpose after archive.
## Requirements
### Requirement: SNMPv2c agent on UDP port 161

The firmware SHALL run an SNMPv2c agent that binds UDP port 161 once network
connectivity is available and answers `GetRequest`, `GetNextRequest`, and
`GetBulkRequest` PDUs, returning a `Response` PDU with matching `request-id` and
correctly BER-encoded variable bindings. It SHALL conform to the protocol-frontend
contract (`lib/common`): it is started via `snmp_agent_start()` after
connectivity, reads only the shared data model, and defines no competing data
source. The agent SHALL be connectionless — it retains no per-manager session and
serves any number of managers over time without a reboot.

#### Scenario: Manager reads a scalar

- **WHEN** a manager sends a `GetRequest` for `sysName.0` with the configured read
  community
- **THEN** the agent replies with a `Response` PDU whose variable binding carries
  `sysName.0` as an OCTET STRING and whose `request-id` matches the request

#### Scenario: Multiple managers over time

- **WHEN** one manager polls the agent and later a different manager polls it
- **THEN** both receive valid responses without any device reboot or reconfiguration

### Requirement: Community-string access control (read-only)

The agent SHALL accept requests only when the request's community string matches
the configured read community (Kconfig, default `public`), and SHALL silently
ignore (no response) requests with a non-matching community, per SNMP convention.
The agent SHALL be read-only: it exposes no writable objects, and any `SetRequest`
SHALL be answered with an error-status of `noSuchName`/`notWritable` (or dropped),
never mutating state.

#### Scenario: Wrong community is ignored

- **WHEN** a `GetRequest` arrives with a community string other than the configured
  read community
- **THEN** the agent sends no response and does not read or change any value

#### Scenario: Writes are refused

- **WHEN** a `SetRequest` is received for any OID
- **THEN** no object value changes and the agent does not perform a write

### Requirement: Switch/router MIB view (system + interfaces groups)

The agent SHALL expose a browsable MIB view mapped from the selected simulation,
comprising the standard **system group** — `sysDescr.0`, `sysObjectID.0`,
`sysUpTime.0` (TimeTicks since agent start), `sysContact.0`, `sysName.0`,
`sysLocation.0`, `sysServices.0` — and the **MIB-II interfaces group**:
`ifNumber.0` and an `ifTable` (`ifIndex`, `ifDescr`, `ifType`, `ifMtu`,
`ifSpeed` (Gauge32), `ifAdminStatus`, `ifOperStatus` (up=1/down=2),
`ifInOctets`/`ifOutOctets` and `ifInUcastPkts`/`ifOutUcastPkts` as Counter32) for
each simulated interface. Values SHALL be sourced from the shared data model, and
`sysUpTime`/counters SHALL reflect live device state at request time.

#### Scenario: Interface row reflects the simulation

- **WHEN** a manager reads `ifOperStatus.<n>` for a simulated interface that is
  currently down
- **THEN** the agent returns the INTEGER value `2` (down) for that interface index

#### Scenario: Counters advance

- **WHEN** a manager reads `ifInOctets.<n>` twice while the interface is passing
  simulated traffic
- **THEN** the second Counter32 value is greater than or equal to the first
  (monotonic between wraps)

### Requirement: Firmware identity objects on the enterprise arc

The agent SHALL expose the device's firmware name, firmware version and build
timestamp as three separate OCTET STRING objects under the private-enterprise arc
that `sysObjectID.0` names (`1.3.6.1.4.1.99999.1.{1,2,3}.0`), sourced from the
shared identity model in `lib/common`. `sysDescr.0` MAY continue to render the
same three strings as one sentence, but a collector SHALL NOT have to parse it to
obtain any one of them. The objects SHALL sort after the `mib-2` subtree so the
MIB view stays in lexicographic walk order.

#### Scenario: Version is readable on its own

- **WHEN** a manager reads `1.3.6.1.4.1.99999.1.2.0`
- **THEN** the agent returns the firmware version string (the app's
  `VERSION`-derived `APP_VERSION_STRING`) as an OCTET STRING, with no other text
  around it

#### Scenario: A walk still ends cleanly

- **WHEN** a manager walks the MIB from the root
- **THEN** the firmware identity objects appear after the last `ifTable` object,
  in ascending OID order, and the walk terminates on `endOfMibView`

### Requirement: GETNEXT and GETBULK walk the MIB in lexicographic order

The agent SHALL support MIB traversal such that `GetNextRequest` returns the
lexicographically next object after each requested OID, and `GetBulkRequest`
returns up to `max-repetitions` successive objects per non-repeater binding.
Walking past the last object SHALL yield `endOfMibView`. A full walk from the root
SHALL therefore enumerate the entire MIB view exactly once, in order, enabling
`snmpwalk`/`snmpbulkwalk` to complete.

#### Scenario: snmpwalk completes

- **WHEN** a manager issues `snmpwalk` (repeated `GetNextRequest`) from the MIB
  root
- **THEN** it receives every object in the system and interfaces groups exactly
  once in ascending OID order and terminates cleanly at `endOfMibView`

#### Scenario: GETBULK returns multiple rows

- **WHEN** a manager issues a `GetBulkRequest` with `max-repetitions` = 5 starting
  at `ifOperStatus`
- **THEN** the response contains up to 5 successive `ifOperStatus` column values in
  interface-index order, bounded by the response buffer

### Requirement: Bounded, allocation-free request handling

The agent SHALL process each request within a fixed work buffer and a compile-time
bound on interface count, performing no per-request dynamic heap allocation. A
request that is malformed, unsupported, or too large to answer in one datagram
SHALL produce a valid SNMP error-status response (e.g. `tooBig`, `genErr`) or be
safely dropped, never crashing or leaking memory.

#### Scenario: Oversized GETBULK is bounded

- **WHEN** a `GetBulkRequest` asks for more repetitions than fit in one response
  datagram
- **THEN** the agent truncates the variable-binding list to what fits (or returns
  `tooBig`) and returns a well-formed response

#### Scenario: Malformed PDU does not crash

- **WHEN** a datagram with a malformed BER structure arrives on port 161
- **THEN** the agent discards it without crashing and remains able to serve
  subsequent valid requests

