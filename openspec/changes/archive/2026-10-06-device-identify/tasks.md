## 1. Button gesture classifier (firmware, host-testable)

- [x] 1.1 `button_gesture.{c,h}`: `identify_count` in `gesture_cfg` (0 = off), `GESTURE_IDENTIFY`; provisioning count and erase hold also accept 0 = off (design D5)
- [x] 1.2 Host tests in `tests/button_gesture/`: 2 → identify, 3 → provision, 2 with identify off → none, 2 too slow → none, triple press never fires identify, hold → erase unchanged, provisioning off + 3 presses → none

## 2. `sw0` wiring and identify feature in lib/common (firmware)

- [x] 2.1 Kconfig: `APP_IDENTIFY` (default y if `TEDGE`), `APP_IDENTIFY_DURATION_S` (30, 1–300), `APP_IDENTIFY_PRESS_COUNT` (2, 2–6); `BUILD_ASSERT` that the press counts differ when both features are built
- [x] 2.2 Split GPIO/debounce/work item out of `prov_handoff.c` into `button.c`, built for `APP_PROV_HANDOFF || APP_IDENTIFY`, started from `net.c` outside the hand-off block; provisioning/erase dispatch unchanged (design D4)
- [x] 2.3 `identify.{c,h}`: `tedge identify [seconds]` via `SHELL_SUBCMD_ADD((tedge), …)` under `CONFIG_SHELL`; default/cap 300/reject non-positive or non-numeric; fail with "no status LED" when `!status_led_present()`; `status_led_flash(STATUS_LED_IDENTIFY, …)`, print the duration and return at once (design D1, D2)
- [x] 2.4 `identify.c` gesture handler: 1 s LED ack, `zephyr_Identify` event with the hostname, 5 s rate limit (design D6)
- [x] 2.5 Add `tedge identify` to `CONFIG_TEDGE_SHELL_COMMAND_ALLOW_LIST` and its comment block in `lib/common/tedge-boards/extras/shell-diagnostics.conf` (design D3)

## 3. Cumulocity side (cloud / tenant, not firmware)

- [x] 3.1 `cumulocity/smart-functions/tedge-zephyr-events/`: `data-prep.yaml` (`te/device/*///e/*`), `events.js` (design D8)
- [x] 3.2 Tests: `zephyr_Identify`, an arbitrary type, payload without text, payload with extra fields; pass `cumulocity/smart-functions/test.py` against the tenant
- [x] 3.3 `cumulocity/README.md`: add the function to the table and deployment notes

## 3b. tedge-zephyr: failed shell commands end FAILED (module)

- [x] 3b.1 `tedge_shell_clean_output()` strips escape sequences and control characters from captured output; called in `tedge_shell_cmd.c` (design D9)
- [x] 3b.2 `sr_quote()` drops control characters except `\n` (line mode), `\r` handling as before, and tab
- [x] 3b.3 Unit tests (native_sim): the captured `shell_error()` text from the S3, layout kept, `sr_quote` control characters; all 93 pass; `check-independence.py` passes
- [x] 3b.4 On the S3: `tedge identify 0` and `tedge identify soon` end FAILED with a readable reason (2026-10-06: both FAILED in seconds, multi-line reason accepted; also `kernel uptime too many args` and `tedge identify 1 2` FAILED with the shell's usage text; no "cloud error" in the console)

## 4. Build, size and footprint

- [x] 4.1 Build every tedge-* app × board (ota and full profiles) plus `native_sim`; queue builds sequentially (2026-10-06: all 36 `release/devices.yml` builds and native_sim modbus-server pass)
- [x] 4.2 Measure the flash/RAM delta per board against the baseline, record it in the footprint table, and pass the size gate (measured vs main, 2026-10-06: S3-DevKitC modbus full+shell +463 B image / +32 B dram0; WROOM agent ota +336 B / +24 B; `release/size-baseline.json` rewritten, per-build table in DEVICES.md "Identify measured")
- [x] 4.3 Build with `APP_IDENTIFY=n`, with `APP_PROV_HANDOFF=n`, and without `CONFIG_SHELL` to check each half builds alone (2026-10-06: S3 modbus ota with `APP_PROV_HANDOFF=n` links with `app_button_start` and `app_identify_gesture`, no `app_prov_handle_gesture`; WROOM agent ota links without the shell; the standalone release images build without identify)

## 5. Verification on hardware

- [x] 5.1 Deploy the events Smart Function to the dev tenant (eu-latest t493319102, 2026-10-06 18:20 UTC; no other rule on `te/device/*///e/*`)
- [x] 5.2 S3-DevKitC-1 modbus-server full + shell (RGB LED): `tedge identify` from the Shell tab blinks white for 30 s and the operation succeeds early with the duration; `tedge identify 10` blinks ~10 s; `tedge identify 0` fails with the reason; a command off the list is still refused (2026-10-06, tedge-modbusd405927b33d0: all of these pass on the operations and the device log — 3600→300 s, 10 s, 30 s default, 5 s succeed in 1–3 s; 0/soon/1 2 fail; `kernel bogus` refused; `help` lists `tedge identify`. LED behaviour confirmed by the user at the board)
- [x] 5.3 QT Py ESP32-S3 tedge-agent full + shell: same `tedge identify` checks on the NeoPixel (2026-10-06, run as modbus-server full + shell to keep tedge-dot polling it: tedge-modbusf412fa5a9424 on the Pi — help lists it; 0/soon FAILED with reason; 3600→300, 10, 30 s SUCCESSFUL; double press → zephyr_Identify event 18:45:12 UTC; LEDs confirmed by the user)
- [x] 5.4 S3-DevKitC-1 or QT Py: double press → LED ack + `zephyr_Identify` event on the right device; triple press still enters the provisioner; device keeps serving through the double press (2026-10-06, S3: double press → event queued ~0.7 s after release, `zephyr_Identify` "Identify button pressed on tedge-modbusd405927b33d0" on device 1659934142, 2 s from device time to creation; two gestures 12 s apart each sent one; measurements kept flowing. user confirmed the board behaves as expected at the board)
- [x] 5.5 WROOM-32 ota image (no shell, GPIO LED): double press sends the event; `tedge identify` from the cloud is refused as unsupported, not left pending (2026-10-06, no WROOM enrolled, so run on the other two shell-less boards: ESP32-C6 modbus full (no c8y_Command advertised; `tedge identify` sent anyway FAILED "no handler (511)"; double press → event 18:45:28 UTC) and ESP32-CAM snmp tedge-ota (GPIO33 LED, IO0 on the CAM-MB base; double press → event 18:45:42 UTC); LED acknowledgements confirmed by the user on both. The WROOM on the Pi runs a standalone OPC-UA image with no identify, so it was not part of the test)

## 6. Documentation

- [x] 6.1 README / DEVICES.md: identify from the cloud (Shell tab: `tedge identify [seconds]`, or `c8y operations create … --template "{c8y_Command:{text:'tedge identify'}}"`) and from the button; gesture table updated with the double press; per-image support (cloud identify only with the shell) in the features table
- [x] 6.2 Update `SCOPE.md` Phase 3 roadmap if identify is listed or should be (checked: the roadmap covers the module only; identify is application-side, so no entry)
- [x] 6.3 Re-check the "Shell diagnostics measured" table in DEVICES.md still holds with the subcommand added (size gate) (the shell builds pass with the new baseline; allow-list text updated; S3 and QT Py run the subcommand)
