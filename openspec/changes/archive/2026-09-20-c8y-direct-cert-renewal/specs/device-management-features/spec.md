## MODIFIED Requirements

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
