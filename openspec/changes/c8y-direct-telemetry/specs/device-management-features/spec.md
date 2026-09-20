## MODIFIED Requirements

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
