## ADDED Requirements

### Requirement: Subscriptions and monitored items
The server SHALL support OPC-UA subscriptions and monitored items so a client
can subscribe to variable nodes and receive value-change notifications instead
of polling.

#### Scenario: Create subscription and monitor a value
- **WHEN** a connected client creates a subscription and adds a monitored item on a measurement variable node
- **THEN** the server SHALL accept them and deliver notifications when the node's value changes

#### Scenario: Notification on change
- **WHEN** a monitored measurement value changes (e.g. the sampler updates it)
- **THEN** the client SHALL receive a data-change notification reflecting the new value

### Requirement: Bounded subscription resources
The server SHALL cap the number of subscriptions, monitored items per
subscription, and queued notifications so a client cannot exhaust device memory.
Requests beyond the caps SHALL be rejected gracefully without crashing.

#### Scenario: Excess monitored items rejected
- **WHEN** a client requests more subscriptions or monitored items than the configured limits
- **THEN** the server SHALL reject the excess with an appropriate status code and continue serving existing ones

#### Scenario: Fits the device RAM budget
- **WHEN** subscriptions are enabled on a target board
- **THEN** the server SHALL start within the board's RAM budget, or subscriptions SHALL be disabled for boards that cannot fit them (writes and reads remain available)

### Requirement: Write service for writable nodes
The server SHALL support the OPC-UA Write service on nodes marked writable, so a
client can set their value.

#### Scenario: Write a value
- **WHEN** a connected client writes a valid value to a writable node
- **THEN** the server SHALL accept the write with a Good status and a subsequent read SHALL return the written value

#### Scenario: Write to a read-only node rejected
- **WHEN** a client attempts to write a measurement node that is read-only
- **THEN** the server SHALL reject the write with a Bad status and leave the value unchanged
