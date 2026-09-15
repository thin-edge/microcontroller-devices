## ADDED Requirements

### Requirement: Modbus TCP server on port 502

The firmware SHALL run a Modbus TCP server that listens on TCP port 502 once
network connectivity is available, accept a client (master) connection, and
respond to standard Modbus request PDUs framed in MBAP headers. It SHALL use
Zephyr's in-tree `modbus` subsystem in raw-ADU server mode with a self-managed
TCP transport.

#### Scenario: Client connects and reads

- **WHEN** a Modbus TCP master opens a connection to the device on port 502 and
  issues a Read Input Registers (FC04) request for unit id 1
- **THEN** the server responds with a valid Modbus TCP response carrying the
  requested register values
- **AND** the transaction id and unit id in the response match the request

#### Scenario: Client reconnects

- **WHEN** a client disconnects and a new client connects later
- **THEN** the server accepts the new connection and continues serving without a
  device reboot

### Requirement: All four Modbus object types with a defined register map

The server SHALL expose all four primary Modbus object types, mapped from the
shared pump/motor data model (`lib/common`) plus simulated device status, per this
register map (unit id 1, zero-based addresses):

- **Input Registers (FC04, read-only, 16-bit):**
  - `IR 0` flow_lpm ×10, `IR 1` pressure_bar ×100, `IR 2` motor_temp_c ×10
    (signed), `IR 3` rpm (direct), `IR 4` vibration_mms ×100
  - `IR 10..11` run_hours as a **32-bit unsigned counter** (2 registers,
    big-endian)
  - `IR 20..21` flow_lpm, `IR 22..23` pressure_bar, `IR 24..25` motor_temp_c as
    32-bit IEEE-754 floats (2 registers each, big-endian)
- **Holding Registers (FC03/06/16, read/write, 16-bit):**
  - `HR 0` speed_setpoint (0–100), `HR 1` mode (0–2)
- **Coils (FC01/05/15, read/write, 1-bit):**
  - `Coil 0` running
- **Discrete Inputs (FC02, read-only, 1-bit):**
  - `DI 0` running (mirror), `DI 1` fault (simulated), `DI 2` network connected

Reads of unmapped addresses SHALL return the Modbus illegal-data-address
exception.

#### Scenario: Read measurements from input registers

- **WHEN** a master reads input registers `IR 0..4`
- **THEN** it receives flow/pressure/motor-temp/rpm/vibration as the scaled
  integers above, changing over time as the data source updates
- **AND** reading `IR 20..25` yields the same measurements as IEEE-754 floats

#### Scenario: Read the run-hours counter

- **WHEN** a master reads input registers `IR 10..11` as a 32-bit big-endian value
- **THEN** it receives the current run-hours count
- **AND** the value is non-decreasing on subsequent reads

#### Scenario: Read device status from discrete inputs

- **WHEN** a master reads discrete inputs `DI 0..2`
- **THEN** `DI 0` reflects the current running state, `DI 2` reflects network
  connectivity, and `DI 1` reports the simulated fault flag

#### Scenario: Illegal address

- **WHEN** a master reads a register/coil outside the defined map
- **THEN** the server returns the illegal-data-address exception (not a crash or
  arbitrary data)

### Requirement: Writes update the shared control points with clamping

Writes to writable objects SHALL update the shared control-point state via the
`lib/common` control API, applying the same range clamping as other frontends. A
read-back SHALL reflect the stored (possibly clamped) value.

#### Scenario: Write and clamp the speed setpoint holding register

- **WHEN** a master writes a value above 100 to `HR 0` (speed_setpoint)
- **THEN** the stored speed_setpoint is clamped to 100
- **AND** reading `HR 0` back returns the clamped value
- **AND** the same value is observable via the shared control API

#### Scenario: Write the mode holding register

- **WHEN** a master writes `HR 1` (mode) with a value outside 0–2
- **THEN** the stored mode is clamped into 0–2 and reads back accordingly

#### Scenario: Toggle the Running coil

- **WHEN** a master writes `Coil 0` on/off (FC05)
- **THEN** the shared running control updates accordingly
- **AND** discrete input `DI 0` (running mirror) reflects the new state

### Requirement: Conforms to the protocol-frontend contract

The Modbus TCP server SHALL be a `lib/modbus/` frontend that depends only on
`lib/common`, exposes a `modbus_server_start()` lifecycle entry point called by
`apps/modbus-server` once connectivity is up, and defines its Modbus-specific
options in its own Kconfig. It SHALL NOT introduce a competing data source.

#### Scenario: Application composition

- **WHEN** `apps/modbus-server` is built
- **THEN** it composes `lib/common` + `lib/modbus`
- **AND** `CONFIG_APP_FIRMWARE_NAME` defaults to `zephyr-modbus-server`
- **AND** the device advertises the same identity (device id / firmware
  name+version) available to any frontend
