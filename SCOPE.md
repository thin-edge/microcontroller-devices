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
- **Phase 3 (started 2026-09-19): tedge-zephyr, a device-management client.**
  A thin-edge.io client shipped as a **reusable Zephyr module** that any user's
  Zephyr application can include. The OPC-UA, Modbus and SNMP apps here serve
  as example user applications. It provides device-management features:
  remote access (including to other hosts on the LAN), firmware update,
  telemetry, log retrieval and configuration. It connects **directly to
  Cumulocity** first, and later also **through a thin-edge.io gateway** as a
  child device. See
  [Phase 3 roadmap](#phase-3-roadmap-device-management-client) below.

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
- Adafruit QT Py ESP32-S3 (dual-core Xtensa LX7, 512 KB SRAM, Wi-Fi) — co-primary
- ESP32-C6-WROOM-1-N4 (single-core **RISC-V**, ~512 KB SRAM, 4 MB flash,
  **Wi-Fi 6**) — co-primary. The first non-Xtensa target: it validates that the
  shared core and the protocol frontends are architecture-portable, and all
  three applications are verified on it.
- ESP32-S3-DevKitC-1 (dual-core Xtensa LX7, 512 KB SRAM, 8 MB flash) — co-primary
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
  (temperature/humidity/pressure, default), `sim_pump` (a control-driven
  pump/motor) or `sim_switch` (a managed switch/router with per-port link status
  and traffic counters). Each app picks one; frontends are simulation-agnostic.
- `lib/<protocol>/` — one protocol frontend per library (`lib/opcua/` today),
  mapping the shared data model onto its wire protocol. See
  `lib/common/README.md` for the frontend contract.
- `apps/<protocol>/` — one application per firmware, composing `lib/common` with
  one protocol frontend; owns its `prj.conf`, `Kconfig`, `VERSION`, `boards/`
  overlays, and `CONFIG_APP_FIRMWARE_NAME`. Build with
  `west build -b <board> apps/<protocol>`. Today: `apps/opcua-server` (OPC-UA,
  env sim), `apps/modbus-server` (Modbus TCP on port 502, pump sim) and
  `apps/snmp-agent` (SNMPv2c on UDP 161 + traps on 162, switch sim).
- `tedge-zephyr/` — (Phase 3) the thin-edge.io device-management client, a
  standalone Zephyr module (`tedge`) for *any* Zephyr application. It never
  depends on `lib/` or `apps/`, and it will move into its own repository. Apps
  here opt in with `CONFIG_TEDGE` and keep their glue code in `apps/<app>/src/`.

## Phase 3 roadmap: device management client

### A module for any application

The client lives in `tedge-zephyr/`, a self-contained Zephyr module named
`tedge`:

- **Namespaced:** the API is `tedge_*` and Kconfig is `CONFIG_TEDGE_*`.
- **Self-contained:** it depends only on Zephyr and MCUboot, and never on
  this repo's `lib/`.
- **Portable:** it is laid out so it can move into its own repository and
  west project later without changing its files.
- **The host application owns** connectivity, identity, telemetry and the
  watchdog.
- **The module owns** its threads, a bounded heap, its settings subtree
  (`tedge/`) and its TLS credential tags.
- **Hooks** let the application register custom operations, log types and
  config types, veto a restart, add firmware-confirm checks, and narrow
  remote-access targets.

In this repo, the glue between `lib/common` and the module lives in each app.

### Two transports, one feature set

| | Direct to Cumulocity (first) | Via a thin-edge.io gateway (later) |
|---|---|---|
| Connection | MQTTS to the Cumulocity **MQTT Service** (:9883): SmartREST 2.0 for device management, free-form topics for telemetry. Core MQTT (:8883) is the fallback while SmartREST on the MQTT Service is in Public Preview | Plain MQTT to the gateway's broker (`te/device/<id>//…`), HTTP to its file transfer service, discovered via mDNS `_thin-edge_mqtt._tcp` |
| Device owns | TLS, time (SNTP), authentication, the cloud protocol | Only thin-edge.io JSON; the gateway does the rest |
| Fits | ESP32-C6 and ESP32-S3 (roughly 35–50 KB heap per TLS session, to be measured) | Every board, including the WROOM running OPC-UA |
| Prior art | None; this is new | `thin-edge/rpi-pico-client` (MicroPython), `thin-edge/freertos-esp32-client` (ESP-IDF) |

The feature handlers live in the module and talk to a small transport
interface, so the gateway transport comes without a rewrite. Telemetry sent on free-form topics uses thin-edge.io's `te/` JSON shape,
so both transports share one payload format.

### Every feature is optional at build time

Each device-management feature is its own Kconfig option. The transport and
the authentication method are Kconfig choices. A board with little RAM or
flash builds only what it can afford:

- a disabled feature costs no flash, RAM, threads or TLS sessions;
- the device advertises only the operations compiled into it;
- invalid combinations (for example firmware update without MCUboot) fail at
  configure time.

The cost of each feature is measured per board and app and documented. Each
board has a default profile that fits it.

### Remote access: the MCU as a gateway into its LAN

This is the headline feature. Cumulocity Cloud Remote Access (SSH, VNC, Telnet,
passthrough) is tunnelled over a device-initiated WebSocket (WSS) and bridged by
the MCU to a TCP target. The target can be the MCU itself (for example a
loopback telnet shell, OPC-UA or Modbus), or **another host on its network**,
such as SSH to a Raspberry Pi, PLC or HMI next to it. A brownfield site
therefore gets remote access to its machines with nothing more than a Wi-Fi MCU.
The thin-edge.io gateway can't offer this for child devices, which makes it
the strongest reason for the direct transport.

- **Targets:** a build-time policy (own subnets by default, an allow-list, or
  local only), which the app can narrow further.
- **Audit:** every tunnel opens and closes as an event naming the target.
- **Limits:** a capped number of sessions (default 1, about one TLS session
  each). The feature requires CA-certificate authentication.

### Onboarding

- **Primary: the Cumulocity CA.**
  - The device generates its key on the device, plus a one-time password and
    a pre-filled registration URL.
  - The operator registers the device from that URL.
  - The device enrolls through EST `simpleenroll` and connects with mTLS.
  - It renews through `simplereenroll`.
- **Fallback:** basic-auth credentials obtained via the Cumulocity bootstrap user.
- **Delivery:** the BLE Wi-Fi provisioner returns the registration URL as the
  Improv RPC result. The provisioner stores only the one-time password; all
  crypto stays in the application image. The tenant URL comes from a per-fleet
  Kconfig default, overridable from the shell. A SoftAP/captive-portal
  provisioner, which would add iOS support, is assessed as an alternative.

### Order

| Step | Change | Content |
|---|---|---|
| P0 | `c8y-direct-spikes` | TLS/MQTT Service cost, OTA into `slot1`, CA enrollment, remote-access tunnel to a LAN host, module skeleton, footprint table |
| P1 | `c8y-direct-core` | The `tedge-zephyr` module and public API; onboarding (CA and bootstrap), connection, inventory, restart, availability, state events; one protocol app and `samples/minimal` integrated |
| P2 | — | **Remote access**: WSS bridge to LAN hosts and local services under the target policy, session cap, audit events |
| P3 | — | Firmware update: confirmed only after the new image reaches Cumulocity and passes the app's checks; MCUboot rolls back otherwise |
| P4 | — | Telemetry (free-form, `te/`-shaped) and device-health measurements |
| P5 | — | Diagnostics: shell command (allow-listed), log upload (RAM ring buffer, coredump, health) |
| P6 | — | Certificate renewal (before any real deployment) |
| P7 | — | Configuration management (settings-backed parameters) |
| P8 | — | Gateway transport: thin-edge.io child device, reusing P1–P7 handlers |
| — | — | Move `tedge-zephyr` into its own repository once the P1 API has settled |
| later | — | Device profiles, WROOM tuning, "software" via LLEXT or Wasm |

## Open questions

- Chosen OPC-UA stack: open62541 (reduced/nano profile). Confirm it fits each
  board alongside the Wi-Fi stack (PSRAM likely required on ESP32-S2).
- Is Zephyr's Wi-Fi support mature enough on each target, or should Pico W be
  descoped?
- Which data/sensor sources to model in the initial address space.
- How to supply Wi-Fi credentials without committing secrets. (Answered by BLE
  provisioning, `ble-wifi-provisioning`.)
- Phase 3: see the unknowns U1–U12 in
  `openspec/changes/c8y-direct-spikes/design.md`. In particular: the TLS
  heap cost, whether Cumulocity honours max-fragment-length, SmartREST on the
  MQTT Service, and how `simplereenroll` authenticates when called directly.
- Phase 3: which consumer on the tenant turns free-form telemetry into
  measurements (Dynamic Mapper or other)?
