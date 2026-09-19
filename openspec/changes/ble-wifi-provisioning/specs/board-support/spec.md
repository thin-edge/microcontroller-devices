## MODIFIED Requirements

### Requirement: Declared flash size matches the physical part

A board port SHALL ensure the `flash0` size seen by the build matches the flash
actually fitted to the board in hand, as read from the part itself. When the
upstream board devicetree assumes a different module variant, the port SHALL
correct the size in its overlay, in **either** direction — a devicetree may
overstate or understate the fitted part. On a board with BLE provisioning the
correction is carried by the board's shared provisioner layout (see "Boards with
BLE provisioning add a shared layout"), which declares the size together with
the partitions it needs.

#### Scenario: Board devicetree overstates the fitted flash

- **WHEN** a board's devicetree declares 8 MB (an N8 variant) but the module in
  hand is a 4 MB N4 part
- **THEN** the port's overlay sets `&flash0` to the real 4 MB size
- **AND** the partition table still fits entirely within the corrected size

#### Scenario: Board devicetree understates the fitted flash

- **WHEN** a board's devicetree declares 8 MB but the module in hand is a 16 MB
  N16R8 part
- **THEN** the port's overlay sets `&flash0` to the real 16 MB size
- **AND** the partition layout is left unchanged, since it occupies only the low
  region of flash either way, unless the board has BLE provisioning, whose
  layout uses the corrected size

#### Scenario: Corrected size conflicts with the board's partition table

- **WHEN** correcting the flash size conflicts with the partition layout the
  board includes
- **THEN** the conflict is recorded and the uncorrected size retained, rather
  than the partition table being rewritten as part of a board port

## ADDED Requirements

### Requirement: Boards with BLE provisioning add a shared layout

A board that supports BLE provisioning SHALL have a partition layout in
`lib/common/dts/layout-<soc>-<size>.dtsi` (the MCUboot, application,
provisioner, boot-request and storage partitions) and an entry mapping its board
target to that layout in `sysbuild/provisioning.cmake`. Every application's board
overlay for that board, the provisioner's board overlay, and MCUboot SHALL use
the same layout file. These two shared files are the only shared-code changes a
BLE board port makes; the rest stays per-application board files as for any
board. A board without such an entry SHALL build as before without `--sysbuild`,
and a `--sysbuild` build for it SHALL stop with a message naming the missing
layout.

#### Scenario: Layout shared by all images

- **WHEN** an application is built with `--sysbuild` for the ESP32-C6
- **THEN** MCUboot, the application and the provisioner are all built with
  `lib/common/dts/layout-esp32c6-4M.dtsi`

#### Scenario: Board without a layout

- **WHEN** an application is built with `--sysbuild` for a board that has no
  entry in `sysbuild/provisioning.cmake` (e.g. the ESP32-S2 Feather, which has
  no BLE radio)
- **THEN** the build stops with a message saying the board has no MCUboot +
  Wi-Fi provisioner layout
- **AND** the same application still builds for that board without
  `--sysbuild`
