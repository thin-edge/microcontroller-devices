## ADDED Requirements

### Requirement: Shared device data model owned by common

`lib/common/` SHALL own the device's protocol-independent data model: a set of
read-only measurements, a set of writable control points, and the device
identity (unique id, firmware name, firmware version, build timestamp). Protocol
frontends SHALL read and write this model through the common interface and SHALL
NOT define their own competing source of device data.

#### Scenario: Frontend reads measurements from the common model

- **WHEN** any protocol frontend needs a measurement value
- **THEN** it obtains it from the `lib/common/` data-model interface
- **AND** the same measurement value is available to any other frontend built
  against the same model

#### Scenario: Frontend applies a writable control point through common

- **WHEN** a client of a protocol frontend writes a supported control point
- **THEN** the frontend forwards the write to the common data-model interface
  (including any range/clamp rules the model enforces)
- **AND** the updated value is observable through the common interface

### Requirement: Protocol frontend contract

A protocol frontend SHALL be a library under `lib/<protocol>/` that exposes a
uniform lifecycle to its application: initialise against the shared data model,
start serving once connectivity is available, and expose the model over its wire
protocol. A frontend SHALL depend on `lib/common/` and SHALL NOT depend on
another protocol frontend.

#### Scenario: Frontend lifecycle relative to connectivity

- **WHEN** an application starts and connectivity becomes available
- **THEN** the composed protocol frontend is initialised against the shared data
  model and begins serving on its configured endpoint
- **AND** the frontend surfaces the shared measurements, writable control points,
  and device identity over its protocol

#### Scenario: Frontend isolation

- **WHEN** a new protocol frontend library is added under `lib/<protocol>/`
- **THEN** it can be developed and built without modifying `lib/common/` or any
  other protocol frontend
- **AND** an application can adopt it by composing `lib/common/` with that library

### Requirement: OPC-UA frontend conforms to the contract

The existing OPC-UA server SHALL be refactored into `lib/opcua/` as a protocol
frontend conforming to this contract, with no change to its externally observable
OPC-UA behavior (node set, addresses, writable Setpoint clamping, identity/version
nodes, and connection-churn resilience).

#### Scenario: OPC-UA behavior preserved after relocation

- **WHEN** `apps/opcua-server` (built from `lib/common` + `lib/opcua`) is exercised
  by an OPC-UA client
- **THEN** the client browses and reads the same nodes as before the restructure
  (`Device`, `DeviceId`, `FirmwareName`, `FirmwareVersion`, `BuildTimestamp`,
  measurements, writable `Setpoint`/`Running`)
- **AND** writing `Setpoint` out of range is clamped as before
- **AND** the 300-cycle connection-churn stress completes with zero net-buffer
  allocation failures and zero listen-socket closures
