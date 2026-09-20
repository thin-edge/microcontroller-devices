# tedge-remote-access Specification

## Purpose
Cumulocity remote access through the device: bridging a cloud session to a TCP target on the device's network, the target policy and session cap that bound it, and the events and capacity data that make it auditable.

## Requirements
### Requirement: The device bridges a cloud remote-access session to a TCP target

When the remote-access feature is built in, the client SHALL handle the
Cumulocity remote-access connect operation by opening a TCP connection to the
requested host and port and bridging it to Cumulocity's device-side WebSocket
until either side closes. It SHALL authenticate the WebSocket with the token
it holds for the tenant, and SHALL NOT log the connection key at any log
level.

#### Scenario: SSH to a host on the device's network

- **WHEN** an operator opens a remote-access session whose endpoint is an SSH
  server on the device's subnet
- **THEN** the device bridges the session, and the operator reaches that SSH
  server

#### Scenario: The target closes

- **WHEN** the target closes the TCP connection
- **THEN** the device closes the WebSocket and reports the session as ended

### Requirement: Targets are checked before any connection is opened

The client SHALL check the requested target against the build-time policy
(the device's own IPv4 subnets, an allow-list, or the device itself) and then
against the application's hook, before opening any socket to it. A target the
policy or the hook refuses SHALL fail the operation with a reason naming the
policy, and SHALL NOT be contacted. A host name SHALL be resolved first and
judged by the address it resolves to.

#### Scenario: Target outside the subnet

- **WHEN** a remote-access connect names an address outside the device's
  subnets and the policy is the default
- **THEN** no TCP connection is attempted, and the operation fails with a
  reason naming the policy

#### Scenario: Application narrows the policy

- **WHEN** the application's hook refuses a target that the built-in policy
  allows
- **THEN** the operation fails with the reason, and no connection is made

### Requirement: Concurrent sessions are capped and the refusal is immediate

The number of sessions open at once SHALL NOT exceed the configured maximum.
A request that would exceed it SHALL be failed as soon as the device receives
it, with a reason naming the limit, and SHALL NOT disturb the sessions that
are open.

#### Scenario: Second session while one is open

- **WHEN** the cap is one session and a second remote-access connect arrives
- **THEN** the device fails the new operation within seconds with a reason
  naming the limit, and the open session continues

### Requirement: Every session is auditable

The client SHALL publish an event when a tunnel opens and when it ends. The
open event SHALL name the target host and port; the close event SHALL name
the target, why the session ended and how many bytes travelled each way.

#### Scenario: Session ends

- **WHEN** a tunnel that carried traffic ends because the client disconnected
- **THEN** an event records the target, the reason and the byte counts

### Requirement: Remote-access capacity is visible before a session is opened

The client SHALL publish `tedge_RemoteAccess` twin data with the session
limit, the number of active sessions, the number of free seats and the target
policy. It SHALL publish it on every connect, with no active sessions, and
whenever a session opens or ends. Including the open sessions' targets SHALL
be a build-time option, because those are internal addresses.

#### Scenario: Operator checks before connecting

- **WHEN** one session is open and the cap is one
- **THEN** the twin data shows `activeSessions` 1 and `freeSessions` 0

#### Scenario: Device reboots with a session open

- **WHEN** the device reboots while a tunnel is open and reconnects
- **THEN** the twin data it publishes shows no active sessions and all seats
  free

### Requirement: A telnet target is offered echo by the bridge

When the target port is the telnet port, the bridge SHALL offer echo and
suppress-go-ahead to the cloud-side client as soon as the tunnel is up, so
that a browser terminal echoes what the operator types.

#### Scenario: Web terminal to a telnet target

- **WHEN** an operator opens a telnet session through the device
- **THEN** characters typed in the browser terminal appear as they are typed

### Requirement: A session that goes quiet is ended

The client SHALL close a tunnel that has carried no traffic in either
direction for the configured idle timeout, free its seat, and name the
timeout in the close event.

#### Scenario: Forgotten session

- **WHEN** an operator leaves a session open with no traffic for longer than
  the idle timeout
- **THEN** the device closes it, publishes the close event and frees the seat

