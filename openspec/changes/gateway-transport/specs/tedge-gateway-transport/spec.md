## ADDED Requirements

### Requirement: A device reaches the cloud through its gateway

When the gateway transport is built in, the device SHALL connect to a
thin-edge.io gateway on its own network and SHALL NOT require a route to the
cloud, a cloud certificate, a cloud token or a synchronised clock of its
own. It SHALL find its gateway by discovery, falling back to a configured
address.

#### Scenario: A device with no route to the internet

- **WHEN** a device on a plant network with no internet route is started
  next to a thin-edge.io gateway
- **THEN** it appears in the cloud, through the gateway, with its identity
  and type

#### Scenario: The gateway moves

- **WHEN** the gateway's address changes and discovery is available
- **THEN** the device finds it again without being reconfigured

#### Scenario: A network without discovery

- **WHEN** discovery finds nothing and an address is configured
- **THEN** the device uses the configured address and says which it used

### Requirement: Telemetry and state are unchanged by the transport

Telemetry and state SHALL reach the cloud through the gateway with the same
meaning they have on the direct transport: the measurements, events, alarms,
twin data and health an application produces SHALL be unchanged by the
choice of transport, and the application SHALL NOT have to change anything.

#### Scenario: The same application on either transport

- **WHEN** an application is built once for the direct transport and once
  for the gateway
- **THEN** the same measurements, events and alarms appear in the cloud

### Requirement: Operations arrive as commands and are answered as commands

The device SHALL advertise the commands it can carry out, SHALL execute a
command the gateway sends, and SHALL report its progress and outcome so the
gateway can complete the cloud's operation. A command the image cannot carry
out SHALL be reported as failed with a reason, never left unanswered.

#### Scenario: A restart through the gateway

- **WHEN** an operator restarts the device from the cloud
- **THEN** the device restarts and the operation is reported as successful
  once it is back

#### Scenario: A command the image does not have

- **WHEN** the gateway sends a command for a feature that is not built in
- **THEN** the device reports it as failed, naming the missing feature

### Requirement: Files transfer through the gateway

Firmware downloads and log uploads SHALL use the gateway's file-transfer
service, without TLS and without cloud credentials.

#### Scenario: A firmware update through the gateway

- **WHEN** a firmware update is sent to a device on the gateway transport
- **THEN** the image is fetched from the gateway and installed as it is on
  the direct transport

### Requirement: A gateway build carries no cloud credentials

An image built for the gateway transport SHALL NOT contain the device
certificate, its enrollment, the cloud token handling or a TLS session for
the cloud connection.

#### Scenario: A board too small for TLS

- **WHEN** a board that cannot hold a TLS session alongside its protocol
  server is built for the gateway transport
- **THEN** the image fits and the device is managed through the gateway
