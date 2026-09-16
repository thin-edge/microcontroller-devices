# simulation-model Specification

## Purpose

Define a pluggable, per-firmware simulation layer behind the shared data-model interface, including the environment simulation (default) and the control-driven pump/motor simulation.
## Requirements
### Requirement: Pluggable simulation selected per firmware

`lib/common` SHALL define a data-model/simulation interface — enumerated
measurements (read), control points (read/write, with clamps), and a periodic
step — behind which multiple simulation implementations can live. Exactly one
simulation SHALL be compiled into a given firmware, chosen by a Kconfig `choice`
with a per-app default. Frontends SHALL consume whatever measurements and control
points the selected simulation exposes; switching a firmware's simulation SHALL
require only changing the app's simulation selection, not frontend code.

#### Scenario: Each app selects its simulation

- **WHEN** `apps/opcua-server` is built with its default configuration
- **THEN** it uses the environment simulation
- **AND WHEN** `apps/modbus-server` is built with its default configuration
- **THEN** it uses the pump simulation

#### Scenario: Simulation is swappable without frontend changes

- **WHEN** an app's simulation Kconfig choice is changed to a different available
  simulation
- **THEN** the firmware exposes that simulation's measurements/controls through
  the same frontend, with no edit to the frontend code

### Requirement: Environment simulation (unchanged default)

The environment simulation SHALL provide the existing measurements
(`temperature`, `humidity`, `pressure`, smoothly varying) and control points
(`Setpoint`, `Running`). It is the default for the OPC-UA firmware, whose exposed
node set is unchanged by this change (no reflash required for existing behavior).

#### Scenario: OPC-UA keeps its current data model

- **WHEN** the OPC-UA firmware (environment simulation) is browsed
- **THEN** it exposes `temperature`/`humidity`/`pressure` measurements and the
  writable `Setpoint`/`Running` control points exactly as before this change

### Requirement: Pump/motor simulation (control-driven)

The pump simulation SHALL provide read-only measurements `flow_lpm`,
`pressure_bar`, `motor_temp_c`, `rpm`, `vibration_mms` and a monotonic
`run_hours`, plus control points `speed_setpoint` (%, 0–100, clamped), `running`,
and `mode` (enum 0=off/1=auto/2=manual, clamped). A stateful step (on the sample
interval) SHALL couple the measurements to the controls: commanded speed from
`running`/`mode`/`speed_setpoint`, actual speed ramping toward commanded
(inertia), pump affinity laws (flow ∝ speed, pressure ∝ speed²), rpm/vibration
rising with speed, a first-order thermal model for `motor_temp_c`, `run_hours`
accruing only while running, and an over-temp fault flag. It is the default for
the Modbus firmware.

#### Scenario: Stopping the pump

- **WHEN** `running` is set false (or `mode` is off)
- **THEN** within a few sample intervals `flow_lpm`, `pressure_bar`, and `rpm`
  fall toward zero, `motor_temp_c` trends back toward ambient, and `run_hours`
  stops increasing

#### Scenario: Raising the speed setpoint (manual mode, running)

- **WHEN** `mode` = manual, `running` = true, and `speed_setpoint` is increased
- **THEN** `rpm` and `flow_lpm` rise roughly proportionally to speed, `pressure_bar`
  rises faster (∝ speed²), and `vibration_mms` and (over time) `motor_temp_c`
  increase

#### Scenario: Run-hours accrue only while running

- **WHEN** the pump runs for a period, then is stopped for a period
- **THEN** `run_hours` increases while running and holds constant while stopped

#### Scenario: Over-temperature fault

- **WHEN** sustained high speed drives `motor_temp_c` above the over-temp threshold
- **THEN** the simulated fault status (e.g. Modbus discrete input `DI 1`) is
  asserted

### Requirement: Clamping and shared control semantics

Control-point writes SHALL be range-clamped by the shared control API so every
frontend behaves identically, and the clamped state SHALL be observable through
that API and by any other frontend built against the same simulation.

#### Scenario: Setpoint clamp

- **WHEN** a client writes a control point above its maximum
- **THEN** the stored value is clamped to the maximum and a read-back returns the
  clamped value

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

