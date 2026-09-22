Groups 1–4 are firmware (device side); groups 5–8 and 10–11 are
build/release tooling (CI, host scripts, docs); group 9 is the client side
(uploading to Cumulocity); group 12 is end-to-end verification on boards.
Group 4 gates the manifest: no `tedge-*` build is listed until measured.

## 1. Shared board settings and the ota profile (device side)

- [x] 1.1 Create `lib/common/tedge-boards/` with one app-independent file per
      board (C6, S3-DevKitC, QT Py S3, WROOM-32, ESP32-CAM) from the existing
      `apps/modbus-server/boards/*_tedge.conf` and
      `apps/snmp-agent/boards/esp32_devkitc_esp32_procpu_tedge.conf`; remove
      the originals; rebuild the in-service Modbus C6/S3/QT Py and CAM SNMP
      images and confirm `.config` is unchanged
- [x] 1.2 Add `tedge-zephyr/profiles/ota.conf` (minimal + firmware update +
      second TLS context), with the measured cost per board in its header;
      update `profiles/README.md`
- [x] 1.3 Update DEVICES.md "Rebuilding one" and README commands to the new
      paths

## 2. apps/tedge-agent (device side)

- [x] 2.1 Scaffold `apps/tedge-agent` (CMakeLists, Kconfig, `prj.conf` with
      `CONFIG_TEDGE=y`, VERSION at the shared version, `sysbuild.cmake` /
      `sysbuild.conf` like the protocol apps, board files for the five devices)
- [x] 2.2 `src/main.c`: Wi-Fi + stored/ZTP credentials, provisioning hand-off,
      identity, status LED, button, liveness watchdog — `lib/common` only,
      no frontend or simulation
- [x] 2.3 `src/tedge_glue.c`: identity (type `thin-edge.io-zephyr-agent`,
      firmware name/version), health, restart, device telemetry (RSSI, free
      heap, uptime), remote-access allow-list hook (LAN), shell allow-list
      empty by default
- [x] 2.4 Build it `tedge-full` on the C6 and provision it with
      lab-ztp-provisioner: enrols, reports telemetry, SSH to a LAN host through
      a remote-access tunnel works — *done on the rpi5 WROOM instead* (C6
      `tedge-full` links at 63.6%; no ZTP server was running, so the device
      was registered with `c8y deviceregistration register-ca`): enrols,
      restart and tunnel pass; telemetry is published but eu-latest has no
      `te/` measurement mapping, so it could not be seen in the tenant
- [x] 2.5 README section for the app; workspace-structure spec delta holds

## 3. tedge-zephyr: report the full firmware version (device side)

- [x] 3.1 `tedge_fw_running_version()` returns `identity.firmware_version`
      when set, the MCUboot header's `X.Y.Z` otherwise; the refusal of the
      running version and the post-reboot confirm/rollback comparison use it
- [x] 3.2 Unit test (native_sim): with a version `0.4.0-rc1`, a request for
      `0.4.0-rc1` is refused and a request for `0.4.0` is accepted; the
      post-reboot check treats `0.4.0-rc1` as installed, not rolled back
- [x] 3.3 Update the module README (reported version) and the
      tedge-firmware-update spec delta; bump `tedge-zephyr/VERSION` to 0.0.2;
      run `check-independence.py`; record flash/RAM delta on the C6 and QT Py


## 4. Capacity measurement (device side, on hardware)

Per design 1d: link fit, then a run covering connect, 10 min telemetry, a
firmware update that completes and confirms, a tunnel where remote access
is built in, and a protocol read during the update/tunnel. Record DRAM %,
min free libc heap and mbedTLS heap peak in a per-board fit table in
DEVICES.md. Flash with `--app-only` onto already-onboarded devices.

- [x] 4.1 WROOM-32 build-only pass: every protocol app × {ota,
      remote-access-enabler, full} and `tedge-agent` × {ota, ota +
      remote access, full}, 8 KB TLS records, internal heap; record region
      use and overflow amounts (started during proposal: `build_fit_wroom_*`)
