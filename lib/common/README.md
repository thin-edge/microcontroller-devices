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
| Liveness watchdog + progress hook | `liveness.*`, `sysworkq_probe.c` | `app_alive()`, `app_liveness_feed()` |
| Health diagnostics | `diag.*` | `app_diag_beat()`, `app_diag_watch_work()`, `app_diag_dump_threads()` |

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
5. **Reports progress from every thread it owns.** Each serving loop calls
   `app_alive(APP_CTX_PROTO)` (a second thread such as the SNMP trap sender uses
   its own context, `APP_CTX_TRAP`) every time round the loop, from that thread
   and never from a timer. No wait in the loop may block without a bound: wait
   with `zsock_poll()` (the frontends use 5 s) or sleep, then call `app_alive()`
   again, so an idle server still shows it is alive.
   - With `CONFIG_APP_LIVENESS`, the first call registers a task watchdog channel
     for the context, and a context that stops calling for
     `CONFIG_APP_LIVENESS_TIMEOUT_S` resets the device.
   - With `CONFIG_APP_DIAG`, the same call feeds the periodic health line, and a
     stale context is reported with its thread state.
   - With both off, `app_alive()` compiles to nothing.

   A new context needs an entry in `enum app_ctx` (`diag.h`) and, with the
   liveness watchdog on, a free `CONFIG_TASK_WDT_CHANNELS` slot (4 of the 5
   default slots are in use).

## Work queues and stacks

Connectivity work (the 3 s status tick, the reachability watchdog and Wi-Fi
reconnects in `net.c`) runs on its own work queue, `net_wq`
(`CONFIG_APP_NET_WORKQ_STACK_SIZE`), which feeds `APP_CTX_NETWQ`. Its handlers
make blocking Wi-Fi management calls, and the gateway ping runs the whole
IPv4/ARP/driver transmit path on the caller's stack. That peaked at about 1.1 KB
on the ESP32, which overflowed the 1 KB system workqueue it used to share.

Don't put deep or blocking work on the system workqueue. `sysworkq_probe.c`
schedules a tiny item there every 3 s, which feeds `APP_CTX_SYSWQ`. Work items
the diagnostics should report (`R`/`Q`/`D` in the health line) are registered
with `app_diag_watch_work()`.

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
