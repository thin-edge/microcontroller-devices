## ADDED Requirements

### Requirement: Switch/router simulation (network-interface model)

`lib/common` SHALL provide a switch/router simulation (`sim_switch`), selectable
per firmware via the existing simulation Kconfig `choice` alongside the
environment and pump simulations, that models a small managed switch/router. It
SHALL expose, through the shared data-model interface, a fixed compile-time set of
network interfaces (count bounded, e.g. ≤ 8) where each interface has: an
operational status (`up`/`down`), an administrative status, a nominal speed, and
monotonic in/out octet and packet counters. A periodic step SHALL advance the
counters for interfaces that are operationally up and SHALL flap link state over
time so that operational-status transitions occur. The flap cadence SHALL be
configurable (`APP_SIM_SWITCH_FLAP_PERIOD_STEPS`, in sampling steps), because
every transition becomes a notification and an alarm state change downstream, and
a value of `0` SHALL disable flapping entirely while leaving the counters
running. It SHALL be the default simulation for `apps/snmp-agent`. Selecting it
SHALL require no changes to any protocol frontend.

#### Scenario: Switch app selects the switch simulation

- **WHEN** `apps/snmp-agent` is built with its default configuration
- **THEN** it uses the switch/router simulation, exposing per-interface status and
  traffic counters through the shared data model

#### Scenario: Counters advance only while up

- **WHEN** the simulation steps while an interface is operationally `up`
- **THEN** that interface's in/out octet and packet counters increase monotonically
- **AND WHEN** an interface is operationally `down`
- **THEN** its traffic counters do not advance

#### Scenario: Link state flaps to drive notifications

- **WHEN** the switch simulation runs over time
- **THEN** at least one interface's operational status transitions between `up` and
  `down`, so that a consuming frontend can observe the change

#### Scenario: Flapping can be turned off

- **WHEN** the firmware is built with `APP_SIM_SWITCH_FLAP_PERIOD_STEPS = 0`
- **THEN** no interface's operational status ever changes, no link notification is
  originated, and the traffic counters of every interface keep advancing

#### Scenario: Simulation is swappable without frontend changes

- **WHEN** an app's simulation Kconfig `choice` is set to `sim_switch`
- **THEN** the firmware exposes the switch simulation's interfaces/counters through
  the same frontend, with no edit to the frontend code
