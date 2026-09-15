# lib/common — shared firmware core & protocol-frontend contract

`lib/common` is a Zephyr module holding everything that is **not** specific to a
single industrial protocol. Every `apps/<protocol>` application composes it with
exactly one protocol frontend from `lib/<protocol>`.

## What lives here

| Area | Files | Public API |
|------|-------|------------|
| Connectivity (Wi-Fi + mDNS/DNS-SD, native_sim host net) | `net.*` | `app_net_init()`, `app_net_wait_connected()`, `app_net_hostname()`, `app_net_show_status()` |
| Status display (optional TFT) | `display.*`, `font8x8.inc` | `display_status_*()` |
| Data model — measurements | `data_source.*` | `data_source_count()`, `data_source_descriptor()`, `data_source_sample()` |
| Data model — writable control points | `controls.*` | `app_control_setpoint()`, `app_control_set_setpoint()` (clamps), `app_control_running()`, `app_control_set_running()` |
| Device / firmware identity | `identity.*` | `app_identity_device_id()`, `app_identity_firmware_name()`, `app_identity_firmware_version()`, `app_identity_build_timestamp()` |

`lib/common` has **no dependency on any protocol stack** (e.g. no open62541).

## The device data model

The "data model" a frontend exposes is the union of three protocol-independent
sources owned here:

- **Measurements** (read-only) — enumerated via `data_source_*`.
- **Writable control points** — `Setpoint` (Int32, range-clamped in
  `app_control_set_setpoint`) and `Running` (bool). State is owned here so every
  frontend enforces identical semantics; the firmware can also act on it.
- **Identity** — device id + firmware name/version/build timestamp.

## Protocol-frontend contract

A protocol frontend is a Zephyr module under `lib/<protocol>/` that:

1. **Depends only on `lib/common`** (and its own protocol stack). It MUST NOT
   depend on another frontend.
2. **Exposes a lifecycle entry point** named `<protocol>_frontend_start()` (e.g.
   `opcua_server_start()`), which the application's `main.c` calls once
   connectivity is available. It brings up the frontend's server/agent, maps the
   shared data model onto its wire protocol, and returns 0 / negative errno.
3. **Reads/writes the shared model through `lib/common`** — measurements via
   `data_source_*`, control points via `app_control_*` (respecting the clamp),
   identity via `app_identity_*`. It MUST NOT define a competing data source.
4. **Provides its protocol-specific options** in its own `lib/<protocol>/Kconfig`
   (port, resource caps, ...). Shared options stay in `lib/common/Kconfig`.

The `lib/opcua` frontend is the reference implementation: `opcua_server.c` owns
the lifecycle/event loop and `address_space.c` maps the shared model onto OPC-UA
nodes. A new frontend (SNMP/Modbus/CAN) follows the same shape — see
`lib/frontend-template/` for a skeleton.

## Composing an application

In `apps/<protocol>/CMakeLists.txt`, before `find_package(Zephyr)`:

```cmake
list(APPEND ZEPHYR_EXTRA_MODULES
  ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/common
  ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/<protocol>)
```

Each app owns its `prj.conf`, `Kconfig` (`source "Kconfig.zephyr"`), `VERSION`,
`boards/<board>.conf` overlays, and its `CONFIG_APP_FIRMWARE_NAME` default.
