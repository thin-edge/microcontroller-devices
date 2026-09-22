## Context

Every image in this repo is built by hand in the `zephyrprojectrtos/zephyr-build`
container (README, DEVICES.md "Rebuilding one"). A BLE board's image is a
`west build --sysbuild` of three domains — MCUboot, the signed app for
`slot0`, the signed provisioner for `prov_partition` — and `scripts/flash.sh`
writes them at offsets it reads from the build tree's devicetree. The right
combination of board file, `tedge-zephyr` profile, per-board `_tedge.conf`
and provisioner overlay lives in people's shell history and in DEVICES.md.

Two existing CI workflows (`tedge-zephyr.yml`, `wifi-provisioner.yml`) already
run `west init -l` / `west update --narrow --depth=1` in the container as
root with `ZEPHYR_SDK_INSTALL_DIR=/opt/toolchains`; neither fetches the
Espressif blobs or builds a hardware target.

Constraints: release images must carry no secrets (no Wi-Fi, no tenant, no
private key in the repo); a user must be able to flash without the repo;
the per-board fit already measured must not change.

## Goals / Non-Goals

**Goals:**
- One tag push produces a GitHub Release with a flashable image for every
  supported (device, app, variant).
- The supported list is data (`release/devices.yml`), not workflow YAML, so
  adding a board is one manifest entry plus its board files.
- A user flashes with one `esptool` command, no repo, no build — or with no
  tools at all, from a browser flasher such as ESPHome Web.
- Breakage of a released board is caught on the PR that causes it.
- A `tedge` release image can be put into a tenant's firmware repository and
  installed over the air with the existing `c8y_Firmware` operation, and can
  never be offered to hardware it wasn't built for.

**Non-Goals:** see proposal (hosting our own browser flasher, Improv serial,
production key management,
HIL tests, S2/Pico W/native_sim, per-tenant uploads).

## Decisions

### 1. A manifest drives the matrix

`release/devices.yml`:

```yaml
variants:                      # variant -> profile + provisioner
  standalone: { profile: null,                          provisioner: improv }
  tedge-full: { profile: tedge-zephyr/profiles/full.conf, provisioner: ztp }
  tedge-ota:  { profile: tedge-zephyr/profiles/ota.conf,  provisioner: ztp }

devices:
  - id: esp32c6-devkitc
    name: ESP32-C6-DevKitC-1 (N4)
    board: esp32c6_devkitc/esp32c6/hpcore
    chip: esp32c6
    flash_size: 4MB
    tedge_board_conf: lib/common/tedge-boards/esp32c6_devkitc.conf
    builds:
      - { app: modbus-server, variants: [standalone, tedge-full, tedge-ota] }
      - { app: opcua-server,  variants: [standalone, tedge-full, tedge-ota] }
      - { app: snmp-agent,    variants: [standalone, tedge-full, tedge-ota] }
      - { app: tedge-agent,   variants: [tedge-full, tedge-ota] }
  - id: esp32-devkitc          # WROOM-32: no PSRAM; rows from measurement
    board: esp32_devkitc/esp32/procpu
    chip: esp32
    flash_size: 4MB
    tedge_board_conf: lib/common/tedge-boards/esp32_devkitc.conf
    builds:
      - { app: modbus-server, variants: [standalone] }
      - app: tedge-agent
        variants: [tedge-ota]
        extra_conf: [lib/common/tedge-boards/extras/remote-access-1.conf]  # what fits beyond ota
        measured: DEVICES.md#esp32-wroom-32   # required for every tedge build
```

A `tedge-*` build is `EXTRA_CONF_FILE` = variant profile + the device's
`tedge_board_conf` + the build's `extra_conf`, and
`-Dwifi-provisioner_EXTRA_CONF_FILE=apps/wifi-provisioner/overlay-ztp.conf`.
Every `tedge-*` build must carry `measured:` pointing at its DEVICES.md
entry; `matrix.py` rejects one that doesn't, so nothing reaches a release on
an assumption. `scripts/release/matrix.py` validates the file (known board,
files exist, unique ids, `measured` present) and prints the GitHub
`matrix.include` JSON, one entry per (device, app, variant).

*Alternative:* a hand-written `strategy.matrix` in the workflow. Rejected: the
ESP32-CAM shares a board target with the WROOM but needs different overlays,
and variants are per (device, app), which a cross-product matrix with
`exclude:` expresses badly. The manifest also feeds the release notes and
README table, so the list lives in one place.

