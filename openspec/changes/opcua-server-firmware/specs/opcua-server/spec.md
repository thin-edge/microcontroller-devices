## ADDED Requirements

### Requirement: Device joins Wi-Fi before serving
On hardware, the firmware SHALL connect to a Wi-Fi network in station mode using
the configured credentials and obtain an IP address before it begins accepting
OPC-UA connections. On `native_sim`, the host network backend stands in for the
radio.

#### Scenario: Wi-Fi association and address
- **WHEN** valid Wi-Fi credentials are configured and the device boots on hardware
- **THEN** the device SHALL associate with the network and obtain an IP address before starting the OPC-UA server

#### Scenario: Reconnect after Wi-Fi loss
- **WHEN** the Wi-Fi connection is lost while running
- **THEN** the firmware SHALL attempt to reconnect and resume serving OPC-UA once connectivity is restored, without requiring a manual reboot

### Requirement: OPC-UA server endpoint
The firmware SHALL run an OPC-UA server that listens on a configurable TCP port
(default 4840) using the OPC-UA binary protocol (`opc.tcp`) once the device's
network interface is up.

#### Scenario: Server starts and listens
- **WHEN** the device boots and the network interface has an address
- **THEN** the OPC-UA server SHALL be listening on the configured `opc.tcp` port

#### Scenario: Configurable port
- **WHEN** the server port is changed via configuration and the firmware is rebuilt/flashed
- **THEN** the OPC-UA server SHALL listen on the newly configured port

### Requirement: Client discovery and session establishment
The server SHALL allow a standard OPC-UA client to discover its endpoint and
establish a session so that subsequent browse and read requests succeed.

#### Scenario: GetEndpoints discovery
- **WHEN** an OPC-UA client sends a discovery (GetEndpoints) request to the server
- **THEN** the server SHALL return at least one usable endpoint description for the server

#### Scenario: Session established
- **WHEN** an OPC-UA client opens a secure channel and creates/activates a session
- **THEN** the server SHALL accept the session and permit browse and read requests

### Requirement: Browse the address space
The server SHALL expose a browsable address space so that a client can navigate
from the Objects folder to the device's nodes.

#### Scenario: Browse from Objects folder
- **WHEN** a connected client browses starting at the standard Objects folder
- **THEN** the client SHALL be able to reach the device object and its child data nodes

### Requirement: Read node values
The server SHALL support reading the value attribute of its variable nodes and
return the current value with a good status code.

#### Scenario: Read a measurement value
- **WHEN** a connected client reads the value attribute of a measurement variable node
- **THEN** the server SHALL return the current value with a Good status code

#### Scenario: Read reflects updated data
- **WHEN** the underlying data source updates a measurement and the client reads that node again
- **THEN** the server SHALL return the updated value

### Requirement: Bounded resource usage
The server SHALL operate within the reference board's resource budget by
bounding concurrent connections/sessions and avoiding unbounded per-request
allocation, so it remains stable under repeated client connect/read cycles.

#### Scenario: Repeated connect and read
- **WHEN** a client repeatedly connects, reads nodes, and disconnects many times
- **THEN** the server SHALL continue to respond successfully without exhausting memory or crashing

#### Scenario: Excess connections rejected gracefully
- **WHEN** more clients attempt to connect than the configured maximum
- **THEN** the server SHALL reject the excess connections without crashing and continue serving existing sessions
