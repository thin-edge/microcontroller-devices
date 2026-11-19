## RENAMED Requirements

- FROM: `### Requirement: Three variants per build`
- TO: `### Requirement: Variants name the operating system and the feature set`

## MODIFIED Requirements

### Requirement: Variants name the operating system and the feature set

Each build SHALL be one of these variants, named `<os>-<set>`:

| Variant | Operating system | Contents | Set-up |
|---|---|---|---|
| `zephyr-standalone` | Zephyr | a protocol app, no `tiny-edge` code | Improv Wi-Fi over BLE |
| `zephyr-full` | Zephyr | every `tiny-edge` feature (`profiles/full.conf`) | lab-ztp-provisioner |
| `zephyr-ota` | Zephyr | at least connection, inventory, health, telemetry, restart and firmware update (`profiles/ota.conf`), plus the extra features its manifest entry lists | lab-ztp-provisioner |
| `idf-full` | ESP-IDF (FreeRTOS) | the device agent with every `tiny-edge` feature | `tedge-onboard` over USB |
| `nuttx-full` | NuttX | tiny-edge's NuttX agent with every feature | `tedge-onboard` over USB |

`<os>` SHALL follow from the variant's framework (`zephyr`, `idf` for
ESP-IDF, `nuttx`) and `<set>` SHALL be `standalone`, `full` or `ota`; the
manifest SHALL be rejected when a variant's name does not. The Zephyr
`full` and `ota` variants SHALL use the device's shared board-level `tedge`
settings. Every build SHALL be named `<app>-<variant>-<device>`, where
`<app>` is the app's name in the manifest (the ESP-IDF agent in
`apps/tedge-agent-idf/` is `tedge-agent`), and no two builds SHALL share a
name. The release notes SHALL list each `tedge` image's features as read
from its build configuration. No release image SHALL contain Wi-Fi
credentials, a Cumulocity tenant, or any other site-specific setting.

#### Scenario: Protocol apps on the C6 and S3 boards

- **WHEN** a release is built
- **THEN** for each of the ESP32-C6, ESP32-S3-DevKitC and QT Py ESP32-S3,
  and each of `modbus-server`, `opcua-server` and `snmp-agent`, there is a
  `zephyr-standalone` and a `zephyr-ota` image, and a `zephyr-full` image
  wherever its capacity measurement passed

#### Scenario: One app on several operating systems

- **WHEN** a release has the device agent for the ESP32-C6 on Zephyr,
  ESP-IDF and NuttX
- **THEN** its images are `tedge-agent-zephyr-full-esp32c6-devkitc`,
  `tedge-agent-idf-full-esp32c6-devkitc` and
  `tedge-agent-nuttx-full-esp32c6-devkitc`, and so are the firmware names
  the devices report

#### Scenario: A variant name that does not match its build

- **WHEN** the manifest names a NuttX variant `zephyr-full`, or gives two
  builds the same firmware name
- **THEN** the matrix job fails with a message naming the variant or the
  builds, and no build runs

#### Scenario: Most capable image on a constrained board

- **WHEN** `zephyr-full` does not fit or run on a board
- **THEN** that board's `zephyr-ota` image for the app carries every further
  feature that passed measurement there, and the notes list them

#### Scenario: Standalone image

- **WHEN** a `zephyr-standalone` image is flashed and provisioned over Improv
- **THEN** the device serves its protocol on the provisioned network and makes
  no connection to Cumulocity

#### Scenario: tedge image gets its tenant from provisioning

- **WHEN** a `zephyr-full` or `zephyr-ota` image is flashed and provisioned
  by lab-ztp-provisioner
- **THEN** the device joins the network and enrols with the tenant delivered
  in the ZTP bundle, without any tenant having been compiled in

#### Scenario: No secrets in the build

- **WHEN** a release build runs
- **THEN** it passes no Wi-Fi, tenant or `*.local.conf` overlay, and the
  resulting `.config` has empty `CONFIG_APP_WIFI_SSID` and `CONFIG_TEDGE_C8Y_URL`

### Requirement: Release assets are complete and named consistently

For every build, the release SHALL attach, named with the stem
`<app>-<os>-<set>-<device>-<version>` (the firmware name, then the
version):

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

### Requirement: tedge images can be installed over the air from Cumulocity

Every `tedge` build SHALL report the firmware name
`<app>-<os>-<set>-<device>` (at most 47 characters), unique per build. Every release SHALL attach `c8y-firmware.json` listing, per `tedge`
build, the firmware name, the version exactly as the image reports it
(`X.Y.Z` or `X.Y.Z-<pre>`), the device type, the `.app.bin` asset name and
its SHA-256. The project SHALL provide a user-run script that uploads a
release's `tedge` application images, pre-releases included, into a
Cumulocity tenant's firmware repository under those names and versions,
idempotently. The workflow itself SHALL hold no Cumulocity credentials.

#### Scenario: Upload and install

- **WHEN** a user runs the upload script for `v0.4.0` against their tenant
  and then installs `modbus-server-zephyr-ota-esp32c6-devkitc` `0.4.0` on a C6
  running an earlier `tedge` image signed with the same key
- **THEN** the device downloads the image from the tenant, reboots into it,
  confirms it, reports `modbus-server-zephyr-ota-esp32c6-devkitc 0.4.0`, and the
  operation succeeds

#### Scenario: Only matching images are offered

- **WHEN** an operator opens the firmware repository entry named after a
  device's reported firmware name
- **THEN** every version in it was built for that device, app and operating
  system

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

#### Scenario: Moving a device to the new names

- **WHEN** a device running `modbus-server-tedge-ota-esp32c6-devkitc` (a name
  from before 0.17.0) is sent `modbus-server-zephyr-ota-esp32c6-devkitc`
  `0.17.0`
- **THEN** it installs it, confirms it, and reports
  `modbus-server-zephyr-ota-esp32c6-devkitc 0.17.0`