*Alternative:* derive the list from `apps/*/boards/` files. Rejected: a board
file existing doesn't mean the combination is verified (the S2 has board files).

### 1a. Variants and profiles

- **`tedge-full`** = `profiles/full.conf`: every feature. Listed only where
  it links **and** passes the runtime check (below).
- **`tedge-ota`** = new `profiles/ota.conf`: `minimal.conf` plus
  `CONFIG_TEDGE_FIRMWARE_UPDATE` and the second TLS context the download
  needs. The same lean set on every board, so a user who needs RAM for their
  own code has a predictable choice. On a board where `tedge-full` isn't
  listed, the build adds `extra_conf` for each further feature that fits
  (e.g. remote access on a WROOM), so the board still ships its most capable
  image. The release notes list each image's actual features, read from its
  `.config`, so the name never has to carry them.

*Alternative:* one "best that fits" variant per board. Rejected: users with
roomy boards who want headroom for their app would have no lean option, and
the feature set behind one name would differ silently between boards.

*Alternative:* per-feature variants (`tedge-ra`, `tedge-shell`, ...).
Rejected: combinatorial; users who need a precise set build locally.

### 1b. Board-level `tedge` settings, shared by all apps

The per-board settings a `tedge` build needs — mbedTLS heap size and
location (PSRAM on the S3s and CAM), `CONFIG_ESP_SPIRAM*`, TLS context count,
net buffer depths for interactive tunnels, heap pool — do not depend on the
app. Today they live in `apps/modbus-server/boards/*_tedge.conf` and
`apps/snmp-agent/boards/esp32_devkitc_esp32_procpu_tedge.conf`. They move to
`lib/common/tedge-boards/<board>.conf` (one per board; the CAM gets its own,
since it shares the WROOM's board target but has PSRAM), and the manifest
points at them. An app that needs a board-specific delta (e.g. OPC-UA's
malloc arena under `tedge-full`) keeps a small `apps/<app>/boards/*_tedge.conf`
that the manifest adds as `extra_conf`. DEVICES.md's rebuild commands are
updated to match.

### 1c. `apps/tedge-agent`: device management without a protocol

A fourth app composing `lib/common/` (Wi-Fi + stored credentials,
provisioning hand-off and ZTP data, identity, status LED, button, liveness
watchdog) with `tedge-zephyr`, and **no** protocol frontend or simulation.
`CONFIG_TEDGE=y` is in its `prj.conf`; it has no `standalone` variant. Its
glue (`src/tedge_glue.c`) follows the protocol apps' pattern: identity (type
`thin-edge.io-zephyr-agent`), health, restart, and device telemetry (RSSI,
free heap, uptime) so a bare device still shows something. Remote access to
LAN hosts is its headline use: this replaces the hand-built WROOM
"remote-access enabler". It is a Phase 3 *example application*; nothing in
it goes into `tedge-zephyr/`.

*Alternative:* ship `tedge-zephyr/samples/minimal` as-is. Rejected: the
sample has no provisioning hand-off, no ZTP tenant, no stored credentials —
it can't be provisioned without a rebuild — and the module must not depend
on `lib/`.

### 1d. Capacity is measured on the board

For each (device, app, variant) proposed for the manifest:

1. **Link fit** (container build): DRAM/IRAM/flash per region, image vs.
   `slot0` ≤ 95 %.
2. **Runtime fit** (on the board, `--app-only` onto an already-onboarded
   device so enrolment is not repeated): connects; survives 10 minutes of
   telemetry; **firmware update** of a same-app image with a bumped version
   completes and confirms (MQTT + HTTPS download concurrently); where
   remote access is built in, a tunnel carries an SSH session (and, on the
   C6/S3, `htop` at 122×61, per DEVICES.md); for protocol apps, a client
   reads the protocol while the tunnel or download runs. Log the minimum
   free libc heap and the mbedTLS heap peak.
3. Record the result in DEVICES.md (a fit table per board: app × variant →
   DRAM %, heaps, pass/fail + reason). Failures don't enter the manifest;
   for `tedge-ota` on a constrained board the extras are trimmed until it
   passes.

