# Add ESP32-S3-DevKitC and ESP32-C6-DevKitC board support to all applications

## Why

Board coverage today is narrow and partly broken: the WROOM-32 and the Adafruit
QT Py ESP32-S3 are the only targets verified on hardware, the Feather ESP32-S2
cannot pass Wi-Fi traffic under Zephyr 4.4.2, and the Pico W has never been
built. Every verified board is an Adafruit/DevKitC variant of the same two
Xtensa SoCs, so "portability across Zephyr-supported boards" (SCOPE goal 4) is
asserted rather than demonstrated.

Two boards are now on hand and plugged in: an **ESP32-S3-DevKitC-1** (Espressif's
own S3 reference board, as opposed to the Adafruit QT Py already supported) and a
**QIQIAZI ESP32-C6 (ESP32-C6-WROOM-1-N4)**. The C6 matters disproportionately: it
is the first **RISC-V** target and the first **Wi-Fi 6** radio in the fleet, so
porting to it is the first real test that the shared core and the three protocol
frontends are genuinely architecture-independent rather than accidentally
Xtensa-shaped.

## What Changes

- Add per-app board configuration for **`esp32s3_devkitc/esp32s3/procpu`** to all
  three applications (`opcua-server`, `modbus-server`, `snmp-agent`).
- Add per-app board configuration for **`esp32c6_devkitc/esp32c6/hpcore`** to all
  three applications — the first RISC-V and first Wi-Fi 6 target.
- Correct the **flash size** for the on-hand C6: its board devicetree declares
  8 MB (`esp32c6_wroom_n8.dtsi`), but the module in hand is an **N4 (4 MB)** part.
  The partition table itself is already 4 MB-based
  (`partitions_0x0_default_4M.dtsi`), so partitions fit; the declared `flash0`
  size and the bootloader's flash-size setting must be brought into line with the
  real part.
- Enable a **console over native USB** where needed. Both boards are plugged in
  through their native USB-Serial-JTAG port (they enumerate as
  `USB JTAG_serial debug unit` and `Espressif Device`), but the S3-DevKitC
  devicetree explicitly *disables* `&usb_serial` and routes the console to
  `uart0` on the separate UART bridge — so out of the box there is no console on
  the port actually in use.
- Extend the **flash/console documentation** (README targets table, build/flash
  sections, "Adding another Wi-Fi board") to cover both boards, including the
  `--chip esp32c6` flashing path and the offset-`0x0` / `--before usb-reset`
  native-USB procedure.
- Refresh the host flashing prerequisites: the README's `~/flashenv` virtualenv
  no longer exists on this machine, and the C6 requires **esptool ≥ 4.5**.
- No changes to `lib/common/`, the protocol frontends, or `west.yml` are
  anticipated; if the C6 port forces one, that is a finding this change must
  surface rather than absorb silently.

Notably *not* required: neither board needs a Wi-Fi devicetree overlay. Unlike
the QT Py S3 and the S2 Feather, both upstream board devicetrees already set
`&wifi { status = "okay"; }`, so `CONFIG_WIFI_ESP32` can be selected directly.

## Capabilities

### New Capabilities

- `board-support`: The contract a board port must satisfy to be a supported
  target — Wi-Fi station enablement, a reachable console, an image that fits the
  board's real flash and RAM, a net-buffer/stack budget appropriate to the
  workload, and the optional status indicator — plus the requirement that a port
  consists only of per-app `boards/` files, with no application or shared-core
  source changes. Covers the two new boards and makes the existing, undocumented
  porting convention explicit for future ones.

### Modified Capabilities

None. `wifi-connectivity` already covers boards with no usable status LED via its
"No LED present" scenario, and `workspace-structure` already requires per-app
`boards/<board>.conf` overlays to apply — these ports exercise those requirements
rather than change them.

## Impact

**Affected code**

- `apps/opcua-server/boards/`, `apps/modbus-server/boards/`,
  `apps/snmp-agent/boards/` — six new `.conf` files, plus `.overlay` files where
  the console, flash size, or a status LED needs adjusting.
- `README.md` — targets table, per-board build/flash instructions, prerequisites,
  and the "Adding another Wi-Fi board" section.
- `SCOPE.md` — "Target devices (on hand)" list.
- `scripts/soak/remote.sh` — its `flash` verb accepts `esp32|esp32s3` today and
  needs an `esp32c6` case if these boards are to be soak-tested.

**Resource constraints the design must fit**

- **ESP32-C6**: ~512 KB HP SRAM, single RISC-V core, **4 MB flash** on this part,
  no PSRAM. The OPC-UA firmware is the tight one — on the QT Py S3 it is
  684,500 B flash / 239,792 B RAM (60.1%). The C6 has less usable flash than any
  currently verified board, so OPC-UA fitting there is an open risk, not an
  assumption, and the SNMP/Modbus images (UDP-only, much smaller) are the
  fallback if it does not.
- **ESP32-S3-DevKitC-1 (N8)**: 8 MB flash, ~512 KB SRAM, PSRAM not enabled by the
  default devicetree. Closely comparable to the already-verified QT Py S3, so it
  is the lower-risk of the two ports.

**Dependencies**

- Zephyr v4.4.2 ships both boards (`boards/espressif/esp32s3_devkitc`,
  `boards/espressif/esp32c6_devkitc`) — no manifest change needed.
- Zephyr SDK 1.0.1 provides both `xtensa-espressif_esp32s3_zephyr-elf` and
  `riscv64-zephyr-elf`.
- `hal_espressif` blobs for `esp32c6` (`libnet80211.a`, `libpp.a`, `libphy.a`,
  `libcore.a`) are already fetched in the build container.
- `WIFI_ESP32` requires `!SMP`; the C6 is single-core and the S3 is built as
  `procpu` only, so both satisfy it.

**Operational note**

Running the same application on two boards at once means two devices claim the
same mDNS hostname base (e.g. `tedge-snmp`). `CONFIG_NET_HOSTNAME_UNIQUE=y`
disambiguates them with a MAC-derived suffix, so the `points.d/` device instance
files, which name a host explicitly, need per-board entries rather than one
shared name.

**Phase**

Phase 1 (portability is a Phase-1 goal) for the OPC-UA application, and Phase 2
for the Modbus and SNMP applications, which already exist. Phase 3
(thin-edge.io / Cumulocity) stays out of scope.

## Non-goals

- **No new protocol or application.** This change adds targets for the three
  applications that exist; it does not extend the data model or any frontend.
- **No PSRAM bring-up** on either board, and no attempt to use the S3-DevKitC's
  octal-PSRAM variants. If OPC-UA does not fit the C6's 4 MB flash, the answer is
  to record that limit, not to go hunting for memory.
- **No fix for the ESP32-S2 Wi-Fi data path.** That remains the known upstream
  problem it is today.
- **No Bluetooth, no IEEE 802.15.4 / Thread / Zigbee.** The C6 has both radios and
  its devicetree enables `&ieee802154`; this change uses the board as a Wi-Fi
  station only.
- **No CI integration.** Building these targets in automation is separate work.
- **No replacement of the existing verified boards.** The WROOM-32 and QT Py S3
  remain the reference targets for deployment.
