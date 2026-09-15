## Why

The workspace supports multiple protocol frontends (OPC-UA is the reference).
Modbus TCP is one of the most common protocols a collector needs to test against,
and there is no realistic Modbus device on the bench. This adds a **Modbus TCP
server** firmware that behaves like a believable field device — a client/master
can read live values and write control points over TCP port 502.

To make the demos richer *without* disturbing what already works, this also
introduces a **pluggable simulation layer**: each firmware selects which
simulation drives its data model. The OPC-UA firmware keeps its existing
environment simulation (temperature/humidity/pressure) **unchanged**, while the
Modbus firmware uses a new **pump/motor simulation** whose measurements *react to
the control points* (a basic physical model). Frontends are simulation-agnostic,
so any simulation can back any protocol later.

## What Changes

- **Add a pluggable simulation layer** in `lib/common`: a data-model/simulation
  interface (measurements + control points + a periodic step) with the concrete
  model chosen by a Kconfig `choice` (per-app default). Two implementations:
  - **Environment simulation (default, existing behavior):** `temperature`,
    `humidity`, `pressure` + `Setpoint`/`Running`. This is today's model,
    repackaged as the default simulation — **no behavior change**.
  - **Pump/motor simulation (new, control-driven):** measurements `flow_lpm`,
    `pressure_bar`, `motor_temp_c`, `rpm`, `vibration_mms`, and `run_hours`
    (monotonic); controls `speed_setpoint` (%, 0–100), `running`, `mode`
    (0=off/1=auto/2=manual). A **stateful step** couples measurements to controls
    via pump affinity laws (flow ∝ speed, pressure ∝ speed²), a motor-thermal lag,
    run-hours accrual while running, and an over-temp fault — so writing a control
    visibly changes what a client reads.
- **The OPC-UA firmware is not changed** — it selects the environment simulation
  (its node set and behavior are identical; no forced reflash).
- **Add a Modbus TCP frontend** `lib/modbus/` (Zephyr module) on Zephyr's in-tree
  `modbus` subsystem in **raw-ADU server mode**, plus a TCP listener on **port
  502** (mirrors `samples/subsys/modbus/tcp_server`).
- **Add application** `apps/modbus-server/` composing `lib/common` + `lib/modbus`,
  selecting the **pump simulation**, with `CONFIG_APP_FIRMWARE_NAME` =
  `zephyr-modbus-server`.
- **Expose all four Modbus object types** mapped from the pump model + status
  (register map in design.md / the spec): Input Registers (scaled ints + IEEE-754
  float pairs + a 32-bit `run_hours` counter), Holding Registers (speed_setpoint,
  mode), a Coil (running), Discrete Inputs (running mirror, fault, network).
- README/SCOPE updated with the simulation-selection mechanism, the new app, the
  register map, and a client test recipe.

## Non-goals

- **Changing the OPC-UA firmware's data model / node set** — it stays on the
  environment simulation.
- **Modbus RTU/serial**, Modbus client/master, Modbus/TLS, multi-unit gateways,
  exotic function codes.
- OPC-UA subscriptions (separate, PSRAM-gated concern).
- Phase 3 (thin-edge.io / Cumulocity) — out of scope.

## Capabilities

### New Capabilities
- `simulation-model`: a pluggable simulation layer behind the shared data-model
  interface, selected per firmware via Kconfig; ships an environment simulation
  (existing) and a control-driven pump/motor simulation.
- `modbus-tcp-server`: a Modbus TCP server frontend on port 502 serving the four
  primary object types, mapping the selected simulation's data model, conforming
  to the `protocol-frontend` contract.

### Modified Capabilities
<!-- None. The OPC-UA frontend and its data model are unchanged; the environment
     simulation preserves today's behavior. -->

## Impact

- **`lib/common`:** new simulation interface + Kconfig `choice`; `data_source.*`
  refactored so the environment model is one selectable simulation (identical
  behavior) and the pump model is another; `controls.*` gains `mode` (additive;
  ignored by the environment sim); new stateful pump-sim step. The environment
  simulation's outputs are unchanged.
- **`lib/opcua`:** unchanged — selects the environment simulation.
- **New:** `lib/modbus/` (module.yml, Kconfig, CMake, TCP listener + raw-ADU
  server + object callbacks), `apps/modbus-server/`.
- **Dependencies:** none added — Zephyr's in-tree `modbus` subsystem
  (`CONFIG_MODBUS`) + existing socket stack. No third-party vendoring.
- **Boards:** Wi-Fi boards (`esp32_devkitc/esp32/procpu`,
  `adafruit_qt_py_esp32s3/esp32s3/procpu`); excludes the ESP32-S2 (Wi-Fi broken).
  `native_sim` builds; end-to-end TCP under NSOS has the same `select()` caveat as
  OPC-UA, so verification is on hardware.
- **Resource constraints:** the Modbus core + single-client listener is far
  lighter than open62541; fits the WROOM budget with a small fixed register map.
