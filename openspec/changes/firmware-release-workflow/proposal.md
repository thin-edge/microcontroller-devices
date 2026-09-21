## Why

Today the only way to run any of the apps is to build them: a Docker
container with the Zephyr SDK, a multi-GB `west update`, blob fetches, a
submodule init, and knowing which board file, profile and provisioner overlay
go together. That is the biggest barrier between "this repo exists" and "a
board on my desk serves Modbus". Since BLE provisioning (Improv and
lab-ztp-provisioner) landed, an image no longer needs site-specific
credentials baked in, so one prebuilt image per device can serve every user —
the missing piece is a pipeline that produces and publishes them.

## What Changes

- Add a **supported-devices manifest** (`release/devices.yml`) that names each
  releasable device and, per device, the Zephyr board target, the apps and
  variants it ships, the extra Kconfig/devicetree overlays that variant needs,
  and the esptool chip and flash parameters. It is the single source of truth
  for the build matrix and the release notes.
- Add a **GitHub Actions release workflow** (`.github/workflows/release.yml`):
  - on a `v*` tag, builds every (device × app × variant) in the manifest with
    `west build --sysbuild` in the `zephyr-build` container, and publishes a
    GitHub Release with the images attached;
  - on `workflow_dispatch` and on pull requests that touch the manifest, the
    workflow or the release scripts, builds the same matrix without
    publishing, so a board that stops building is caught before a tag.
- **Three variants**, each a (feature set, provisioner) pair:
  - `standalone` — a protocol app with the **Improv** BLE provisioner; no
    thin-edge.io/Cumulocity code.
  - `tedge-full` — every `tedge-zephyr` feature (`profiles/full.conf`),
    shipped only where it fits **and runs** on the board.
  - `tedge-ota` — connection, inventory, health, telemetry, restart and
    **firmware update**: the smallest image that can still be updated over
    the air, leaving the most RAM to the app. On boards where `tedge-full`
    doesn't fit, it also carries every further feature that does (listed per
    build in the manifest), so each board gets the most capable image it can
    run.

  Both `tedge` variants use the **lab-ztp-provisioner** BLE provisioner,
  which delivers the Cumulocity tenant at provisioning time, so no tenant is
  baked into the image.
- **A device-management-only app, `apps/tedge-agent`**: `lib/common`
  (Wi-Fi, provisioning hand-off, identity, status LED, button) plus
  `tedge-zephyr`, no protocol frontend. It is for devices whose job is to be
  managed and to give Cumulocity remote access to LAN hosts (the WROOM
  "remote-access enabler" today is a hand-built version of this). Shipped as
  `tedge-full` and `tedge-ota`.
- **Board-level `tedge` settings shared across apps**: the per-board TLS/PSRAM
  /net-buffer settings now in `apps/modbus-server/boards/*_tedge.conf` move to
  one app-independent file per board (`lib/common/tedge-boards/<board>.conf`),
  so all four apps build `tedge` variants from the same, measured settings.
- **Capacity is measured, not assumed**: before a (device, app, variant)
  enters the manifest it is built (link fit) and run on the board (both TLS
  sessions exercised: connect + firmware download, and a remote-access
  tunnel where included). Results go into DEVICES.md. The WROOM-32 rows are
  decided this way, on the rpi5-attached WROOMs.
- **Release assets per build**: a single merged `<stem>.factory.bin` flashed
  at `0x0` for a first flash — usable as-is in browser flashers such as
  [ESPHome Web](https://web.esphome.io/) (Chrome/Edge, Web Serial: "Install"
  → choose the downloaded file), so a user needs neither the repo, Python
  nor `esptool`; the signed application image on its own (for `--app-only`
  updates and **Cumulocity firmware update**), and a bundle with MCUboot, the app,
  the provisioner and a `flash.json` offsets manifest. Plus `SHA256SUMS` and
  a size report (flash/RAM use per build) in the release notes.
- **One version for all apps**: `apps/*/VERSION` (the four apps and the
  provisioner) always hold the same version, set by
  `scripts/release/bump.sh X.Y.Z` in a release commit that is then tagged
  `vX.Y.Z`. CI **checks** the tag against the files instead of rewriting
  them, so the repo, the MCUboot image header, the `c8y_Firmware` version and
  the SNMP/OPC-UA version object always agree. The first release is
  **`v0.4.0`**, above every app's current version (opcua is already 0.3.0).
  `tedge-zephyr/VERSION` stays independent (the module is meant to move to its
  own repository); release notes state which module version each `tedge`
  image contains.
