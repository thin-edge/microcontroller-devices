## 1. Scaffold the workspace skeleton

- [x] 1.1 Create `lib/common/`, `lib/opcua/`, `apps/opcua-server/` directory trees (empty CMake/Kconfig stubs)
- [x] 1.2 Add `lib/common/zephyr/module.yml` and `lib/opcua/zephyr/module.yml` pointing at each lib's `CMakeLists.txt` + `Kconfig`
- [x] 1.3 Add `.gitignore` entries for per-app build dirs (`apps/*/build*/`, `lib/*/build*/`); confirm `west.yml` root manifest and pins are unchanged

## 2. Extract `lib/common` (step 1 — no OPC-UA move yet)

- [x] 2.1 Move `src/net.*`, `src/display.*`, `src/font8x8.inc`, `src/nsos_compat.c` into `lib/common/` and wire them in `lib/common/CMakeLists.txt` (preserve the `CONFIG_NET_SOCKETS_OFFLOAD` conditional for nsos)
- [x] 2.2 Move `src/data_source.*` into `lib/common/` and expose the data-model API (measurements via data_source; writable control points via new `controls.[ch]`; identity via new `identity.[ch]`)
- [x] 2.3 Move the device-identity/version node helpers into `lib/common/identity.[ch]` as protocol-agnostic accessors (DeviceId/FirmwareName/FirmwareVersion/BuildTimestamp)
- [x] 2.4 Split `Kconfig`: shared symbols into `lib/common/Kconfig`; OPC-UA-only symbols into `lib/opcua/Kconfig`
- [x] 2.5 Consume `lib/common` via `ZEPHYR_EXTRA_MODULES` and fix includes
- [x] 2.6 Build `native_sim/native/64` in the container; resolve Kconfig/link wiring (fixed: board overlays are app-relative — moved `boards/` under the app)
- [x] 2.7 Superseded — the intermediate hardware smoke test is folded into the group 4 hardware gate (common extraction + OPC-UA relocation done together, both verified via native_sim + ESP32 builds)

## 3. Relocate the OPC-UA frontend into `lib/opcua` + `apps/opcua-server` (step 2)

- [x] 3.1 Move `src/opcua_server.*` and `src/address_space.*` into `lib/opcua/`; address_space now reads the common data-model/identity/controls APIs; `opcua_server_start()` is the lifecycle entry point
- [x] 3.2 Move `third_party/open62541/` under `lib/opcua/third_party/` and update `scripts/regen-open62541.sh` output path; re-ran regen — all 10 patches apply, diagnostics off
- [x] 3.3 Move `src/main.c` → `apps/opcua-server/src/main.c`; move `prj.conf` (+ `CONFIG_APP_FIRMWARE_NAME`), and `VERSION` into `apps/opcua-server/`
- [x] 3.4 Write `apps/opcua-server/CMakeLists.txt` appending `lib/common` + `lib/opcua` to `ZEPHYR_EXTRA_MODULES`; app `Kconfig` sources `Kconfig.zephyr`
- [x] 3.5 Remove the root `src/`, root `CMakeLists.txt`, and root `Kconfig`
- [x] 3.6 Build `apps/opcua-server` for `native_sim/native/64` AND `esp32_devkitc/esp32/procpu` in the container (both OK)

## 4. Hardware re-verification (regression gate — must match pre-restructure)  [BLOCKED: needs an ESP32 plugged in]

- [ ] 4.1 Build `apps/opcua-server` for `esp32_devkitc/esp32/procpu` and flash an ESP32-WROOM/D0WD
- [ ] 4.2 Verify browse/read of `Device`, `DeviceId`, `FirmwareName`, `FirmwareVersion` (=`0.2.0`), `BuildTimestamp`, and the three measurements
- [ ] 4.3 Verify writable `Setpoint` (in-range write, out-of-range clamp)
- [ ] 4.4 Re-run the 300-cycle connection-churn stress; confirm 300/300 with zero `Failed to allocate net buffer` and zero `closing the server socket`
- [ ] 4.5 Confirm mDNS/DNS-SD (`<hostname>.local` + `_opcua-tcp._tcp`) still advertises

## 5. Frontend-contract seam (step 3 — docs/stubs only, no new protocol)

- [x] 5.1 Document the protocol-frontend contract in `lib/common/README.md` (data model, lifecycle, dependency rules)
- [x] 5.2 Add a `lib/frontend-template/` skeleton (module.yml, gated CMake, header + stub `.c`, README) demonstrating how a future frontend plugs in

## 6. Documentation

- [x] 6.1 Update README build/flash commands to `west build -b <board> apps/opcua-server` (WROOM, native_sim, S2-blocked note retained); note the root Wi-Fi overlay passed by absolute path
- [x] 6.2 Add a repository-layout section to README + SCOPE.md (lib/common, lib/<protocol>, apps/<protocol>) and the per-app `FirmwareName` convention
- [ ] 6.3 Commit; leave a follow-up note that SNMP/Modbus/CAN are separate Phase-2 changes riding on this structure