The **WROOM-32s on the rpi5** (`/dev/ttyUSB0`, `/dev/ttyUSB2`) are the only
classic-ESP32-without-PSRAM boards; measuring there means temporarily
replacing their current images (OPC-UA; the remote-access enabler) and
restoring them afterwards. A first **build-only** pass (every protocol app ×
{ota, remote-access, full} on the WROOM) runs before any flashing, so only
candidates that link are run.

### 1e. WROOM-32 results (measured 2026-09-21)

Board: rpi5 `/dev/ttyUSB2`, 3c:71:bf:10:c2:e4, no PSRAM; `--app-only`
images over its MCUboot + ZTP provisioner; it enrolled on
thin-edge-io.eu-latest with the one-time password its ZTP bundle already
held. Afterwards the full 4 MB backup was written back and verified by hash.

Shared WROOM `tedge` settings that the runs validated
(`lib/common/tedge-boards/esp32_devkitc.conf`):
`MBEDTLS_SSL_MAX_CONTENT_LEN=8192`, `MBEDTLS_HEAP_SIZE=57344` (internal),
`NET_SOCKETS_TLS_MAX_CONTEXTS=2`, `NET_MAX_CONN=10`, `NET_MAX_CONTEXTS=10`,
`ZVFS_OPEN_MAX=14`.

| Build | Link (dram0 of 192 KB) | Run |
|---|---|---|
| modbus ota | 96.3 % | enrol + connect 9 s; OTA 800 KB/32 s, swap, confirmed; sys heap 23 KB free steady |
| modbus ota + RA, 6 conns | 99.1 % | tunnel **fails**: `Not enough connection contexts` |
| modbus ota + RA, 10 conns | 99.9 % (136 B spare) | OTA confirmed; SSH tunnel 409 KB/19 s (~21 KB/s); 86/86 Modbus reads during it; min sys heap 21.6 KB; `malloc` arena 76 B |
| modbus ota + RA + cert renewal | 99.6 % at 6 conns; no room at 10 | — |
| modbus ota + RA + cert + params | over by 328 B | — |
| modbus full | over by 8.4 KB (+ dram1 6 KB) | — |
| agent ota + RA + cert renewal | 99.7 % | CA-registered enrolment; restart; SSH tunnel 405 KB/21 s; OTA 0.4.0 → 0.4.1 confirmed |
| agent + params / full | over by 440 B / 3.7 KB | — |
| snmp ota / RA / full | over by 24 / 27 / 40 KB | — |
| opcua ota / RA / full | over by 16 / 19 / 30 KB | — |

Consequences:
- WROOM manifest rows: `modbus-server` `standalone` + `tedge-ota`
  (extra: remote access); `tedge-agent` `tedge-ota` (extras: remote
  access, certificate renewal); `snmp-agent` and `opcua-server`
  `standalone` only.
- No certificate renewal on the WROOM Modbus `tedge` image: those devices
  must be re-onboarded before the certificate expires (as on the ESP32-CAM).
  The release notes say so for that build. The agent has it.
- Measurements over the MQTT Service need the tenant to map
  `te/.../m/<type>`; eu-latest has no such mapping (no device there has
  measurements), so telemetry could not be checked end to end on it.
- 136 B of DRAM and 76 B of `malloc` spare means any growth in `lib/common`
  or `tedge-zephyr` can push the WROOM image over; the PR build matrix
  catches link overflows, but a runtime regression would only show on a
  board — noted in the WROOM's DEVICES.md entry.
- The runs also showed the tedge_Agent twin reporting `APP_VERSION_STRING`
  while `c8y_Firmware` reports the MCUboot header version — the mismatch the
  `tedge-zephyr` change in decision 5 removes.

### 2. Plain runners, cached SDK, workspace and compiler; one job per (device, app)

*First version:* every build ran in the `zephyr-build` container pinned by
digest, one job per build. Measured on PR #4: pulling the image took 238 s
per job on average — more than the 173 s build — the separate workspace job
6 minutes, and 36 jobs ran in two waves under GitHub's concurrency limit:
23 minutes in all.

*Now:*
- **No container.** `zephyrproject-rtos/action-zephyr-setup` installs the
  minimal Zephyr SDK (pinned, `ZEPHYR_SDK_VERSION`) with only the three
  toolchains the boards use, from its cache, and the SDK's host tools.
- **Workspace cache** (`zephyr`, `modules`, `bootloader`, keyed on
  `west.yml`) restored before the action, so its `west update` has nothing
  to fetch; `.west` is left out because the action runs `west init`.
