## ADDED Requirements

### Requirement: Device identity in the address space
The address space SHALL contain a device object that carries identification
metadata (at minimum a human-readable device/application name) so a client can
identify what it is connected to.

#### Scenario: Device object present with identity
- **WHEN** a client browses the address space after connecting
- **THEN** it SHALL find a device object exposing a readable device/application name

### Requirement: Measurement variable nodes
The address space SHALL expose one or more measurement variable nodes, each with
a browse name, a numeric/scalar data type, and a value that can be read.

#### Scenario: At least one measurement node exists
- **WHEN** a client browses the device object's children
- **THEN** it SHALL find at least one measurement variable node with a readable scalar value

#### Scenario: Measurement has correct data type
- **WHEN** a client reads the data type of a measurement variable node
- **THEN** the reported data type SHALL match the value returned when reading the node

### Requirement: Data-source layer updates node values
A data-source layer SHALL periodically sample device data and update the
corresponding OPC-UA variable node values. The Phase 1 source MAY be simulated
(synthetic) data.

#### Scenario: Values update over time
- **WHEN** the sampling interval elapses
- **THEN** the measurement variable node values SHALL be refreshed from the data source

#### Scenario: Simulated source produces plausible values
- **WHEN** no real sensor is configured and the simulated source is active
- **THEN** the measurement nodes SHALL report changing, in-range synthetic values

### Requirement: Data source is decoupled from the OPC-UA layer
The data-source layer SHALL be independent of the OPC-UA server layer so that a
real sensor source can replace the simulated source without changing the address
space or server code.

#### Scenario: Swap source without changing OPC-UA layer
- **WHEN** the data source implementation is replaced (e.g. simulated → real sensor)
- **THEN** the OPC-UA address space and server code SHALL require no changes to continue serving those nodes
