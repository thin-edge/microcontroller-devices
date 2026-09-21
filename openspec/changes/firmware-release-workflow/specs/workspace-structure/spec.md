## ADDED Requirements

### Requirement: Device-management-only application

The workspace SHALL provide `apps/tedge-agent`, an application that composes
`lib/common/` with the `tedge-zephyr` module and **no** protocol frontend
library, for devices whose job is to be managed from the cloud and to give
it remote access to LAN hosts. It SHALL declare its own firmware name via
`CONFIG_APP_FIRMWARE_NAME`, use the same provisioning hand-off and MCUboot
layout as the protocol applications, and keep its `tedge-zephyr` glue in the
application, not in the module.

#### Scenario: Composition

- **WHEN** `apps/tedge-agent/` is built
- **THEN** it links `lib/common/` and `tedge-zephyr` and none of
  `lib/opcua/`, `lib/modbus/` or `lib/snmp/`

#### Scenario: Provisioned like the other apps

- **WHEN** a `tedge-agent` image with no credentials boots
- **THEN** it hands off to the provisioner exactly as a protocol application
  does, and after lab-ztp-provisioner provisioning it enrols with the
  delivered tenant

### Requirement: Board-level tedge settings are shared across applications

Every application's `tedge` builds SHALL take the per-board settings a
`tedge-zephyr` build requires (TLS heap size and location, PSRAM, TLS context
count, network buffer depths, heap pool) from one application-independent
file per board under `lib/common/tedge-boards/`. An
application SHALL add only its own measured deltas on top.

#### Scenario: Same board, different apps

- **WHEN** `modbus-server` and `snmp-agent` are built as `tedge-full` for the
  ESP32-S3-DevKitC
- **THEN** both use `lib/common/tedge-boards/` settings for that board, with
  the mbedTLS heap in PSRAM
