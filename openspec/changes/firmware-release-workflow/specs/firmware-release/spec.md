## ADDED Requirements

### Requirement: Supported devices are declared in one manifest

The repository SHALL declare every releasable device in `release/devices.yml`,
giving for each device an id, a display name, the Zephyr board target, the
esptool chip, the flash size, any extra devicetree overlays, and the list of
(app, variant) builds it ships with any extra Kconfig overlays per build. The
build matrix, the release notes and the README's supported-devices table
SHALL be derived from this file. A device or build not in the manifest SHALL
NOT be released.

#### Scenario: Adding a device

- **WHEN** a maintainer adds a device entry whose board already has a
  provisioner layout and board files
- **THEN** the next CI run builds it and the next release includes it, with
  no change to the workflow file

#### Scenario: Invalid manifest

- **WHEN** the manifest names an app that does not exist, an overlay file that
  does not exist, a board without a provisioner layout, or a duplicate device id
- **THEN** the matrix job fails with a message naming the offending entry, and
  no build runs

### Requirement: Three variants per build

Each build SHALL be one of three variants. A `standalone` build SHALL contain
a protocol app with the Improv Wi-Fi provisioner and no `tedge-zephyr` code.
A `tedge-full` build SHALL contain every `tedge-zephyr` feature
(`profiles/full.conf`). A `tedge-ota` build SHALL contain at least
connection, inventory, health, telemetry, restart and firmware update
(`profiles/ota.conf`), plus the extra features its manifest entry lists.
Both `tedge` variants SHALL use the lab-ztp-provisioner provisioner
(`overlay-ztp.conf`) and the device's shared board-level `tedge` settings.
The release notes SHALL list each `tedge` image's features as read from its
build configuration. No release image SHALL contain Wi-Fi credentials, a
Cumulocity tenant, or any other site-specific setting.

#### Scenario: Protocol apps on the C6 and S3 boards

- **WHEN** a release is built
- **THEN** for each of the ESP32-C6, ESP32-S3-DevKitC and QT Py ESP32-S3,
  and each of `modbus-server`, `opcua-server` and `snmp-agent`, there is a
  `standalone` and a `tedge-ota` image, and a `tedge-full` image wherever
  its capacity measurement passed

#### Scenario: Most capable image on a constrained board

- **WHEN** `tedge-full` does not fit or run on a board
- **THEN** that board's `tedge-ota` image for the app carries every further
  feature that passed measurement there, and the notes list them

#### Scenario: Standalone image

- **WHEN** a `standalone` image is flashed and provisioned over Improv
- **THEN** the device serves its protocol on the provisioned network and makes
  no connection to Cumulocity

#### Scenario: tedge image gets its tenant from provisioning

- **WHEN** a `tedge` image is flashed and provisioned by lab-ztp-provisioner
- **THEN** the device joins the network and enrols with the tenant delivered
  in the ZTP bundle, without any tenant having been compiled in

#### Scenario: No secrets in the build

- **WHEN** a release build runs
- **THEN** it passes no Wi-Fi, tenant or `*.local.conf` overlay, and the
  resulting `.config` has empty `CONFIG_APP_WIFI_SSID` and `CONFIG_TEDGE_C8Y_URL`

### Requirement: A device-management-only application is released

The project SHALL release `apps/tedge-agent` — `lib/common/` plus
`tedge-zephyr`, no protocol frontend — as `tedge-full` and `tedge-ota` on
every board where they pass capacity measurement.

#### Scenario: Remote access without a protocol app

- **WHEN** a `tedge-agent` image with remote access is flashed and
  provisioned over lab-ztp-provisioner
- **THEN** the device enrols, and an operator can open an SSH session to a
  LAN host through it from Cumulocity

### Requirement: Every tedge build is measured on its board

A `tedge-*` build SHALL enter the manifest only with a reference to a
DEVICES.md entry recording, for that device, app and variant: link fit
(RAM regions and `slot0` use), and a run on the board covering connection,
ten minutes of telemetry, a firmware update that completes and confirms,
a remote-access tunnel when that feature is included, and a protocol read
during the update or tunnel for protocol apps, with the minimum free heap
observed. The matrix job SHALL reject a `tedge-*` build without that
reference.

#### Scenario: A build that links but fails at runtime

- **WHEN** a WROOM `tedge-ota` protocol image links but its firmware download
  fails for lack of heap
- **THEN** it is recorded as failed in DEVICES.md and does not enter the
  manifest

#### Scenario: Unmeasured build

- **WHEN** a manifest entry adds a `tedge-*` build without a `measured`
  reference
