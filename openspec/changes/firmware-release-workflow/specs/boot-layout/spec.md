## MODIFIED Requirements

### Requirement: Development signing key is marked as such

Images SHALL be signed during the build. Until the OTA change introduces key
management, local builds SHALL use MCUboot's development key, and the
documentation SHALL state that this key is public and must not be used for
production devices. Release builds SHALL use the development key unless a
signing key is configured for the repository, and every release SHALL state
which key signed it: the public development key, with the same warning, or
the configured key's SHA-256 fingerprint.

#### Scenario: Documentation warns about the key

- **WHEN** a developer reads the flashing instructions for a BLE board
- **THEN** they are told the images are signed with a public development key
  that is not suitable for production

#### Scenario: Release signed with the development key

- **WHEN** a release is built with no signing key configured
- **THEN** its notes and each bundle's `flash.json` state that the images are
  signed with the public MCUboot development key and are not for production

#### Scenario: Release signed with a configured key

- **WHEN** a release is built with the repository signing key configured
- **THEN** the bootloader, application and provisioner images are signed with
  that key, and the release notes give its SHA-256 fingerprint

### Requirement: Flashing a board

The project SHALL provide one documented command per board that flashes the
bootloader, the application into the primary slot and the provisioner into the
provisioning partition at the correct offsets, and an option to erase the
storage and boot-request partitions. The same command SHALL accept either a
local sysbuild build directory or an extracted release bundle, taking the
chip and offsets from the bundle's `flash.json` in the latter case. A release
SHALL also be flashable without the repository, with a single documented
`esptool` command writing its merged image at offset `0x0`.

#### Scenario: Fresh board

- **WHEN** a developer flashes a board with the documented command and no
  compile-time credentials
- **THEN** the device boots the application, which finds no credentials and
  reboots into the provisioner, ready to be provisioned

#### Scenario: Flashing a release bundle

- **WHEN** a user runs `scripts/flash.sh <extracted-bundle> --erase-all`
- **THEN** MCUboot, the application and the provisioner are written at the
  offsets in `flash.json`, and the device boots into the provisioner

#### Scenario: Flashing a merged image without the repository

- **WHEN** a user with only `esptool` erases the flash and writes a release's
  `<stem>.factory.bin` at `0x0`
- **THEN** the device boots the application, finds no credentials and
  reboots into the provisioner, the same as a bundle flashed with `flash.sh`

#### Scenario: Bundle for a different flash size

- **WHEN** `flash.sh` is given a bundle whose `flash.json` declares a larger
  flash size than the connected chip reports
- **THEN** it refuses to flash and names both sizes
