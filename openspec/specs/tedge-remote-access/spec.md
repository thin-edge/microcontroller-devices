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
judged by the address it resolves to; this applies equally to a name
resolved over unicast DNS and to a `.local` name resolved over mDNS. An
allow-list entry SHALL match the host as requested, so a `.local` name is
allowed by listing that name; because any host on the link can answer an
mDNS query, a `.local` name SHALL in every policy also resolve to an
address on the device's own IPv4 subnets or to the device itself. A name that cannot be resolved SHALL fail the
operation with a reason that names the host and says whether it timed out
or failed.

#### Scenario: Target outside the subnet

- **WHEN** a remote-access connect names an address outside the device's
  subnets and the policy is the default
- **THEN** no TCP connection is attempted, and the operation fails with a
  reason naming the policy

#### Scenario: Application narrows the policy

- **WHEN** the application's hook refuses a target that the built-in policy
  allows
- **THEN** the operation fails with the reason, and no connection is made

#### Scenario: An mDNS answer outside the subnet

- **WHEN** a `.local` name resolves to an address outside the device's
  subnets and the policy is the default
- **THEN** no TCP connection is attempted, and the operation fails with a
  reason naming the policy and the address the name resolved to

#### Scenario: A `.local` name on the allow-list

- **WHEN** the policy is the allow-list, it lists `rpi5.local:22`, and a
  connect names `rpi5.local:22`
- **THEN** the name is resolved over mDNS and the session is bridged

#### Scenario: A listed `.local` name answers from off the subnet

- **WHEN** the policy is the allow-list, it lists `rpi5.local:22`, and the
  mDNS answer for `rpi5.local` is an address outside the device's subnets
- **THEN** no TCP connection is attempted, and the operation fails with a
  reason naming the address

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
When the target was given as a name, both events SHALL also name the
address it resolved to, so the record shows which machine was reached.

#### Scenario: Session ends

- **WHEN** a tunnel that carried traffic ends because the client disconnected
- **THEN** an event records the target, the reason and the byte counts

#### Scenario: Session to an mDNS name

- **WHEN** a tunnel to `rpi5.local:22` opens and later ends, and the name
  resolved to 192.168.68.72
- **THEN** both events name `rpi5.local` and 192.168.68.72

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

### Requirement: A `.local` target is resolved over mDNS

The client SHALL resolve a remote-access target whose host name ends in
`.local` by a multicast DNS query on the device's network when mDNS
resolution is built in, and the session SHALL proceed with the IPv4 address it resolves
to. Each lookup SHALL be bounded by the configured timeout. A lookup that
times out or fails with a transient error SHALL be retried after the
configured delay, up to the configured number of attempts in all, and
the session SHALL proceed as soon as any attempt resolves; so a host that
never answers fails the operation within a bounded time rather than
holding the seat. A
`.local` target on a build without mDNS resolution SHALL fail with a
reason saying mDNS is not built in, and SHALL NOT be sent to a unicast DNS
server.

#### Scenario: SSH to a Raspberry Pi by its mDNS name

- **WHEN** an operator opens a remote-access session whose endpoint is
  `rpi5-d83add9f145a.local:22`, and that host answers mDNS on the device's
  subnet
- **THEN** the device resolves the name, bridges the session to the
  address it answered with, and the operator reaches its SSH server

#### Scenario: The host has a new DHCP lease

- **WHEN** a session to `rpi5-d83add9f145a.local:22` is opened after the
  host's address changed since the previous session
- **THEN** the device connects to the new address, without any change to
  the endpoint in Cumulocity

#### Scenario: The first lookup goes unanswered

- **WHEN** the first mDNS query for the requested `.local` host gets no
  answer within the resolve timeout, and the second one does
- **THEN** the device bridges the session to the address from the second
  answer, and the operation succeeds

#### Scenario: Nobody answers

- **WHEN** the requested `.local` host does not answer any of the
  configured attempts
- **THEN** the operation fails with a reason saying the name did not
  answer over mDNS and how many attempts were made, no TCP connection is
  attempted, and the seat is freed

#### Scenario: Retrying is turned off

- **WHEN** the configured number of attempts is one and that lookup gets
  no answer
- **THEN** the operation fails after that single lookup

#### Scenario: mDNS is not built in

- **WHEN** a build without mDNS resolution receives a connect for a
  `.local` target
- **THEN** the operation fails with a reason saying mDNS is not built in

#### Scenario: The device also advertises itself over mDNS

- **WHEN** the application runs Zephyr's mDNS responder and an operator
  opens a session to another host's `.local` name
- **THEN** the name resolves and the session is bridged, and the device
  keeps answering for its own `.local` name

