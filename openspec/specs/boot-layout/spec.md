# boot-layout Specification

## Purpose
How BLE-capable boards boot and are laid out in flash: MCUboot, A/B application slots that leave `slot1` free for OTA, a separate `prov` partition for the Wi-Fi provisioner image, the shared `storage` partition, and the boot request that tells MCUboot to launch the provisioner instead of the application. Also covers the application identity record the provisioner advertises under, image signing, and flashing.
## Requirements
### Requirement: MCUboot on BLE-capable boards

Boards that support BLE provisioning SHALL boot through MCUboot instead of
Espressif simple boot. MCUboot SHALL verify an image's signature before
launching it, and SHALL refuse to launch an image that fails verification.
Boards without a provisioner (the ESP32-S2 Feather) MAY stay on simple boot.

#### Scenario: Signed application boots

- **WHEN** a board is flashed with MCUboot and an application signed with the
  configured key
- **THEN** MCUboot verifies it and boots it

#### Scenario: Unsigned or corrupted image is refused

- **WHEN** `slot0` holds an image that is unsigned or fails signature checks
- **THEN** MCUboot does not launch it and logs why

### Requirement: Partition layout with OTA slots and a provisioning partition

The flash layout of every BLE-capable board SHALL provide, as separate
partitions: the bootloader; a primary and a secondary application slot of equal
size; a provisioning partition for the provisioner; a settings/NVS storage
partition shared by the application and the provisioner; a boot-request
partition; and the scratch area MCUboot's swap needs. The secondary slot SHALL
be reserved for application updates (OTA) and SHALL NOT hold the provisioner.
Every partition SHALL fit within the flash physically fitted to the board.

#### Scenario: 4 MB board

- **WHEN** the layout for a 4 MB board (WROOM-32, ESP32-C6) is built
- **THEN** all listed partitions fit within 4 MB without overlap
- **AND** each application image of that board fits its slot, and the
  provisioner fits the provisioning partition

#### Scenario: Application swap is unaffected by the provisioner

- **WHEN** an image is placed in the secondary slot and marked for test
- **THEN** MCUboot swaps it into the primary slot and reverts it on the next
  reset if it is not confirmed, exactly as without the provisioner
- **AND** the provisioning and storage partitions are unchanged by the swap

### Requirement: Boot request selects the provisioner

A boot request stored in its own flash partition SHALL tell the bootloader
which image to launch. When the request asks for the provisioner and the
provisioning partition holds a valid, correctly signed image, the bootloader
SHALL launch the provisioner; when no request is set, it SHALL boot the
application through its normal primary/secondary logic. When the request is
set but the provisioner image is invalid, the bootloader SHALL log the problem
and boot the application. The request SHALL survive a power cut. Only the
application SHALL set the request, and only the provisioner SHALL clear it.

#### Scenario: Request set

- **WHEN** the application sets the boot request and reboots
- **THEN** the bootloader launches the provisioner from the provisioning
  partition, not the secondary slot

#### Scenario: Request cleared

- **WHEN** the provisioner clears the request and reboots
- **THEN** the bootloader boots the application

#### Scenario: Missing provisioner

- **WHEN** the request is set but the provisioning partition is erased or holds
  an invalid image
- **THEN** the bootloader boots the application and logs that the provisioner
  is unavailable

### Requirement: Application identity record for the provisioner

Each application SHALL keep an identity record in the shared storage partition
holding its unique hostname, DNS-SD service type and port, and SHALL write it
only when it differs from what is stored. The provisioner SHALL use it for its
BLE name and the service URL it reports.

#### Scenario: Provisioner names itself after the application

- **WHEN** the Modbus application has run at least once and the provisioner
  starts
- **THEN** the provisioner advertises as the Modbus application's hostname and
  reports `modbus://<hostname>.local:502` after provisioning

### Requirement: Development signing key is marked as such

Images SHALL be signed during the build. Until the OTA change introduces key
management, the build SHALL use MCUboot's development key, and the
documentation SHALL state that this key is public and must not be used for
production devices.

#### Scenario: Documentation warns about the key

- **WHEN** a developer reads the flashing instructions for a BLE board
- **THEN** they are told the images are signed with a public development key
  that is not suitable for production

### Requirement: Flashing a board

The project SHALL provide one documented command per board that flashes the
bootloader, the application into the primary slot and the provisioner into the
provisioning partition at the correct offsets, and an option to erase the
storage and boot-request partitions.

#### Scenario: Fresh board

- **WHEN** a developer flashes a board with the documented command and no
  compile-time credentials
- **THEN** the device boots the application, which finds no credentials and
  reboots into the provisioner, ready to be provisioned