- **THEN** the matrix job fails and names the entry

### Requirement: Releases are built from a version tag

The workflow SHALL build every manifest entry and publish a GitHub Release
with all assets attached when a tag of the form `vMAJOR.MINOR.PATCH` or
`vMAJOR.MINOR.PATCH-<pre>` is pushed. A pre-release tag SHALL publish a pre-release. The
release SHALL NOT be published unless every build succeeded. A tag of any
other form SHALL fail without building.

#### Scenario: Successful release

- **WHEN** `v0.4.0` is pushed and every build succeeds
- **THEN** a release `v0.4.0` exists with, for every manifest build, a
  factory image, an application image and a bundle, plus
  `c8y-firmware.json` and `SHA256SUMS`

#### Scenario: One board fails

- **WHEN** one build in the matrix fails
- **THEN** the other builds still run to completion and report, and no
  release is published

#### Scenario: Pre-release

- **WHEN** `v0.4.0-rc1` is pushed
- **THEN** the release is marked as a pre-release

### Requirement: One version for all applications

All `apps/*/VERSION` files SHALL hold the same `MAJOR.MINOR.PATCH`, changed
only by the release bump script in a commit. A release tag `vX.Y.Z[-pre]`
SHALL only build if every file holds `X.Y.Z`; the build SHALL NOT change the
version numbers, and only sets `EXTRAVERSION` in its own checkout. `<pre>`
SHALL be lowercase letters, digits and dots, which Zephyr accepts as
`EXTRAVERSION`. Every image SHALL carry that version in its MCUboot header
and its `APP_VERSION_STRING`, with `-<pre>` appended to the latter for a
pre-release tag and `-dev` for a non-tag build. `tedge-zephyr/VERSION` SHALL
be versioned independently, and the release notes SHALL state it for every
`tedge` image.

#### Scenario: Bumping for a release

- **WHEN** a maintainer runs the bump script with `0.4.0`
- **THEN** all five application VERSION files read `0.4.0` and nothing else
  changes

#### Scenario: Tag does not match the files

- **WHEN** `v0.5.0` is pushed on a commit whose VERSION files say `0.4.0`
- **THEN** the workflow fails before building and names the mismatch

#### Scenario: Files disagree

- **WHEN** a pull request leaves `apps/snmp-agent/VERSION` at a different
  version from the other apps
- **THEN** the release-matrix check fails and names the file

#### Scenario: Version reported in the cloud

- **WHEN** a `tedge` image from release `v0.4.0` connects to Cumulocity
- **THEN** the device's firmware version is shown as `0.4.0`

#### Scenario: Update from the previous release

- **WHEN** a device running release `v0.4.0` is offered the `v0.4.1`
  application image through Cumulocity firmware update
- **THEN** the update is accepted, since the versions differ

#### Scenario: CI artifact

- **WHEN** a build runs for a pull request
- **THEN** its `APP_VERSION_STRING` ends in `-dev` and its artifact names
  say `dev`

### Requirement: Release assets are complete and named consistently

For every build, the release SHALL attach, named with the stem
`<app>-<variant>-<device>-<version>`:

- `<stem>.factory.bin` — bootloader, application and provisioner merged into
  one image to be written at `0x0`;
- `<stem>.app.bin` — the signed application image alone;
- `<stem>.zip` — the bootloader, the signed application, the signed
  provisioner, a `flash.json` giving the chip, flash mode, frequency and size,
  each image's offset and partition, the signing key statement, and a
  `README.txt` with the flash commands.

The release SHALL attach `SHA256SUMS` covering every asset.

#### Scenario: Offsets match the build

- **WHEN** a bundle is produced for the ESP32-WROOM-32
- **THEN** `flash.json` places the bootloader at `0x1000` and the application
  and provisioner at the `slot0_partition` and `prov_partition` offsets of
  that build's devicetree

#### Scenario: Checksums

- **WHEN** a user runs `sha256sum -c SHA256SUMS` over the downloaded assets
- **THEN** every asset verifies

### Requirement: The factory image flashes from a browser

Each `<stem>.factory.bin` SHALL be a single raw image to be written at
`0x0`: every part at its absolute offset, gaps padded, the bootloader's flash
mode, frequency and size set for the device, and no data at or beyond the
`storage` partition. It SHALL be flashable, unchanged, by a Web Serial
browser flasher that writes one file at `0x0`, such as ESPHome Web
(web.esphome.io). The README SHALL document that path, including erasing on
a first install and provisioning over BLE afterwards.

