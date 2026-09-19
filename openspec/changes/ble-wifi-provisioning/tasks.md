## 0. Iteration 1: in-app provisioning (done, superseded by the two-image design)

- [x] 0.1 Improv GATT service, RPC reassembly and validation, identify, advertising (`lib/common/provisioning.c`) — reused by the provisioner (group 4)
- [x] 0.2 Credential resolution (`app_wifi_creds_resolve()`), `wifi_connect(creds)`, `app_net_try_credentials()` with IPv4 check and retry on spurious first-association failures (`lib/common/net.c`)
- [x] 0.3 `sw0` gesture classifier (`lib/common/button_gesture.c`) with host test `tests/button_gesture/` (9 sequences pass)
- [x] 0.4 Status LED mode API and patterns (`lib/common/status_led.c`)
- [x] 0.5 Host client `scripts/improv_provision.py` (bleak) and `scripts/measure_prov.sh`
- [x] 0.6 Verified on the C6 with in-app provisioning: Modbus and OPC-UA provisioned and served; wrong password rejected with nothing stored; protocol error cases; no BLE while serving; window expiry to idle with no reboot over 7 min; no automatic fallback (140 connects + last-resort reboot, 0 provisioning entries); credentials survive app reflash
- [x] 0.7 Measured all 12 board/app pairs (design.md, "Measured cost of in-app provisioning"): WROOM cannot link OPC-UA/SNMP; led to this redesign

## 1. Spike: MCUboot launching a third partition (firmware, gates everything below)

- [x] 1.1 Build MCUboot + the Modbus app for the C6 with sysbuild (dev key, swap-scratch), flash both, and confirm the app boots signed from `slot0`; record MCUboot size and boot-time overhead
- [x] 1.2 Add a test partition layout with a `prov` and a `bootreq` partition; build a trivial signed "hello" image for `prov`
- [x] 1.3 Implement `boot_go_hook()` + `flash_area_id_from_multi_image_slot_hook()` in the repo, built into MCUboot via sysbuild; show that setting `bootreq` boots "hello" from `prov` and clearing it boots the app
- [x] 1.4 Show MCUboot's test-swap and revert of `slot1` still works with the hooks in place, and that `prov`/`storage` are untouched by it
- [x] 1.5 Repeat 1.1–1.3 on a WROOM-32 (bootloader at 0x1000) — needs a WROOM attached — done 2026-09-19 on the Pi's WROOMs; needed `ESP32_REGION_1_NOINIT` (design.md)
- [x] 1.6 Record the results in design.md; if the hooks fall short, decide between a carried `do_boot()` patch and another layout, and update the artifacts before continuing

## 2. Partition layouts and sysbuild (firmware)

- [x] 2.1 Shared partition overlays for 4 MB (C6 at 0x0, WROOM at 0x1000), 8 MB and 16 MB, included from each BLE board's `.overlay` in all three apps; drop the C6 LP-core slots — C6, QT Py S3 and WROOM (4 MB), S3-DevKitC-1 (16 MB); no 8 MB board in hand
- [x] 2.2 Per-app `sysbuild.conf` (MCUboot, signing with the dev key, hooks) for the BLE boards; the S2 and `native_sim` stay on their current builds
- [x] 2.3 Size check: every app fits its slot on every BLE board; fix final offsets once the provisioner size is known (4.5) — C6 and S3-DevKitC-1 done (design.md, "Spike and implementation results")

## 3. Boot request (firmware)

- [x] 3.1 `lib/common/boot_request.{c,h}`: read/set/clear the `{magic, target}` record in `bootreq_partition` via flash_area; shared by the apps, the provisioner and the MCUboot hooks
- [x] 3.2 Production MCUboot hooks from the spike: validate the provisioner (header + signature) before launching it; fall back to the application with a log line when it is invalid

## 4. Provisioner image (firmware)