- **ccache, one cache per chip**, through the action: every app and variant
  for a chip compiles the same Zephyr, HAL, mbedTLS and MCUboot sources.
- **One job per (device, app)**, building its variants in turn: 19 jobs for a
  release (one wave), each paying its setup once, later builds reusing the
  first's compiler cache.
- **Pull requests build the `pr: true` subset** (13 builds covering every
  device, app, variant and extra, with each board's tightest images); tags
  and manual runs build everything (a manual run can choose the subset).

*Trade-off:* the digest pinned the host tools too (CMake, dtc, Python); now
they come from the runner image and the SDK's host tools. The SDK version,
the action version and `west.yml` stay pinned, and the runner is pinned to
`ubuntu-24.04` rather than `ubuntu-latest`.

### 3. One version, committed, checked against the tag

All five `apps/*/VERSION` files (three protocol apps, `tedge-agent` and the
provisioner)
hold the same version. `scripts/release/bump.sh 0.4.0` rewrites them; the
maintainer commits that ("chore(release): v0.4.0"), then tags the commit
`v0.4.0`. In CI, `scripts/release/check-version.sh <ref>`:

- on a tag `vX.Y.Z[-pre]`: fails unless every VERSION file is exactly
  `X.Y.Z`; for a `-pre` tag it sets `EXTRAVERSION=<pre>` so
  `APP_VERSION_STRING` reads `0.4.0-rc1` (`<pre>` lowercase, digits and
  dots only: Zephyr's version parser rejects anything else);
- otherwise: fails if the files disagree with each other, and sets
  `EXTRAVERSION=dev`, so a CI artifact reads `0.4.0-dev`.

Zephyr reads the version only from the VERSION files, so with `--apply` the
script writes `EXTRAVERSION` into the CI checkout's copies before building;
the version numbers themselves are only ever checked.

`APP_VERSION_STRING` feeds the MCUboot header (`imgtool --version`), the
SNMP version OID, the OPC-UA version node and — after the `tedge-zephyr`
change in decision 5 — the version a `tedge` device reports to Cumulocity.
The MCUboot header carries `MAJOR.MINOR.PATCH` only.

Bump policy (documented in the README's release section): semver on the
user-visible behaviour of any app or of the flash layout — PATCH for fixes,
MINOR for features or new devices, MAJOR for a change that needs a full
reflash (partition layout, signing key). Every release bumps at least PATCH,
since firmware update refuses the version already running.

`tedge-zephyr/VERSION` is independent and bumped only when the module
changes; the release notes list it per `tedge` image.

*Alternative:* write the tag's version into the files in CI only. Rejected:
the committed files go stale, and a local build after a release would report
an older version than the release it follows.

*Alternative:* per-app versions and per-app tags. Rejected for now (user
decision): the apps share `lib/common/`, the layout and the provisioner, and
ship together.

### 4. Assets and naming

Per build, `scripts/release/package.py <build-dir> --device --app --variant`
produces, with `<stem>` = `<app>-<variant>-<device>-<version>`:

| Asset | Content | Use |
|---|---|---|
| `<stem>.factory.bin` | MCUboot + app + provisioner merged by `esptool merge-bin`, starting at `0x0` | first flash: `esptool write-flash 0x0 <stem>.factory.bin` after `erase-flash` |
| `<stem>.app.bin` | `zephyr.signed.bin` of the app | `flash.sh --app-only`, Cumulocity firmware update |
| `<stem>.zip` | the three images + `flash.json` + `README.txt` | `scripts/flash.sh <dir>` |

`flash.json` records chip, flash mode/freq/size, and each image's offset and
partition — taken from the **app's `zephyr.dts`** exactly as `flash.sh` does
today (same `part()` logic, moved into the packager), including the classic
ESP32 `0x1000` bootloader offset. The merged image covers `0x0` to the end of
`prov_partition`; it does not touch `storage`, so `erase-flash` first is what
guarantees a factory state. The release also carries `SHA256SUMS`.

*Alternative:* ship only the merged image. Rejected: OTA and `--app-only`
need the signed app alone, and the merged image rewrites the provisioner and
bootloader, which a user updating an app shouldn't have to.

### 5. `tedge` images are ready for the Cumulocity firmware repository

How the device handles `c8y_Firmware` today (tedge-firmware-update spec,
`tedge_firmware.c`), and what each fact requires of a release:

| Device behaviour | Requirement on the release |
|---|---|
| Downloads the URL into `slot1`; MCUboot verifies the signature against its own key | upload `<stem>.app.bin` (the signed app, never the factory image); same key as the device's bootloader |
| Reports the running version and, after a reboot, compares it with the version it was asked to install | the repository version must equal the image's reported version exactly — see "Pre-releases over the air" below |
| Refuses a request whose name and version equal what's running | every release bumps the version (decision 3) |
| Sends its token only within the tenant's domain and trusts only the tenant CA for the download | the binary must be stored in the tenant (firmware repository upload), not referenced by a GitHub URL |
| Reports `115,<firmware_name>,<version>` with `CONFIG_APP_FIRMWARE_NAME` | the name identifies the hardware (below) |

**Per-device firmware name.** Every build (standalone too, for consistency in
the OPC-UA/SNMP firmware-name objects) passes
`-DCONFIG_APP_FIRMWARE_NAME="<app>-<variant>-<device>"`, e.g.
`modbus-server-tedge-ota-esp32c6-devkitc` (≤ 47 chars, the client's buffer;
`matrix.py` enforces it). All boards share one device type per app
(`thin-edge.io-zephyr-modbus`), so the type cannot separate a C6 image from
an S3 one; the name can. Cumulocity groups versions under a firmware name, so
an operator picks the name the device already reports and sees only images
built for it.

**`c8y-firmware.json`** (release asset) lists, per `tedge` build: firmware
name, version, device type (`c8y_Filter.type`), asset file name, SHA-256,
and the signing statement. It is generated from the manifest and the build
outputs, so tools other than the script can use it.

**`scripts/release/c8y-upload.sh <tag> [--dry-run]`**, run by a user with a
go-c8y-cli session:

1. `gh release download <tag>` the `.app.bin` files, `SHA256SUMS` and
   `c8y-firmware.json`; verify checksums; refuse a pre-release tag.
2. Per entry: find or create the firmware item by name (`c8y firmware get` /
   `create --name … --deviceType …`), then create the version with the
   binary uploaded (`c8y firmware versions create --firmware … --version …
   --file …`), skipping a version that already exists (idempotent).
   Pre-release tags are uploaded like final ones (version `0.4.0-rc1`).
3. Print what was created and a one-line install example
   (`c8y firmware versions install --device … --firmware … --version …`).

*Alternative:* a CI job uploading to a tenant on release. Rejected: it would
put tenant credentials in the repository's secrets for a project whose users
each have their own tenant.

*Alternative:* firmware entries pointing at GitHub release URLs (no upload).
Rejected: the device's download trusts only the tenant CA; adding GitHub's
CA chain costs flash/RAM on every board and a second TLS trust anchor.

**Pre-releases over the air.** Today `tedge_fw_running_version()` reads the
MCUboot header, which holds only `MAJOR.MINOR.PATCH(+build)`: a
`0.4.0-rc1` image would report `0.4.0`, and after installing it the device
would compare `0.4.0` with the requested `0.4.0-rc1` and record a
**rollback** although the image runs fine. The fix is in `tedge-zephyr`:
report `identity.firmware_version` — the application's
`APP_VERSION_STRING`, which every app already passes — and fall back to the
header only when it is empty. With swap-based MCUboot the running image is
always the one in `slot0`, so the compiled-in string and the header describe
the same image; the string just carries more. This also makes CI builds
visibly `0.4.0-dev` in the cloud instead of posing as `0.4.0`.

The repository version is then the full string (`0.4.0-rc1`, `0.4.0`), and
`c8y-upload.sh` accepts pre-release tags, uploading them as versions of the
same firmware name. Nothing in the device orders versions — it only refuses
the exact version already running — so installing `0.4.0` over `0.4.0-rc1`,
or going back to `0.3.x`, works; Cumulocity sorts versions as strings, which
the README notes.

*Alternative:* encode the rc number in the header's build number
(`imgtool --version 0.4.0+1`) and have the module print `X.Y.Z+N`. Rejected:
it gives the build number a meaning the module can't know for other users'
apps, and the cloud would show `0.4.0+1` while the app's own version objects
show `0.4.0-rc1`.

*Alternative:* keep pre-releases off the OTA path. Rejected (user decision):
trialling a release candidate over the air on a few devices is the point.

**Upgrading devices that run local builds.** A dev-key device reporting
`zephyr-modbus-server 0.2.0` installs a release image as usual (different
name and version); from then on it reports the per-device name. The release
notes say to register the first update under the release name.

### 6. The factory image works in browser flashers

ESPHome Web (web.esphome.io, built on ESP Web Tools / `esptool-js`) and
Espressif's own web flasher take a single local `.bin` and write it at
`0x0`. That is exactly the factory image, so compatibility is a set of
properties the packager guarantees rather than extra artifacts:

- **One file from `0x0`.** `merge-bin` places each part at its absolute
  offset and pads the gaps with `0xFF`, including `0x0–0x1000` before the
  classic ESP32's bootloader. No offset is left for the user to type.
- **Bootloader header as built.** `merge-bin` runs with `--flash-mode`,
  `--flash-freq` and `--flash-size keep`, so the factory image carries
  byte-for-byte the images `flash.sh` writes and that were run on the boards
  (verified: each part identical at its offset). The manifest's
  `flash_size` is what `flash.sh` checks a bundle against. (The app's own
  `.config` is not a reliable source: the WROOM's says 2 MB.)
- **Raw binary, no trailing padding to the full flash size**, so the upload
  stays ~1–2 MB rather than 4–16 MB, and `storage` is not overwritten.
- **Name ends in `.factory.bin`**, the convention ESPHome users already
  recognise as "the one for a first install".
- **Chip coverage:** `esptool-js` supports ESP32, ESP32-S3 and ESP32-C6,
  which covers every device in the manifest (verified per device, task 7).

What a browser flash does *not* do:

- **Erase `storage`.** Writing the image leaves `storage` and `bootreq` from
  earlier firmware in place. ESPHome Web's first-install path offers an
  erase; the README tells users to take it when a board moves from other
  firmware, or when re-provisioning to a different tenant.
- **Configure Wi-Fi.** After flashing, ESPHome Web tries Improv over the
  serial port and reports that the device can't be configured. Expected —
  provisioning is over BLE (Improv BLE via improv-wifi.com, or
  lab-ztp-provisioner). Implementing Improv serial in the app would make
  Wi-Fi setup one click from the same page; that is a separate change.
- **Native-USB reset quirks** (QT Py S3, C6) apply in the browser too: if
  the page can't connect, hold BOOT, tap RESET, and connect again.

*Alternative:* publish an ESP Web Tools `manifest.json` and host our own
install page. Deferred: it needs the binaries on a CORS-enabled origin
(GitHub Pages rather than release assets), and a local file in ESPHome Web
already works without any hosting.

### 7. `flash.sh` learns release bundles

If its argument contains `flash.json`, `flash.sh` reads chip and offsets from
it instead of from a build tree; all options (`--erase-all`,
`--erase-storage`, `--app-only`, `--before`, `--dry-run`) behave the same.
The bundle's `README.txt` also shows the plain `esptool` commands for users
without the repo. Parsing JSON uses `python3` (esptool requires Python
anyway), not `jq`.

### 8. Signing

`sysbuild.conf` keeps the development key as the default. The workflow checks
for the `MCUBOOT_SIGNING_KEY` secret: if set, it writes it to a temp file and
passes `-DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE=<file>`, which sysbuild already
propagates to the provisioner (`provisioning.cmake`). The packager records
the key's SHA-256 fingerprint in `flash.json`, and the release notes state
either "signed with the public MCUboot development key — not for production"
or the fingerprint. PR builds from forks never see the secret and use the dev
key.

A device's MCUboot only accepts images signed with its own key, so a device
flashed from a dev-key release cannot be OTA-updated to a prod-key release;
the release notes say so whenever the key changes.

### 9. Size gate and report

The linker already fails on overflow. After each build the job runs
`scripts/release/size.py`, which reads the app's `zephyr.elf` section sizes
and the partition size from `zephyr.dts`, and writes a small JSON that the
`release` job assembles into a table in the release notes (image size vs.
`slot0`, DRAM used). It also fails the build when the app is above 95 % of
`slot0`, since slot1 must hold an OTA image of the same size.

### 9a. One build script for CI and the bench

`scripts/release/build.sh <firmware-name>` does what a matrix job does —
`west build --sysbuild` with the manifest's overlays, the firmware name and
the signing key, then `size.py` and `package.py` — and the workflow's build
step is just that call. A developer reproduces any release image in the
container with the same command. `package.py` refuses a build with Wi-Fi
credentials or a tenant compiled in, and one whose firmware name is not the
manifest's.

### 10. Release job

`release` downloads all build artifacts, writes `c8y-firmware.json` and
`SHA256SUMS`, generates notes
from the manifest (device table, which variant to pick, flash commands, the
signing statement, the size table), and creates the release with
`gh release create` (draft for `-rc`/pre-release tags, published otherwise).
Permissions: `contents: write` on that job only.

## Risks / Trade-offs

- **`:latest` image drift changes the SDK under a release** → pin by digest;
  bump it in a PR that the build matrix validates.
- **Cache miss makes a release slow or flaky (network)** → single warm-up job,
  `west update` retried once; builds never fetch.
- **A released `tedge` image depends on an overlay combination nobody built
  locally recently** → the PR trigger builds the full matrix whenever
  `apps/`, `lib/`, `tedge-zephyr/`, `sysbuild/`, `west.yml` or the manifest
  change (path filter), not only the release files.
- **Merged image at `0x0` on a classic ESP32** — the ROM loads the bootloader
  from `0x1000`, so the merged image must pad `0x0–0x1000` → `merge-bin`
  places each part at its absolute offset; verified on a WROOM before the
  first release (task).
- **Users flash the wrong device's image (e.g. an 8 MB S3-DevKitC with the
  16 MB layout)** → device ids name the flash size; notes explain
  `esptool flash-id`; `flash.sh` compares `flash.json`'s size with the chip's
  and refuses a mismatch.
- **A browser flash leaves stale `storage` from earlier firmware** → the
  README's browser section says to choose "erase" on a first install; the
  firmware already treats unreadable settings as absent.
- **A browser flasher changes its behaviour (e.g. starts padding or
  rewriting the header)** → the per-release hardware check (task 7) includes
  one browser flash; `flash.sh` and plain `esptool` remain documented.
- **An operator installs an image built for other hardware** (e.g. via the
  API, bypassing the UI's grouping by name) → accepted risk (decided: no
  device-side name check). The per-device firmware name keeps the UI path
  safe; a wrong-chip image that gets through fails to boot, stays unconfirmed,
  and MCUboot reverts it once the crash resets the board.
- **Devices on images from before the `tedge-zephyr` change** report the
  header version; they are all on final versions (`0.2.x`), where header and
  string agree, so installing a release over them behaves as before. From the
  first release image on, the full string is reported.
- **A device updated from a pre-release to the final release** sees versions
  `0.4.0-rc1` → `0.4.0`; they differ as strings, so the install is accepted.
- **~40 builds × each release** → caching keeps each to a few minutes;
  `fail-fast: false` keeps one board's failure from hiding the others.
- **A build that links but misbehaves under load** (heap exhaustion only on a
  second TLS session) → nothing enters the manifest without the runtime
  check in 1d, and each build's DEVICES.md entry records what was exercised.
- **Measuring on the WROOMs takes two in-service devices offline** → build-only
  pass first; flash with `--app-only` (keeps credentials and certificate);
  restore the original images afterwards and check they reconnect.
- **Dev-key images are trivially re-signable by anyone** → stated on every
  release; production key is an explicit non-goal with a switch ready.
- **Improv/ZTP send Wi-Fi passwords in clear over BLE** → unchanged by this
  change; the release notes link the README's security notes.

## Migration Plan

Additive. The first release commit sets every `apps/*/VERSION` to `0.4.0`
(above modbus 0.2.0, snmp 0.2.1, provisioner 0.1.0 and opcua 0.3.0) and is
tagged `v0.4.0`, so firmware update from every existing device works. Devices flashed from
local builds with the dev key can take release app images over OTA or
`--app-only`. Rollback: delete the release/tag; nothing on devices depends on
it.

## Open Questions

- Could OPC-UA live beside the client if open62541's allocations came from
  PSRAM (the S3 boards have megabytes of it)? Measured: today it runs out of
  internal heap at both levels on the C6 and the S3-DevKitC.
- Which extras to add to the CAM's `tedge-ota` images: certificate renewal
  and parameters link beside SNMP and Modbus (not yet run on the board).
- Worth slimming SNMP's static tables (MIB leaf table to flash, smaller
  varbind/request buffers) so `snmp-agent` `tedge-ota` fits the WROOM? A
  separate change if wanted; ~24 KB must go.