- **Cumulocity OTA-ready `tedge` images**: each `tedge` build reports a
  firmware name unique to its (app, device) — e.g.
  `modbus-server-tedge-ota-esp32c6-devkitc` — so a Cumulocity firmware
  repository entry can only ever be installed on the hardware it was built
  for. Each release carries `c8y-firmware.json` (name, version, device type,
  asset, SHA-256 per `tedge` build), and `scripts/release/c8y-upload.sh
  <tag>` uses go-c8y-cli to create the firmware entries and upload the
  `.app.bin` files into a tenant's firmware repository, from where devices
  install them with the existing `c8y_Firmware` operation. **Pre-releases
  are OTA-installable too** (`0.4.0-rc1`), so a release candidate can be
  trialled on a few devices over the air before the final tag.
- **`tedge-zephyr`: report the full firmware version.** The client reports
  the running version from the MCUboot header, which holds only
  `MAJOR.MINOR.PATCH`; an installed `0.4.0-rc1` would then read as `0.4.0`
  and be recorded as a rollback. It will report the application's version
  string (`tedge_identity.firmware_version`, already supplied by every app)
  instead, falling back to the header when the app gives none.
- Extend **`scripts/flash.sh`** to flash an extracted release bundle (offsets
  from `flash.json`, not from a build tree's devicetree), and document
  flashing a merged image with plain `esptool` for users who have neither
  the repo nor a shell.
- Signing: by default release images are signed with MCUboot's public
  development key, and every release says so. If the repository secret
  `MCUBOOT_SIGNING_KEY` is set, the workflow signs with it instead.

## Target protocols and boards

Apps: `modbus-server` (Modbus TCP), `opcua-server` (OPC-UA), `snmp-agent`
(SNMP) and the new `tedge-agent` (no industrial protocol). Boards: those with
an MCUboot + provisioner layout.

Measured on the boards (DEVICES.md, "Release builds measured on the boards"):

| Device | standalone | tedge-full | tedge-ota |
|---|---|---|---|
| ESP32-C6-DevKitC (N4) | 3 protocol apps | Modbus, SNMP, agent | Modbus, SNMP, agent |
| ESP32-S3-DevKitC-1 (N16R8) | 3 protocol apps | Modbus, SNMP, agent | Modbus, SNMP, agent |
| Adafruit QT Py ESP32-S3 (N4R2) | 3 protocol apps | Modbus, SNMP, agent | Modbus, OPC-UA, SNMP, agent |
| ESP32-WROOM-32 DevKitC | 3 protocol apps | — | Modbus (+ remote access), agent (+ remote access, cert renewal) |
| ESP32-CAM | — | — | SNMP, Modbus, agent |

OPC-UA runs out of memory beside the client on the C6 and the S3-DevKitC at
either level (open62541 cannot create the server at `full` and cannot open
sessions at `ota`), so it ships `standalone` there; only the QT Py's `ota`
image served it. Every `full` build overflows the CAM's dram1.

Release size: 36 builds (15 standalone, 21 tedge).

## Non-goals

- **ESP32-S2 Feather, Raspberry Pi Pico W, `native_sim`.** The S2 has no BLE
  radio or provisioner layout and its Wi-Fi data path is broken upstream; the
  Pico W has never been built; `native_sim` is a dev target.
- **Hosting our own browser flasher (GitHub Pages + ESP Web Tools manifest).**
  Not now (decided). ESPHome Web already takes a local `.factory.bin`.
- **Improv *serial* Wi-Fi setup after a browser flash.** ESPHome Web offers
  Wi-Fi setup over Improv serial once flashing finishes; these images speak
  Improv over **BLE** (or lab-ztp-provisioner), so that step reports the
  device as not configurable and the user provisions over BLE instead.
- **Production key management.** The workflow can use a secret key, but
  generating, rotating and escrowing one, and migrating dev-key devices, is
  the OTA key-management work.
- **Baked-in Wi-Fi or tenant settings.** Release images carry no credentials
  and no tenant; users who want those still build locally.
- **Hardware-in-the-loop tests in CI.** Builds are checked for linking and
  size, not run on boards.
- **CI pushing to a Cumulocity tenant.** The workflow holds no tenant
  credentials; a user runs `c8y-upload.sh` against their own tenant.
- **Installing from a GitHub release URL.** The device trusts only its
  tenant's CA for downloads, so a firmware entry pointing at a GitHub asset
  URL fails the TLS handshake; entries point at binaries uploaded into the
  tenant.
- **OTA of `standalone` images.** They have no cloud client; they are updated
  by flashing.

## Resource constraints

This is where the change is constrained. What decides fit is **internal
DRAM** — above all whether the ~96 KB mbedTLS heap (16 KB TLS records, two
concurrent sessions for firmware download or a tunnel) can live in PSRAM —
not flash (never above 26 %). Known today (DEVICES.md):

| Board | PSRAM | mbedTLS heap | Modbus `tedge-full` DRAM |
|---|---|---|---|
| S3-DevKitC | 8 MB octal | PSRAM | 62.7 % |
| QT Py S3 | 2 MB quad | PSRAM | 73.8 % |
| ESP32-CAM | 4 MB quad | PSRAM (dram1 is the limit) | ota-level only: 96.6 % dram1 |
| ESP32-C6 | none | internal | 79.1 % |
| WROOM-32 | none | internal | does not fit |

A board without PSRAM running two TLS sessions needs 8 KB records to fit at
all; the WROOM run showed Cumulocity's HTTPS firmware download and the
remote-access WebSocket both work with 8 KB records. What a run also showed,
and a link check could not: the WROOM Modbus board file's
`CONFIG_NET_MAX_CONN=6` leaves no connection for a tunnel (the operation
failed with `the cloud connection failed (-2)`), so the shared WROOM
`tedge` board settings raise it to 10 — paid for from the libc `malloc`
arena, which drops to 76 B. That is why every build is run on the hardware
before it is listed. The workflow **fails the build** if any image exceeds
its partition or overflows RAM (the linker already does), and records each
image's flash and RAM use in the release notes so a regression between
releases is visible.

## Phase

Phase 2 (deployability) for the `standalone` variants; the `tedge` variants
package Phase 3 work.

- **Kconfig options added:** none. A new **profile**
  `tedge-zephyr/profiles/ota.conf` (minimal + firmware update) selects
  existing options; its measured cost per board is recorded in its header,
  as for the other profiles.
- **Public `tedge_*` API:** unchanged. `struct tedge_identity.firmware_version`
  already exists and is documented as the reported version; the change makes
  the firmware-update code use it too (internal `tedge_fw_running_version()`).
- **Flash/RAM cost:** measured with Modbus + `full.conf`: ESP32-C6 +0 B image,
  +0 B RAM; QT Py ESP32-S3 +1 B image, +0 B RAM.
- **Host-app glue** stays out of `tedge-zephyr/`: the per-device firmware name
  is set on the app's build command line, the new `apps/tedge-agent` glue
  lives in the app, and board-level settings live in `lib/common/`.

## Capabilities

### New Capabilities

- `firmware-release`: the supported-devices manifest, the CI build matrix,
  the release trigger, the release assets and their naming, versioning from
  the tag, signing policy, flashing a prebuilt image without building, and
  packaging `tedge` images for the Cumulocity firmware repository.

### Modified Capabilities

- `boot-layout`: "Flashing a board" extends to prebuilt release bundles and
  merged images, including from a browser flasher; "Development signing key
  is marked as such" extends to release assets and allows a configured
  production key.
- `tedge-firmware-update`: "The running firmware version is visible in the
  cloud" reports the application's full version string (including a
  pre-release suffix) rather than only the header's `MAJOR.MINOR.PATCH`.
