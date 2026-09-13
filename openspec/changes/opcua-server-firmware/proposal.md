## Why

We need a ready-to-use way to expose data from a microcontroller over a standard
industrial protocol so that a separate collector/gateway device can read it
without any custom, device-specific integration. OPC-UA is the first target
protocol: it is widely supported by industrial clients and self-describing, so
a standard OPC-UA client can discover and read the device's data. This is the
Phase 1 foundation for the repo — it also establishes our Zephyr RTOS build,
configuration, and testing patterns that later phases build on.

## What Changes

- Introduce a **Zephyr RTOS firmware application** that runs an **OPC-UA server**
  on the device and serves device/sensor data to external OPC-UA clients.
- Embed the **open62541** OPC-UA stack, configured for a constrained-resource
  (nano/minimal) build, into the Zephyr application.
- Define an **OPC-UA address space** that represents the device and its data
  points (identification info + one or more sensor/measurement variables) as
  readable nodes.
- Provide a **data-source layer** that samples device data (starting with a
  simulated/synthetic source) and updates the corresponding OPC-UA variable
  nodes.
- Connect over **Wi-Fi** (station mode) — all target boards are Wi-Fi-only,
  with no Ethernet — including configurable Wi-Fi credentials.
- Advertise the device on the local network via **mDNS / DNS-SD** so it is
  discoverable by hostname (`<device>.local`) and as an OPC-UA service
  (`_opcua-tcp._tcp`), without needing to know its IP address in advance.
- Establish a **reusable, portable build/config** across the on-hand boards plus
  a hardware-free **`native_sim`** target for development and automated testing.
- Document how to build, flash, and connect a standard OPC-UA client, with
  **macOS-specific** build/flash steps (the dev machine is macOS) alongside
  Linux where they differ.

**Phase**: Phase 1. **Protocol**: OPC-UA (server role). **Transport**: Wi-Fi
(station mode). **Boards** (all on-hand, Wi-Fi-capable):
- `esp32_devkitc_wroom` (ESP32-WROOM-32, dual-core, 520 KB SRAM) — co-primary
- `esp32s2_franzininho`-class ESP32-S2 (Feather ESP32-S2 TFT, 320 KB SRAM +
  2 MB PSRAM) — co-primary
- `rpi_pico/rp2040/w` (Raspberry Pi Pico W, CYW43 Wi-Fi) — additional/stretch
  target (Zephyr Wi-Fi support less mature)
- `native_sim` — hardware-free dev/CI target

(Exact Zephyr board identifiers to be confirmed during setup.)

### Non-goals

- The client/collector side (reading the data) — that is a separate device/tool.
- thin-edge.io or Cumulocity connectivity — deferred to Phase 3.
- Additional industrial protocols beyond OPC-UA — Phase 2.
- OPC-UA writes/method calls, historical access, subscriptions/PubSub — Phase 1
  is read-focused; anything beyond basic read is out unless explicitly added.
- Security hardening beyond a basic endpoint (full certificate/encryption story
  is tracked but not required for the Phase 1 milestone).
- Real sensor hardware drivers — the initial data source is simulated; concrete
  sensor bindings can follow.

### Resource constraints

The target boards are far tighter than a typical Ethernet dev board. Firmware
must fit within these budgets and use open62541's reduced/nano build profile to
keep the OPC-UA stack's RAM footprint bounded:
- ESP32-WROOM-32: ~520 KB internal SRAM, no PSRAM, dual-core Wi-Fi.
- ESP32-S2 (Feather TFT): ~320 KB internal SRAM **+ 2 MB PSRAM** (PSRAM is the
  practical enabler for open62541 headroom on this part), single-core Wi-Fi.
- Pico W (RP2040): ~264 KB SRAM, Wi-Fi via CYW43 — the tightest target.

The Wi-Fi stack itself consumes significant RAM on all of these, so designs must
avoid unbounded per-connection allocation, cap concurrent sessions/connections,
and keep the address space small and static.

## Capabilities

### New Capabilities
- `opcua-server`: An OPC-UA server endpoint on the device — network transport,
  server lifecycle, discovery/session handling, and interoperable read access
  such that a standard OPC-UA client can connect, browse, and read nodes.
- `device-data-model`: The OPC-UA address space and how device/sensor data is
  modeled as nodes and kept up to date from a data-source layer (initially a
  simulated source).
- `mdns-discovery`: Advertise the device and its OPC-UA service over mDNS /
  DNS-SD so it can be found on the local network by hostname and service type
  without knowing its IP in advance.
- `firmware-build-config`: A reusable, portable Zephyr firmware build and
  configuration usable across the reference hardware boards and a `native_sim`
  dev/CI target, easy to build and flash, with macOS and Linux instructions.

### Modified Capabilities
<!-- None — this is the first change; no existing specs. -->

## Impact

- **New code**: a Zephyr application (`west`/CMake/Kconfig/prj.conf, board
  overlays), integration of the open62541 stack, an OPC-UA address-space setup
  module, a data-source/sampling module, and mDNS/DNS-SD advertisement.
- **Dependencies**: Zephyr RTOS + SDK, `west`, the open62541 library (vendored
  or as a Zephyr module), Zephyr's networking + Wi-Fi stack (ESP32 Wi-Fi driver,
  CYW43 driver for Pico W), Zephyr mDNS responder / DNS-SD (or open62541's
  multicast discovery), and PSRAM support for the ESP32-S2 target.
- **Tooling/CI**: a `native_sim` build/run path enabling automated tests with a
  host-side OPC-UA client; macOS + Linux documentation for build/flash/connect
  (macOS is the primary dev machine).
- **Systems**: introduces the device as an OPC-UA server on the local network;
  no cloud or thin-edge.io systems are touched in this phase.