- [x] 4.2 WROOM-32 runtime (rpi5): note the images on `/dev/ttyUSB0` (OPC-UA)
      and `/dev/ttyUSB2` (remote-access enabler) so they can be restored; run
      each candidate that linked, starting with `tedge-agent` `ota` +
      remote access and the lightest protocol app `ota`; confirm whether the
      HTTPS firmware download works with 8 KB records
- [x] 4.3 WROOM-32: restore the original image and confirm it boots (full
      backup written back, hash verified); decide its manifest rows (design 1e)
- [x] 4.3a Copy the WROOM fit table (design 1e) into DEVICES.md, and put the
      validated settings into `lib/common/tedge-boards/esp32_devkitc.conf`
- [x] 4.3b WROOM `tedge-agent`: once group 2 exists, build and run `tedge-ota`
      with remote access, then try adding certificate renewal and parameters
- [x] 4.4 ESP32-C6: `tedge-full` and `tedge-ota` for `opcua-server`,
      `snmp-agent` and `tedge-agent` (Modbus is known); OPC-UA `tedge-full`
      is the likeliest failure (no PSRAM)
- [x] 4.5 ESP32-S3-DevKitC: `tedge-full` and `tedge-ota` for all four apps
- [x] 4.6 QT Py S3 (rpi5): `tedge-full` and `tedge-ota` for all four apps
- [x] 4.7 ESP32-CAM (rpi5): `tedge-ota` for all four apps and whether any
      extra (parameters, log upload) fits in dram1 alongside SNMP
- [x] 4.8 For each failing `tedge-full`, define that board's `tedge-ota`
      extras (trim until the runtime check passes); summarise all boards in
      DEVICES.md "Features per board"

## 5. Supported-devices manifest

- [x] 5.1 Write `release/devices.yml` with the variants block and the five
      devices, their board targets, chips, flash sizes, CAM overlays,
      `tedge_board_conf`, and every build that passed group 4, each `tedge-*`
      build with its `measured:` reference and any `extra_conf`
- [x] 5.2 Write `scripts/release/matrix.py`: validate the manifest (apps
      exist, files exist, board listed in `sysbuild/provisioning.cmake`,
      unique ids, known variants, `measured` present on `tedge-*`, firmware
      name ≤ 47 chars) and print the GitHub `matrix.include` JSON; errors
      name the offending entry
- [x] 5.3 Unit-test `matrix.py` against a valid manifest and one fixture per
      validation error (plain `python3 -m unittest`, no new dependencies)

## 6. Versioning (one version for all apps)

- [x] 6.1 Write `scripts/release/bump.sh X.Y.Z`: set `VERSION_MAJOR/MINOR`,
      `PATCHLEVEL` in all five `apps/*/VERSION`, reset `VERSION_TWEAK` and
      `EXTRAVERSION`; reject a non-semver argument
- [x] 6.2 Write `scripts/release/check-version.sh <ref>`: files agree with
      each other; on a `vX.Y.Z[-pre]` tag they equal `X.Y.Z`; export
      `EXTRAVERSION` (`<pre>`, `dev` or empty) for the build; malformed tags
      fail
- [x] 6.3 Confirm locally that the version reaches `APP_VERSION_STRING` and
      the MCUboot header (`imgtool dumpinfo`) for app and provisioner, and
      that `EXTRAVERSION` does not break `imgtool --version`
- [x] 6.4 README "Releasing" section: bump policy (PATCH/MINOR/MAJOR as in the
      design), bump → commit → tag, pre-releases, `tedge-zephyr` versioned
      separately

## 7. Packaging

- [x] 7.1 Move the devicetree `part()` offset logic out of `scripts/flash.sh`
      into `scripts/release/offsets.py` (reads `zephyr.dts`, applies the
      classic-ESP32 `0x1000` bootloader offset) and have both callers use it
- [x] 7.2 Write `scripts/release/package.py <build-dir> --device --app
      --variant --out`: produce `<stem>.app.bin`, `<stem>.factory.bin`
      (`esptool merge-bin` at absolute offsets from `0x0`, gaps padded,
      `--flash-mode/--flash-freq/--flash-size` from the manifest, nothing at
      or past `storage`) and
      `<stem>.zip` with the three images, `flash.json` and `README.txt`
- [x] 7.3 Record the signing statement in `flash.json`: "MCUboot development
      key" or the SHA-256 fingerprint of the key's public part