- `workspace-structure`: an application may compose `lib/common/` with **no**
  protocol frontend (device management only), as `apps/tedge-agent` does.

## Impact

- New firmware: `apps/tedge-agent/`, `lib/common/tedge-boards/<board>.conf`
  (moved from `apps/modbus-server/boards/*_tedge.conf`),
  `tedge-zephyr/profiles/ota.conf`.
- New: `.github/workflows/release.yml`, `release/devices.yml`,
  `scripts/release/` (matrix generation, packaging, merged image, notes).
- Changed: `tedge-zephyr/src/tedge_firmware.c` (running version from the
  identity), its unit test, and `tedge-zephyr/VERSION` (patch bump);
  `apps/*/VERSION` (all set to 0.4.0 in the first release commit),
  `apps/*/prj.conf` untouched — release builds override
  `CONFIG_APP_FIRMWARE_NAME` on the command line; `scripts/flash.sh`
  (release-bundle mode), `README.md` (a
  "Flash a prebuilt image" section ahead of the build instructions),
  `DEVICES.md` (link to the manifest as the supported list).
- CI cost: about 40 sysbuild builds per release, run in parallel (GitHub's
  concurrent-job limit queues the rest); the Zephyr workspace is cached by `west.yml` hash to keep
  each job to minutes.
- Dependencies: `esptool` (for `merge-bin`) in the release job; go-c8y-cli
  and `gh` for the user-run upload script; no new firmware dependencies.
- Device fleet: devices running local dev-key builds (0.2.x, firmware name
  `zephyr-<app>`) can be OTA-updated to a release image; after the update
  they report the release's per-device firmware name.
