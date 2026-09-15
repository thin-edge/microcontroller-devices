## Why

The repo is a single Zephyr application whose whole `src/` tree is the OPC-UA
firmware, with protocol selection as an in-app Kconfig toggle. That works for one
protocol but does not scale to the goal: a fleet of **single-purpose** devices
(one is an OPC-UA server, another a Modbus server, another an SNMP/CAN source).
Adding protocols to the current layout would tangle a single `prj.conf`/`Kconfig`/
`CMakeLists.txt`, force conflicting RAM profiles into one image, and make "what
does this device actually do" unreadable. Restructuring now — with exactly one
protocol implemented — is the cheapest it will ever be, and the code seams
(`net`, `display`, `data_source`, and the OPC-UA frontend already gated behind
`CONFIG_APP_OPCUA_SERVER`) already fall on the right lines.

## What Changes

- Introduce a **libraries + per-protocol applications** workspace in the single
  existing manifest repo:
  - `lib/common/` — a Zephyr module holding the protocol-agnostic core:
    connectivity (`net`), status display, device identity + the
    `FirmwareName`/`FirmwareVersion`/`BuildTimestamp` nodes, and the shared
    device **data-model / data-source** abstraction.
  - `lib/opcua/` — the OPC-UA frontend (`opcua_server`, `address_space`) plus the
    vendored open62541 amalgamation and its regen script.
  - `apps/opcua-server/` — an application that composes `common + opcua`, with its
    own `main.c`, `prj.conf`, `Kconfig`, and `CONFIG_APP_FIRMWARE_NAME` default.
- **BREAKING (build only):** the build command changes from
  `west build -b <board> .` to `west build -b <board> apps/opcua-server`.
  Runtime behavior of the OPC-UA firmware is unchanged.
- Define the **protocol-frontend contract**: how any future frontend
  (SNMP/Modbus/CAN) consumes the shared data model + identity from `lib/common`
  and exposes it over its wire protocol. Documented and stubbed only — no new
  protocol is implemented in this change.
- `boards/`, `scripts/`, the Docker/macOS build+flash workflow, the west manifest
  (root, `self.path`), `native_sim` support, and the ESP32-S2 Wi-Fi-blocked note
  are all retained.

## Non-goals

- Implementing SNMP, Modbus, or CANbus firmware (Phase 2 — separate later
  changes). This change only makes room for them and fixes the contract.
- Any change to OPC-UA behavior, the device data model's values, the mDNS/DNS-SD
  advertisement, or the connection-churn wedge fix. This is a structural move.
- Phase 3 (thin-edge.io / Cumulocity on Zephyr) — explicitly out of scope.
- Multi-protocol "gateway" images (one device exposing several protocols); the
  library layout enables it later but no such app is built here.

## Capabilities

### New Capabilities
- `workspace-structure`: the repository layout and build contract for a
  multi-protocol fleet — single manifest repo at root, `lib/common` as a shared
  Zephyr module, per-protocol `lib/<protocol>` libraries, per-protocol
  `apps/<protocol>` applications that compose `common + one protocol`, the
  per-app build command, and each app declaring its own firmware name/version.
- `protocol-frontend`: the contract every protocol frontend implements against
  `lib/common` — consume the shared measurements, writable control points, and
  device identity, and expose them over the frontend's wire protocol; frontend
  lifecycle relative to connectivity. Establishes the extension point that
  SNMP/Modbus/CAN will satisfy without implementing them here.

### Modified Capabilities
<!-- None. The existing capabilities (opcua-server, device-data-model,
     mdns-discovery, firmware-build-config) have their CODE relocated but their
     required behavior is unchanged, so no requirement-level deltas apply. The
     move is verified by re-running the existing acceptance checks against the
     relocated apps/opcua-server build. -->

## Impact

- **Files moved (no logic change):** `src/net.*`, `src/display.*`, `src/font8x8.inc`,
  `src/data_source.*`, the identity/version nodes from `src/address_space.c`, and
  `src/nsos_compat.c` → `lib/common/`; `src/opcua_server.*`, `src/address_space.c`
  (OPC-UA nodes), `third_party/open62541/`, `scripts/regen-open62541.sh` → `lib/opcua/`;
  `src/main.c`, `prj.conf`, `Kconfig` (app-specific parts) → `apps/opcua-server/`.
- **Build system:** root `CMakeLists.txt` split into per-app `CMakeLists.txt` +
  `lib/*/CMakeLists.txt`; `lib/common` gains a Zephyr `module.yml`. Kconfig split
  into shared (common) vs app-specific symbols.
- **Docs:** README build/flash commands updated to `apps/opcua-server`; SCOPE.md
  layout section added.
- **Dependencies:** unchanged (`west.yml` pins/allowlist identical); open62541 is
  pulled in only by `lib/opcua`, so protocol stacks stay out of images that don't
  use them.
- **Verification:** the relocated `apps/opcua-server` must pass the existing
  hardware acceptance checks (browse/read, writable Setpoint with clamp, the
  300-cycle connection-churn stress with zero net-buffer failures / zero
  listen-socket closes, and version nodes reading `0.2.0`) on ESP32-WROOM/D0WD,
  and still build for `native_sim`.
- **Resource constraints:** no new runtime cost; the split must not increase per-
  image flash/RAM (unused protocol libraries are simply not compiled into an app).