- [x] 7.4 Pass `-DCONFIG_APP_FIRMWARE_NAME="<app>-<variant>-<device>"` for every
      build; `matrix.py` rejects names over 47 characters
- [x] 7.5 Write `scripts/release/c8y-manifest.py`: `c8y-firmware.json` from the
      manifest and the packaged `tedge` builds (name, `X.Y.Z`, device type from
      the app's glue, asset, SHA-256, signing statement)
- [x] 7.6 Write `scripts/release/size.py`: app image size vs. `slot0` and
      DRAM use from `zephyr.elf`; write `size.json`; fail above 95 % of `slot0`
- [x] 7.7 Run 7.2–7.6 locally on one C6 `tedge-full` and one WROOM `standalone`
      build and check `flash.json` offsets against `flash.sh --dry-run`

## 8. flash.sh release-bundle mode

- [x] 8.1 Teach `scripts/flash.sh` to accept a directory with `flash.json`:
      chip, offsets and images from the JSON (via `python3`), all existing
      options unchanged
- [x] 8.2 Refuse a bundle whose declared flash size exceeds what
      `esptool flash-id` reports, naming both sizes
- [x] 8.3 `--dry-run` on a bundle prints the same commands as on the build
      directory it came from (check with the builds from 7.7)

## 9. Cumulocity firmware repository upload (client side)

- [x] 9.1 Write `scripts/release/c8y-upload.sh <tag> [--dry-run]` (pre-release
      tags allowed): `gh release download` the `.app.bin` files,
      `SHA256SUMS` and `c8y-firmware.json`; verify checksums; find-or-create
      each firmware by name with its device type; create each version with
      the file uploaded, skipping existing ones; print an install example
- [x] 9.2 Add `--from-dir <dir>` to use a `workflow_dispatch` run's artifacts
      instead of a release; try it against the tenant (done on
      thin-edge-io.eu-latest: dry run, upload, idempotent rerun, checksum
      refusal), then delete the test entries
- [x] 9.3 README: "Update over the air from Cumulocity" — run the script (or
      use `c8y-firmware.json` by hand), install from the device's Firmware
      tab, the same-signing-key rule, trialling a pre-release on a few devices,
      string ordering of versions in Cumulocity, and the name change for
      devices coming from local builds

## 10. Release workflow

- [x] 10.1 Pin `zephyrprojectrtos/zephyr-build` by digest (the one that ships
      SDK 1.0.1) in a workflow env var; note it in the README
- [x] 10.2 `workspace` job: restore/save the west workspace cache keyed on
      `west.yml`; on a miss `west update --narrow --depth=1` (one retry),
      `west blobs fetch hal_espressif`, mbedtls submodule init
- [x] 10.3 `matrix` job: run `matrix.py`, validate the tag format on tag runs,
      output the matrix and the version
- [x] 10.4 `build` job (`fail-fast: false`): restore the cache, run
      `check-version.sh`, `west build --sysbuild` with the manifest's overlays
      and, for `tedge-*`, the variant profile, the board's `tedge_board_conf`,
      `extra_conf` and `-Dwifi-provisioner_EXTRA_CONF_FILE=…/overlay-ztp.conf`;
      then `size.py` and `package.sh`; upload assets and `size.json` as
      artifacts
- [x] 10.5 Signing: when `MCUBOOT_SIGNING_KEY` is set, write it to a temp file
      and pass `-DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE`; never echo it; forks
      fall back to the dev key
- [x] 10.6 Add a check to the build step that the produced `.config` has empty
      `CONFIG_APP_WIFI_SSID` and `CONFIG_TEDGE_C8Y_URL` (in `package.py`, with
      `CONFIG_APP_WIFI_PSK`; the build step is `scripts/release/build.sh`)
- [x] 10.7 `release` job (tags only, `contents: write`, needs all builds):
      download artifacts, write `c8y-firmware.json` and `SHA256SUMS`,
      `gh release create` with
      `--prerelease` for `-pre` tags
- [x] 10.8 Write `scripts/release/notes.py`: release notes from the manifest
      and the `size.json` files — device table, which variant to choose,
      flash commands, signing statement, size table, each `tedge` image's
      feature list (from its `.config`), `tedge-zephyr` version
      per `tedge` image, the OTA upload command, links to the README's
      provisioning and security sections
- [x] 10.9 Triggers: `push: tags: ['v*']`, `workflow_dispatch`, and
      `pull_request` with paths `apps/**`, `lib/**`, `tedge-zephyr/**`,
      `sysbuild/**`, `west.yml`, `release/**`, `scripts/release/**`,
      `scripts/flash.sh`, `.github/workflows/release.yml`
- [x] 10.10 Run the workflow on a branch: all builds pass, artifacts
      downloadable — done on PR #4 (run 35716538593): 36 builds + manifest and
      workspace jobs green in 23 min, 36 artifacts, publish skipped. Not done:
      forcing one failure to watch the others finish (fail-fast is off and
      publish needs every build)

## 11. Documentation

- [x] 11.1 README: a "Flash a prebuilt image" section before the build
      instructions — choosing device and variant, `esptool erase-flash` +
      `write-flash 0x0 <stem>.factory.bin`, per-board reset quirks
      (`--before usb-reset` on the QT Py, the C6 console reset), then
      provisioning over Improv or lab-ztp-provisioner
- [x] 11.2 README: "Flash from your browser" — ESPHome Web, Install → erase →
      choose `<stem>.factory.bin`; what the Improv-serial Wi-Fi prompt means
      and where BLE provisioning continues; the BOOT+RESET fallback for
      native-USB boards; link it from every release's notes
- [x] 11.3 README / DEVICES.md: generate the supported-devices table from the
      manifest (or link to it) so the two cannot drift; document how to add a
      device and how to cut a release (tag format, pre-releases, signing key)
- [x] 11.4 Update the signing-key note to cover release images and the
      dev-key → prod-key OTA incompatibility

## 12. Verification on hardware (end to end)

- [ ] 12.1 Commit `bump.sh 0.4.0`, cut `v0.4.0-rc1`, and confirm a pre-release
      with every asset, `c8y-firmware.json` and a verifying `SHA256SUMS`
- [ ] 12.2 WROOM-32: `erase-flash` + factory image at `0x0` with plain
      esptool; boots into the Improv provisioner (checks the `0x1000`
      padding); provision and read the protocol from a client
- [x] 12.3 C6 `tedge` Modbus: flash the rc bundle with `flash.sh`, provision
      through lab-ztp-provisioner, confirm enrolment and that Cumulocity shows
      `modbus-server-tedge-ota-esp32c6-devkitc 0.4.0-rc1`
- [ ] 12.4 QT Py S3 and S3-DevKitC: factory image flash + Improv provisioning
      (smoke test); ESP32-CAM `tedge` SNMP: bundle flash + ZTP enrolment
- [ ] 12.5 Browser flash with ESPHome Web in Chrome on one board per chip
      (WROOM-32 / ESP32, S3-DevKitC / ESP32-S3, C6): each boots into the BLE
      provisioner; record the ESPHome Web version and any quirks in the README
- [ ] 12.6 Pre-release OTA: `c8y-upload.sh v0.4.0-rc1` against tedge-dev05;
      install it on the S3-DevKitC (running a dev-key local build,
      `zephyr-modbus-server 0.2.0`); confirm success and that it reports
      `modbus-server-tedge-ota-esp32s3-devkitc 0.4.0-rc1`, not a rollback
- [ ] 12.7 Tag `v0.4.0`; upload it; install on the S3-DevKitC from 12.6
      (rc → final) and confirm it reports `0.4.0`
- [ ] 12.8 OTA between releases: bump to `0.4.1` on a throwaway branch, build
      via `workflow_dispatch`, upload with `--from-dir`, install on the C6
      from 12.3; confirm success, then remove the test version
- [ ] 12.9 `tedge-agent` over the air: install a `tedge-agent` `tedge-ota`
      release image on a board and confirm a tunnel still works afterwards
- [ ] 12.10 Resolve or record the design's open questions
- [ ] 12.11 After the first release: long soak of the QT Py's Modbus and agent
      `tedge-full` + shell images (a tunnel and shell commands, then a
      firmware download after 30+ min up — the case that failed on the C6)
