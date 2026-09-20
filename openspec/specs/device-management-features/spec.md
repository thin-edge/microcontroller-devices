# device-management-features Specification

## Purpose
Which device-management features the tedge-zephyr client offers, how each one is selected at build time so a constrained board carries only what fits, the transport and authentication rules between them, and how state such as remote-access capacity is published. Measured costs and board profiles come from `c8y-direct-spikes`.
## Requirements
### Requirement: The device-management client is optional and off by default

The device-management client SHALL be the `tedge-zephyr` Zephyr module, which an
application includes only when the umbrella option `CONFIG_TEDGE` enables it.
When that option is disabled, any application SHALL build exactly as it does
without the module: no extra code, RAM, threads, sockets or TLS sessions.

#### Scenario: Application built without device management

- **WHEN** an application is built with `CONFIG_TEDGE` disabled
- **THEN** its flash and static RAM match the same build without `tedge-zephyr`
  within a few hundred bytes
- **AND** no device-management thread, socket or TLS session exists at runtime

#### Scenario: Application built with device management

- **WHEN** an application is built with `CONFIG_TEDGE` enabled
- **THEN** the selected transport, authentication method and features are
  compiled in, and the application's own behaviour is unchanged

### Requirement: Each feature is selected independently at build time

Each device-management feature SHALL have its own Kconfig option, so that it can
be included in or excluded from an image independently of the others. The
features are telemetry, device-health telemetry, restart, firmware update,
shell command, log upload, remote access, configuration management and
certificate renewal. The transport (direct to Cumulocity, or via a thin-edge.io
gateway) and the authentication method (Cumulocity CA certificate, or bootstrap
basic-auth credentials) SHALL each be a Kconfig choice. A feature that has not
been implemented yet SHALL be present in the menu but not selectable, and
the public API calls that belong to it SHALL return "not supported" rather
than failing silently.

#### Scenario: Feature removed to save resources

- **WHEN** an image is built with only remote access disabled
- **THEN** the remote-access code, its WebSocket client, its bridge thread and
  its TLS session are absent from the image
- **AND** every other selected feature is still built and works

#### Scenario: Minimal image

- **WHEN** an image is built with only the connection, inventory and restart
  enabled
- **THEN** it links and runs with a single MQTT/TLS session and no HTTP client

### Requirement: Advertised capabilities match the build

The device SHALL advertise to the cloud or gateway only the operations and
capabilities of features compiled into the running image. This covers supported
operations, log types and configuration types. When it receives an operation for
a feature that is not compiled in, the device SHALL mark that operation as
failed with a reason that names the missing feature. It SHALL NOT ignore the
operation or leave it pending.

#### Scenario: Firmware update advertises itself and the running version

- **WHEN** an image with the firmware-update feature connects
- **THEN** its supported operations include firmware update, and it reports
  the name and version of the image that is running

#### Scenario: Supported operations reflect the image

- **WHEN** an image without the firmware-update feature connects
- **THEN** its supported-operations list does not include firmware update

#### Scenario: Operation for an absent feature

- **WHEN** the device receives an operation whose feature is not compiled in
- **THEN** it marks the operation as failed with a reason naming the feature

### Requirement: Feature dependencies are enforced at configure time

Dependencies between features and on the platform SHALL be expressed in Kconfig
(`depends on`/`select`), so that an invalid combination fails when the build is
configured, not at runtime. Firmware update SHALL require an MCUboot build.
Remote access and certificate renewal SHALL require certificate (CA)
authentication, because both need a token that only certificate devices
receive. Features that
transfer files (firmware update, log upload, configuration management) SHALL
bring in the HTTP client. Both authentication methods SHALL stay selectable.
The Cumulocity MQTT Service endpoint SHALL require certificate
authentication, and a basic-auth build SHALL use Core MQTT without further
configuration.

#### Scenario: Firmware update without MCUboot

- **WHEN** a build enables firmware update for a board built without sysbuild
  or MCUboot
- **THEN** configuration fails, or the option cannot be selected, with a
  message naming the missing MCUboot dependency

#### Scenario: Basic authentication selects Core MQTT

