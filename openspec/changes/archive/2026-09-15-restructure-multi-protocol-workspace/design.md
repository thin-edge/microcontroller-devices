## Context

Today the repo is one Zephyr T2 "workspace application": the root is the app
(`self.path: app`), the root `CMakeLists.txt` compiles `src/*.c`, and a single
`prj.conf`/`Kconfig` configure it. OPC-UA is already fenced behind
`CONFIG_APP_OPCUA_SERVER`, and `net.c`/`display.c`/`data_source.c` are already
protocol-agnostic. The goal is a fleet of single-purpose devices (OPC-UA now;
SNMP/Modbus/CAN later), which the single-app layout cannot express cleanly.

This change is purely structural: it must move code and split build files
without altering the OPC-UA firmware's behavior, then re-verify on hardware.

## Goals / Non-Goals

**Goals:**
- A layout where each firmware is an explicit app (`apps/<protocol>`) composing a
  shared core (`lib/common`) with one protocol frontend (`lib/<protocol>`).
- A documented, stubbed contract for future frontends against a common data model.
- Zero behavioral change to `apps/opcua-server`; re-verified on ESP32 hardware.
- No per-image flash/RAM growth (unused protocol libs not compiled).

**Non-Goals:**
- Implementing SNMP/Modbus/CAN (later Phase-2 changes).
- Gateway images combining multiple protocols.
- Phase-3 thin-edge.io/Cumulocity.
- Changing board configs, the west manifest pins, or the Docker/macOS workflow.

## Decisions

### D1: Libraries + per-protocol apps (not a single Kconfig-gated app, not sysbuild)

Each protocol is a library; each firmware is an app composing `lib/common` + one
frontend lib. Chosen because it maps 1:1 to "single-purpose device," isolates
per-protocol `prj.conf`/RAM tuning, makes `FirmwareName` fall out of the app, and
still lets a future gateway app compose several libs.

- *Alternative — keep one app, add `CONFIG_APP_SNMP`/`CONFIG_APP_MODBUS` toggles:*
  least work now, but by protocol 3 the shared `prj.conf`/`Kconfig`/`CMakeLists`
  tangle and RAM profiles collide; "what does this device do" becomes unreadable.
- *Alternative — Zephyr `sysbuild`:* aimed at multi-image builds (e.g. bootloader
  + app) on one target, not at selecting one of many single-image firmwares.
  Overkill and orthogonal to this need.

### D2: `lib/common` is a Zephyr module (module.yml), not a bare `add_subdirectory`

`lib/common/` gets `zephyr/module.yml` pointing at its `CMakeLists.txt` and
`Kconfig`. This shares its Kconfig symbols (e.g. `APP_WIFI`, `APP_DEVICE_NAME`,
identity) with every app automatically and compiles the core from one place.

- *Alternative — each app `add_subdirectory(../../lib/common)`:* works for CMake
  sources but does NOT bring the module's Kconfig into the app's config tree
  cleanly; leads to duplicated symbol declarations. The module approach is the
  idiomatic Zephyr way to share in-repo code + Kconfig.

### D3: In-repo modules discovered via `EXTRA_ZEPHYR_MODULES` in each app

Each `apps/<protocol>/CMakeLists.txt` appends the absolute paths of `lib/common`
and its protocol lib to `EXTRA_ZEPHYR_MODULES` before `find_package(Zephyr)`.
This keeps discovery explicit and per-app (an app pulls in exactly the libs it
composes), and needs no change to `west.yml`.

- *Alternative — list libs as `projects`/`self` in `west.yml`:* would make every
  module visible to every build and blurs the "app composes explicitly" model.

### D4: Common owns the data model; frontends are adapters

Promote the current `data_source` into a small `lib/common` data-model API:
enumerated measurements (read) + writable control points (read/write with clamp)
+ device identity. Frontends translate this model to their wire protocol. The
OPC-UA `address_space.c` becomes the first adapter over this API.

- *Alternative — each frontend keeps its own data source:* diverging values per
  protocol, no single source of truth; rejected.

### D5: Per-app `main.c`, shared init sequence

Each app's `main.c` does the same shape: bring up `lib/common` (connectivity,
identity), then start its composed frontend once connectivity is ready. The
current `src/main.c` becomes `apps/opcua-server/src/main.c` calling
`opcua_frontend_start()`.

## Target layout

```
microcontroller-devices/
├── west.yml                      # unchanged (root manifest repo)
├── boards/                       # shared board overlays (unchanged)
├── scripts/regen-open62541.sh    # moves with lib/opcua (path updated)
├── lib/
│   ├── common/
│   │   ├── zephyr/module.yml
│   │   ├── CMakeLists.txt  Kconfig
│   │   ├── net.*  display.*  font8x8.inc  nsos_compat.c
│   │   └── data_model.*  identity.*      # data-model API + identity/version nodes
│   └── opcua/
│       ├── zephyr/module.yml
│       ├── CMakeLists.txt  Kconfig
│       ├── opcua_server.*  address_space.*
│       └── third_party/open62541/
└── apps/
    └── opcua-server/
        ├── CMakeLists.txt  prj.conf  Kconfig  VERSION
        └── src/main.c
```

## Risks / Trade-offs

- [Silent behavior change during the move] → Move files with minimal edits; do
  the common extraction and OPC-UA relocation as separate verified steps; re-run
  the full hardware acceptance suite (browse/read, Setpoint clamp, 300-cycle
  churn, version nodes = 0.2.0) after each step before proceeding.
- [Zephyr module wiring mistakes (Kconfig symbols not found, double-defined)] →
  Build `native_sim` first (fast, in-container) after each move to catch config
  wiring before flashing hardware.
- [Regen script path drift] → Update `scripts/regen-open62541.sh` output path to
  `lib/opcua/third_party/open62541` and re-run once to confirm patches still
  apply (10 patches, exact-count asserts).
- [Docs/onboarding break] → Update README/SCOPE build commands to
  `apps/opcua-server` in the same change; keep old build dirs out of git.
- [VERSION file location] → Move `VERSION` under `apps/opcua-server/` so
  `app_version.h` is generated for that app; confirm `FirmwareVersion` still
  reads `0.2.0`.

## Migration Plan

1. **Extract `lib/common`** (module.yml, CMake, Kconfig; move net/display/
   data-model/identity/nsos). Keep the OPC-UA code where it is but consume the
   new common module. Build `native_sim`; flash + verify one ESP32.
2. **Relocate OPC-UA** into `lib/opcua` + `apps/opcua-server` (move opcua_server/
   address_space/open62541/regen/main/prj/Kconfig/VERSION). Update build docs.
   Build `native_sim`; flash + run full acceptance (incl. 300-cycle churn) on an
   ESP32-WROOM/D0WD.
3. **Land the frontend contract** docs/stubs in `lib/common` so SNMP/Modbus/CAN
   have a defined seam — no protocol implemented.

Rollback: the change is a series of file moves + build-file edits on a branch; if
verification fails, revert the branch — no runtime migration/state is involved.

## Open Questions

- ~~Should `boards/` overlays remain shared at root, or move per-app?~~
  **Resolved during apply:** Zephyr auto-merges `boards/<board>.conf` relative to
  the *application* dir, so overlays must live at `apps/<protocol>/boards/`. Moved
  there — which also gives each protocol workload independent RAM/buffer tuning (a
  benefit). Truly-common board bits (e.g. Wi-Fi driver enablement) are duplicated
  for now; a shared Kconfig snippet could dedupe them once a second app exists.
- Naming: `lib/common` vs `lib/platform` — keep `common` unless it collides with
  a Zephyr reserved name.
