## ADDED Requirements

### Requirement: SNMPv2c trap notifications to a configured manager

The firmware SHALL originate SNMPv2c trap PDUs (`SNMPv2-Trap`) to a manager whose
IPv4 address, UDP port (default 162), and trap community (default `public`) are
configured via Kconfig. Each trap SHALL be a well-formed v2c notification whose
first two variable bindings are `sysUpTime.0` (TimeTicks) and `snmpTrapOID.0`
(the OID identifying the notification), followed by any notification-specific
bindings. Trap delivery is best-effort over UDP (no retransmission or
acknowledgement).

#### Scenario: Trap is well-formed and addressed

- **WHEN** the firmware originates a trap
- **THEN** it sends a `SNMPv2-Trap` PDU to the configured manager IP and port whose
  first varbind is `sysUpTime.0` and whose second varbind is `snmpTrapOID.0`
- **AND** the community string in the PDU is the configured trap community

#### Scenario: Trap destination is configurable

- **WHEN** the trap manager address/port/community Kconfig values are changed and
  the firmware is rebuilt
- **THEN** subsequent traps are sent to the newly configured destination

### Requirement: coldStart trap on startup

The firmware SHALL emit a `coldStart` trap (`snmpTrapOID.0` =
`1.3.6.1.6.3.1.1.5.1`) once, after network connectivity is established and the
agent has started, to announce that the device has (re)initialized.

#### Scenario: Cold start announced

- **WHEN** the device boots, connects to the network, and starts the SNMP agent
- **THEN** exactly one `coldStart` trap is sent to the configured manager

### Requirement: linkUp / linkDown traps on interface state changes

When a simulated interface's operational status transitions, the firmware SHALL
emit the corresponding standard notification: `linkDown`
(`snmpTrapOID.0` = `1.3.6.1.6.3.1.1.5.3`) when it goes to `down`, and `linkUp`
(`1.3.6.1.6.3.1.1.5.4`) when it goes to `up`. Each SHALL include the affected
interface's `ifIndex` (and MAY include `ifAdminStatus`/`ifOperStatus`) as
notification-specific variable bindings. A trap SHALL be emitted only on an actual
transition, not on every poll.

#### Scenario: Interface goes down

- **WHEN** a simulated interface transitions from `up` to `down`
- **THEN** the firmware sends one `linkDown` trap carrying that interface's
  `ifIndex`

#### Scenario: Interface comes back up

- **WHEN** that interface later transitions from `down` to `up`
- **THEN** the firmware sends one `linkUp` trap carrying the same `ifIndex`

#### Scenario: No trap without a transition

- **WHEN** an interface remains in the same operational state across multiple
  simulation steps
- **THEN** no additional `linkUp`/`linkDown` trap is emitted for it