- **WHEN** a build selects bootstrap basic authentication and sets no endpoint
- **THEN** the build uses the Core MQTT endpoint, and the MQTT Service
  endpoint cannot be selected

#### Scenario: Remote access with basic authentication

- **WHEN** a build selects bootstrap basic authentication and remote access
- **THEN** remote access cannot be selected

#### Scenario: Certificate renewal with basic authentication

- **WHEN** a build selects bootstrap basic authentication and certificate
  renewal
- **THEN** certificate renewal cannot be selected, since such a device has
  no certificate to renew

### Requirement: Per-feature footprint is measured and documented

The repository SHALL document, for each board and application, the flash, static
RAM and peak heap that each device-management feature adds. The same script
SHALL regenerate the table, as `scripts/measure_prov.sh` does for provisioning.
The table SHALL be updated whenever a feature is added or its cost changes
materially.

#### Scenario: Footprint table regenerated

- **WHEN** a developer runs the measurement script for a board and application
- **THEN** it prints the size of each feature's delta against a build without
  that feature, using the same format as the documented table

### Requirement: Boards ship with a feature profile that fits them

Each supported board and application combination SHALL have a documented default
device-management profile, provided as a Kconfig overlay, that fits its measured
RAM and flash budget with headroom. A profile SHALL never select more concurrent
TLS sessions than the board's measured heap supports. The profiles SHALL follow
the measured results of `c8y-direct-spikes`: the ESP32-C6 uses the full
profile; the ESP32-S3-DevKitC-1 uses the full profile with the mbedTLS heap in
PSRAM; the ESP32-WROOM-32 uses the direct transport only as a remote-access
enabler with no protocol application, and otherwise the gateway transport.

#### Scenario: Constrained board

- **WHEN** the ESP32-WROOM-32 OPC-UA application is built with its default
  profile
- **THEN** the profile excludes the direct transport's TLS session, and the
  image boots and serves OPC-UA as before

#### Scenario: Larger board

- **WHEN** the ESP32-C6 application is built with its default profile
- **THEN** all implemented features are included, and the documented free heap
  after connecting stays above the margin set in the footprint table

#### Scenario: Board with PSRAM

- **WHEN** the ESP32-S3-DevKitC-1 application is built with its default
  profile
- **THEN** the mbedTLS heap is placed in PSRAM, and all implemented features
  are included without overflowing internal RAM

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

### Requirement: State and telemetry use thin-edge.io topics on the MQTT Service

The device SHALL publish telemetry, twin data, health and events on free-form
topics in thin-edge.io's `te/` topic and payload shape when it uses the direct
transport and the Cumulocity MQTT Service endpoint, so that a thin-edge.io gateway
and Cumulocity Smart Functions can map the same messages. Operations and their
status SHALL stay on SmartREST. Without free-form topics (Core MQTT, basic
authentication), the device SHALL send twin data as direct inventory updates
and telemetry as SmartREST measurements. The device SHALL republish its twin
data after every (re)connect, and SHALL NOT rely on retained messages.

#### Scenario: Twin data on the MQTT Service

- **WHEN** the device, connected to the MQTT Service, has twin data to report
- **THEN** it publishes it to `te/device/<id>///twin/<fragment>`, and a
  Smart Function can map it to the managed object

#### Scenario: Twin data on Core MQTT

- **WHEN** the device is connected to Core MQTT
- **THEN** it sends the same twin data as a direct inventory update of its
  managed object

### Requirement: Remote-access capacity and use are published as twin data

When the remote-access feature is built in, the device SHALL publish
`tedge_RemoteAccess` twin data with the session limit, the number of active
sessions and the target policy. It SHALL publish it when it connects, with no
active sessions after a (re)connect, and whenever a session opens or closes.
The list of active sessions with their targets SHALL be a build-time option.

#### Scenario: A session opens

- **WHEN** a remote-access tunnel is established
- **THEN** the device publishes `tedge_RemoteAccess` with `activeSessions`
  increased by one

#### Scenario: The device reconnects

- **WHEN** the device reconnects after a reboot or a lost connection
- **THEN** it publishes `tedge_RemoteAccess` with `activeSessions: 0`,
  replacing any stale value