#### Scenario: Flashing from ESPHome Web

- **WHEN** a user on Chrome connects a supported device in ESPHome Web,
  chooses "Install", erases, and selects the device's downloaded
  `<stem>.factory.bin`
- **THEN** the device boots the application, finds no credentials and
  reboots into the BLE provisioner, the same as after an `esptool` flash

#### Scenario: Classic ESP32 needs no offset

- **WHEN** the WROOM-32 factory image is written at `0x0`
- **THEN** MCUboot is found at `0x1000` and the device boots, without the user
  entering any offset

#### Scenario: Wi-Fi step after flashing

- **WHEN** ESPHome Web offers to configure Wi-Fi after flashing and cannot
- **THEN** the README explains that these images are provisioned over BLE and
  links the provisioning instructions

### Requirement: tedge images can be installed over the air from Cumulocity

Every `tedge` build SHALL report the firmware name
`<app>-<variant>-<device>` (at most 47 characters), unique per (app,
device). Every release SHALL attach `c8y-firmware.json` listing, per `tedge`
build, the firmware name, the version exactly as the image reports it
(`X.Y.Z` or `X.Y.Z-<pre>`), the device type, the `.app.bin` asset name and
its SHA-256. The project SHALL provide a user-run script that uploads a
release's `tedge` application images, pre-releases included, into a
Cumulocity tenant's firmware repository under those names and versions,
idempotently. The workflow itself SHALL hold no Cumulocity credentials.

#### Scenario: Upload and install

- **WHEN** a user runs the upload script for `v0.4.0` against their tenant
  and then installs `modbus-server-tedge-ota-esp32c6-devkitc` `0.4.0` on a C6
  running an earlier `tedge` image signed with the same key
- **THEN** the device downloads the image from the tenant, reboots into it,
  confirms it, reports `modbus-server-tedge-ota-esp32c6-devkitc 0.4.0`, and the
  operation succeeds

#### Scenario: Only matching images are offered

- **WHEN** an operator opens the firmware repository entry named after a
  device's reported firmware name
- **THEN** every version in it was built for that device and app

#### Scenario: Running the upload twice

- **WHEN** the upload script is run again for a release already uploaded
- **THEN** it creates nothing new and reports the existing versions

#### Scenario: Pre-release over the air

- **WHEN** `v0.4.0-rc1` is uploaded and installed on a device running
  `0.3.x` or `0.4.0-rc0`
- **THEN** the operation succeeds and the device reports `0.4.0-rc1`

#### Scenario: From a pre-release to the final release

- **WHEN** `0.4.0` is installed on a device running `0.4.0-rc1`
- **THEN** the install is accepted, succeeds, and the device reports `0.4.0`

#### Scenario: Checksum mismatch

- **WHEN** a downloaded `.app.bin` does not match `SHA256SUMS`
- **THEN** the script uploads nothing and names the file

### Requirement: Builds are gated on size

Each build SHALL fail if its application image exceeds 95 % of `slot0`,
in addition to any link-time overflow. The release notes SHALL list, per
build, the application image size against `slot0` and the RAM used.

#### Scenario: Image grows past the OTA margin

- **WHEN** a change makes an application image 96 % of its `slot0`
- **THEN** that build fails and names the image, its size and the slot size

#### Scenario: Size table

- **WHEN** a release is published
- **THEN** its notes contain one row per build with flash and RAM use

### Requirement: The release matrix is exercised before a tag

The workflow SHALL build, without publishing, the builds marked `pr: true`
in the manifest on pull requests that change application, library, module,
sysbuild, manifest, west manifest or release-tooling files, and every build
on manual dispatch unless the subset is chosen. The `pr` subset SHALL
include at least one build of every device. The build artifacts of such
runs SHALL be downloadable from the run.

#### Scenario: A PR breaks a released board

- **WHEN** a pull request changes `lib/common/` in a way that stops a build in
  the `pr` subset from linking
- **THEN** that pull request's release-matrix check fails

#### Scenario: A device without a pull-request build

- **WHEN** the manifest marks no build of a device `pr: true`
- **THEN** the matrix job fails and names the device

### Requirement: Builds are reproducible from the tag

Release builds SHALL use a pinned Zephyr SDK version, a pinned runner image
and the Zephyr workspace revisions pinned by `west.yml`, so that rebuilding
the same tag uses the same toolchain, Zephyr and module sources.

#### Scenario: SDK is pinned

- **WHEN** a new Zephyr SDK is released
- **THEN** release builds are unaffected until the pinned version is updated
  in a pull request
