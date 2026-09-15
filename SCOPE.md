# Scope: Microcontroller Devices (Zephyr RTOS)

## Purpose

Build ready-to-use firmware, based on **Zephyr RTOS**, that turns microcontroller
devices into **industrial-protocol data sources** — e.g. an **OPC-UA server**
(and later other industrial protocols). A separate collector/gateway device can
then pull data off these MCUs over that industrial protocol.

The repo doubles as a hands-on vehicle for gaining experience with Zephyr RTOS
and microcontrollers in general.

## Primary use case

1. Flash an MCU with this firmware so it exposes sensor/process data as an
   **OPC-UA server** (or other industrial protocol).
2. A separate device acts as the **client/collector**, reading that data via the
   industrial protocol.

The MCU is the *source*; something else does the collecting. thin-edge.io /
Cumulocity are **not** involved in this primary phase.

## Phasing

- **Phase 1 (now): Industrial-protocol source firmware.**
  Zephyr-based firmware that makes an MCU act as an OPC-UA server (first target
  protocol), serving device/sensor data to an external client.
- **Phase 2: Additional protocols / sources.**
  Extend beyond OPC-UA to other industry protocols as source options.
- **Phase 3 (exploratory, later): Light thin-edge.io on Zephyr.**
  Only after the earlier phases are fulfilled — explore a lightweight thin-edge.io
  variant on Zephyr that could connect to Cumulocity directly. This is a stretch
  goal, not a Phase 1 requirement.

## Goals

1. **Reusable Zephyr firmware** that is easy to flash and configure for turning
   an MCU into an industrial-protocol source.
2. **OPC-UA server** as the first supported protocol, serving data to an
   external client.
3. **Network discoverability** — advertise the device over mDNS / DNS-SD
   (`<hostname>.local`, `_opcua-tcp._tcp`) so it can be found without a known IP.
4. **Portability** across Zephyr-supported MCU boards.
5. **Learning platform** — clear enough structure to build up Zephyr and MCU
   experience that later feeds the thin-edge.io exploration.

Development is done on **macOS**, so build/flash instructions must work on macOS
(with Linux differences noted).

## Non-goals

- Running thin-edge.io or the tedge agent on the MCU in Phase 1.
- Building the client/collector side (that is a separate device/tool).
- Cloud (Cumulocity) connectivity in Phase 1 — deferred to the Phase 3
  exploration.
- Hardware/board design beyond selecting reference targets.

## Success criteria (Phase 1)

- Firmware builds and flashes (from macOS) on at least one reference Zephyr board.
- The MCU exposes data as an OPC-UA server.
- The device is discoverable on the LAN via mDNS / DNS-SD.
- An external OPC-UA client can discover and read the served data.

## Target devices (on hand)

- ESP32-WROOM-32 (dual-core, 520 KB SRAM, Wi-Fi) — co-primary
- Adafruit Feather ESP32-S2 TFT (320 KB SRAM + 2 MB PSRAM, Wi-Fi) — co-primary
- Raspberry Pi Pico W (RP2040 + CYW43 Wi-Fi) — additional/stretch target

All targets are Wi-Fi-only (no Ethernet). A `native_sim` build is used for
hardware-free development and CI.

## Repository layout (multi-protocol workspace)

The repo is a single west manifest repo organised as a shared core plus
per-protocol libraries and applications, so it can build a fleet of
single-purpose devices (OPC-UA today; SNMP/Modbus/CAN as later Phase-2 apps):

- `lib/common/` — protocol-agnostic Zephyr module: connectivity, status display,
  the device **data model** (measurements + writable control points) and device
  identity (`FirmwareName`/`FirmwareVersion`/`BuildTimestamp`). The data model is
  driven by a **selectable simulation** (Kconfig `choice`): `sim_environment`
  (temperature/humidity/pressure, default) or `sim_pump` (a control-driven
  pump/motor). Each app picks one; frontends are simulation-agnostic.
- `lib/<protocol>/` — one protocol frontend per library (`lib/opcua/` today),
  mapping the shared data model onto its wire protocol. See
  `lib/common/README.md` for the frontend contract.
- `apps/<protocol>/` — one application per firmware, composing `lib/common` with
  one protocol frontend; owns its `prj.conf`, `Kconfig`, `VERSION`, `boards/`
  overlays, and `CONFIG_APP_FIRMWARE_NAME`. Build with
  `west build -b <board> apps/<protocol>`. Today: `apps/opcua-server` (OPC-UA,
  env sim) and `apps/modbus-server` (Modbus TCP on port 502, pump sim).

## Open questions

- Chosen OPC-UA stack: open62541 (reduced/nano profile). Confirm it fits each
  board alongside the Wi-Fi stack (PSRAM likely required on ESP32-S2).
- Is Zephyr's Wi-Fi support mature enough on each target, or should Pico W be
  descoped?
- Which data/sensor sources to model in the initial address space.
- How to supply Wi-Fi credentials without committing secrets.
