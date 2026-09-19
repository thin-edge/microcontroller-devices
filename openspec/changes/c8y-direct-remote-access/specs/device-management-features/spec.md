## MODIFIED Requirements

### Requirement: Remote access forwards to hosts on the local network under a target policy

When the remote-access feature is built in, the device SHALL act as a
remote-access endpoint for Cumulocity. On a remote-access connect operation, it
SHALL open a TCP connection to the requested host and port and bridge it to
Cumulocity's device-side WebSocket until either side closes. The target MAY be
the device itself or another host on its network. Which targets are allowed
SHALL be set by a build-time policy: the device's own IPv4 subnets (default),
an explicit allow-list, or the device itself only. The host application SHALL
be able to narrow the policy further. The device SHALL refuse a target outside
the policy, and SHALL mark the operation as failed with a reason. Each tunnel
open and close SHALL be recorded as an event naming the target. The number of
concurrent sessions SHALL be capped by a Kconfig option.

#### Scenario: SSH to a neighbouring host

- **WHEN** a user opens a Cumulocity remote-access SSH session to the device
  with an endpoint of `192.168.1.20:22`, and that host is on the device's subnet
- **THEN** the device bridges the session to that host, and the user reaches
  the host's SSH server
- **AND** an event records the tunnel opening and closing with the target

#### Scenario: Target outside the policy

- **WHEN** a remote-access connect names a target outside the configured policy
- **THEN** the device opens no TCP connection, and marks the operation as failed
  with a reason naming the policy

#### Scenario: Session cap reached

- **WHEN** a remote-access connect arrives while the maximum number of sessions
  is open
- **THEN** the device marks the new operation as failed, and the open sessions
  continue undisturbed