- [x] 4.1 New `apps/wifi-provisioner/` (CMakeLists, `prj.conf` with the Bluetooth, coexistence and credential options from `overlay-ble-provisioning.conf`, board confs/overlays for the four BLE boards)
- [x] 4.2 Move the Improv service and state machine from `lib/common/provisioning.c` into the provisioner; every exit clears the boot request before rebooting
- [x] 4.3 Read the application identity record for the BLE name and result URL; fall back to `tedge-prov<mac>` and no URL when there is none
- [x] 4.4 Keep the liveness watchdog; confirm a reset inside the provisioner comes back into the provisioner
- [x] 4.5 Measure the provisioner's size on each board and confirm it fits `prov` (~1 MB on 4 MB boards); trim (no mDNS/DNS-SD, minimal logging) if needed

## 5. Application side (firmware)

- [x] 5.1 Remove the provisioning-mode branches and Bluetooth from the application build; keep `app_wifi_creds_resolve()` (stored, then Kconfig) with `wifi_credentials`/settings/NVS
- [x] 5.2 No credentials → set the boot request and reboot (only when the board has a provisioner)
- [x] 5.3 `sw0` gestures: the triple press sets the boot request and reboots; the 10 s hold erases credentials, sets the request and reboots
- [x] 5.4 Write the identity record (hostname, service type, port) only when it differs from the stored one
- [x] 5.5 Confirm the application heap is back to within a few KB of the pre-change baseline on all BLE boards (rerun `scripts/measure_prov.sh`)

## 6. Tooling (host side)

- [x] 6.1 `scripts/flash.sh <board> <app>`: flashes MCUboot, the signed app into `slot0` and the signed provisioner into `prov`; `--erase-storage` also erases `storage` and `bootreq`
- [x] 6.2 Update `scripts/measure_prov.sh` to report app and provisioner sizes against their partitions

## 7. Hardware verification

- [x] 7.1 C6: fresh flash → app finds no credentials → provisioner → provisioned from `improv_provision.py` → app serves (Modbus and OPC-UA)
- [x] 7.2 C6: wrong password → "unable to connect", nothing stored, retry succeeds
- [x] 7.3 C6: power cut during provisioning comes back into the provisioner; the window expiry with credentials returns to the application — resets (C6 USB, S3 EN), not a pulled plug, come back into the provisioner; window expiry run on the S3 with a 60 s window: operator request returns to the app, no credentials goes idle
- [x] 7.4 C6 (someone at the board): triple press → provisioner → abandoned → back on the old network; 1, 2, 4 presses and a short hold do nothing; 10 s hold erases; `REQUIRE_AUTH=y` refuses settings until a press — done 2026-09-19 with someone at the board, using test provisioners with a 60 s window and with REQUIRE_AUTH=y (design.md)
- [x] 7.5 C6: an OTA-style test-swap of a second app build through `slot1` swaps and reverts with credentials intact
- [ ] 7.6 Improv web page (Chrome/Edge) and Home Assistant provision a device; record versions — needs a Chrome/Edge or Home Assistant session
- [x] 7.7 WROOM-32 (OPC-UA, including the connection-churn stress test), S3-DevKitC-1 and QT Py S3: provision and serve — S3-DevKitC-1, QT Py S3 (Modbus), two WROOMs (OPC-UA) and an ESP32 SNMP board provisioned and served; WROOM churn test passed on both WROOMs (design.md)
- [x] 7.8 LED patterns on a board with a plain `led0` (WROOM), or record that no BLE board can show them yet — done 2026-09-19 on a WROOM: steady when serving, even blink while connecting, double-blink in the provisioner, fast blink on identify

## 8. Documentation and wrap-up

- [x] 8.1 README: MCUboot flashing per BLE board (offsets, the helper script, the dev-key warning), the provisioning section rewritten for the provisioner, and the support matrix
- [x] 8.2 Remove `overlay-ble-provisioning.conf` and the in-app provisioning Kconfig; update `overlay-wifi-credentials.conf.example`
- [x] 8.3 Update the board-support spec if board ports now need a partition overlay and sysbuild config
- [x] 8.4 `openspec validate ble-wifi-provisioning --strict`; bring the specs in line with what hardware verification changed
