## ADDED Requirements

### Requirement: Advertise a discoverable hostname over mDNS
Once the device has joined the network, the firmware SHALL advertise a
configurable mDNS hostname (default derived from the device name) so it can be
reached as `<hostname>.local` without knowing its IP address in advance.

#### Scenario: Resolve device by hostname
- **WHEN** the device is running on the network and a client on the same LAN queries mDNS for `<hostname>.local`
- **THEN** the query SHALL resolve to the device's current IP address

#### Scenario: Hostname is configurable
- **WHEN** the mDNS hostname is changed via configuration and the firmware is rebuilt/flashed
- **THEN** the device SHALL advertise the new hostname

### Requirement: Advertise the OPC-UA service via DNS-SD
The firmware SHALL advertise its OPC-UA server as a DNS-SD service of type
`_opcua-tcp._tcp` on the configured port, so service-discovery tools and
mDNS-aware OPC-UA clients can find the endpoint without prior configuration.

#### Scenario: Service appears in DNS-SD browse
- **WHEN** a client browses DNS-SD for `_opcua-tcp._tcp` on the local network
- **THEN** the device's OPC-UA service SHALL be listed with a resolvable host and the correct port

#### Scenario: Discovered endpoint is usable
- **WHEN** a client resolves the advertised OPC-UA service and connects to the resolved host/port
- **THEN** the OPC-UA server SHALL accept the connection and serve read requests

### Requirement: Advertisement reflects connectivity changes
The mDNS/DNS-SD advertisement SHALL reflect the device's current network
presence: it is advertised while connected and updated (or withdrawn/re-announced)
across reconnects, so stale records do not persist indefinitely.

#### Scenario: Re-announce after reconnect
- **WHEN** the device loses and re-establishes its Wi-Fi connection (potentially with a new IP)
- **THEN** the mDNS/DNS-SD records SHALL be re-announced so discovery resolves to the current address
