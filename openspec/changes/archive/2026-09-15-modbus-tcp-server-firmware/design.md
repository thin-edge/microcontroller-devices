## Context

The repo builds per-protocol firmwares by composing `lib/common` (connectivity,
data model, identity) with one `lib/<protocol>` frontend. OPC-UA is the reference
frontend. This adds a Modbus TCP server as the second frontend — a realistic
device a collector/master can read and write over the network.

Zephyr ships an in-tree `modbus` subsystem. Its serial (RTU/ASCII) server mode is
turnkey, but **TCP is done via raw-ADU mode**: the application owns the TCP socket
and feeds received MBAP frames to the Modbus core, which dispatches to
user-registered object callbacks and produces the response ADU. The upstream
`samples/subsys/modbus/tcp_server` demonstrates exactly this and is the template.

## Goals / Non-Goals

**Goals:**
- A believable Modbus TCP device on port 502 exercising all four object types.
- Reuse `lib/common` connectivity + data model + identity (no new data source).
- Fit the WROOM SRAM budget; robust to client reconnects (no wedge).
- `native_sim` builds; verified on Wi-Fi hardware.

**Non-Goals:**
- Modbus RTU/serial, Modbus client, Modbus/TLS, multi-unit gateways, exotic FCs.
- Any change to OPC-UA or the shared data-model semantics.

## Decisions

### D1: Zephyr `modbus` subsystem in raw-ADU mode + our own TCP listener

Use `CONFIG_MODBUS` + raw-ADU server (`MODBUS_MODE_RAW`, `modbus_init_server`
with `modbus_raw_cb`). A dedicated thread opens a TCP listener on 502, accepts one
client, reads the 6-byte MBAP header + PDU, wraps it as a `struct modbus_adu`
(`modbus_raw_get_header`), calls `modbus_raw_submit_rx`, and sends the response
from the `raw_tx_cb` (`modbus_raw_put_header` + socket write). Object access is
handled by a registered `struct modbus_user_callbacks`.

- *Alternative — a third-party Modbus stack (e.g. nanomodbus):* unnecessary; the
  in-tree subsystem is already available and integrates with Zephyr sockets.
- *Alternative — Modbus serial + a TCP-to-serial gateway:* over-complex; raw-ADU
  TCP is the direct path.

### D0: Pluggable per-app simulations behind the data-model interface

Rather than one shared model, `lib/common` exposes a data-model/simulation
*interface* (measurements + control points + a periodic step) and ships multiple
implementations selected by a Kconfig `choice` (`APP_SIM_ENVIRONMENT` default,
`APP_SIM_PUMP`). Each app sets its default in `prj.conf`; changing a firmware's
simulation is a one-line config change, not a code change.

- **Environment sim** = today's `data_source`/`controls` behavior
  (temperature/humidity/pressure + Setpoint/Running), repackaged unchanged. The
  OPC-UA app selects it, so **OPC-UA is untouched** — no node-set change, no
  forced reflash.
- **Pump sim** = the control-driven pump/motor model (D1). The Modbus app selects
  it.

`controls` gains `mode` additively (the environment sim ignores it; only the pump
sim + Modbus use it), so no existing control API/behavior changes. Frontends stay
simulation-agnostic: OPC-UA already maps measurements generically and its control
nodes (Setpoint/Running) match the environment sim; Modbus maps the pump sim per
its register map. Because the interface is shared, any frontend could later run
any simulation.

- *Alternative — one shared model reworked to pump for all frontends:* would break
  the OPC-UA demo and force reflashes for no benefit. Rejected per the "don't
  change OPC-UA" requirement.
- *Alternative — separate `lib/sim-*` modules per simulation:* cleaner isolation
  but heavier; a Kconfig choice within `lib/common` is enough for two sims. Revisit
  if simulations proliferate.

### D1: Control-driven pump simulation (stateful, time-stepped)

Measurements are no longer free-running sinusoids — they respond to the control
points via a light physical model, stepped every `CONFIG_APP_SAMPLE_INTERVAL_MS`
by a `lib/common` timer/work item (independent of any frontend). Each step reads
`running`/`mode`/`speed_setpoint` and integrates state:

- **Commanded speed %**: `off` mode or `!running` → 0; `manual` → `speed_setpoint`;
  `auto` → an internal duty target (nominal ~75 % with gentle variation) while
  running.
- **Actual speed** first-order ramps toward commanded (motor inertia), so it
  doesn't step instantly.
- **Affinity laws** off the actual speed fraction `s = speed/100`:
  - `rpm = s · RPM_MAX`
  - `flow_lpm = s · FLOW_MAX` (∝ N)
  - `pressure_bar = s² · PRESSURE_MAX` (head ∝ N²)
  - `vibration_mms = VIB_IDLE + s · VIB_GAIN` (+ small deterministic jitter)
- **Thermal model**: `motor_temp_c` first-order approaches
  `AMBIENT + s · TEMP_RISE` — heats under load, cools toward ambient when
  stopped.
- **`run_hours`** increments by `dt` only while `running`.
- **Fault (DI1)** trips when `motor_temp_c` exceeds an over-temp threshold
  (derived, not free-standing).

Jitter is deterministic (uptime/step-counter driven), not RNG, so behavior is
reproducible. Constants (`RPM_MAX`, `FLOW_MAX`, `PRESSURE_MAX`, `AMBIENT`,
`TEMP_RISE`, ramp/thermal time constants, over-temp threshold) are compile-time
in the sim module (a few exposed via Kconfig if useful).

- *Alternative — pure stateless functions of uptime:* can't express thermal lag,
  run-hours accumulation, ramping, or control coupling. Rejected — the point is
  that the device *reacts* to writes.

### D2: Register map derived from the shared data model (authoritative below)

Measurements → **Input Registers** (read-only): scaled 16-bit ints (`IR0..4`) for
compact reads plus IEEE-754 float pairs (`IR20..25`) for a few — many real devices
expose both, exercising multi-register reads. `run_hours` → a **32-bit counter**
across `IR10..11` (exercises a register pair with counter semantics). Control
points → **Holding Registers** (`HR0` speed_setpoint, `HR1` mode) and **Coil 0**
(running); status → **Discrete Inputs** (`DI0` running mirror, `DI1` fault sim,
`DI2` net-connected). Writes call the shared `app_control_*` setters so clamping
matches OPC-UA. Float/counter byte order: big-endian register pair; documented.

- *Alternative — everything in holding registers:* less realistic and doesn't
  exercise discrete/input registers or the counter. Rejected.

### D3: Single concurrent client, bounded state

A field Modbus TCP device typically serves one master. Accept one client at a
time (close/replace on a new connect) to bound RAM and avoid the connection-churn
class of problems. Keep the listener resilient: transient accept/recv errors are
retried, never fatal (same lesson as the OPC-UA listen-socket fix).

### D4: Per-app board config, no open62541

`apps/modbus-server/boards/<board>.conf` mirrors the OPC-UA Wi-Fi/net/mDNS setup
but without the open62541 heap pressure, so net-buffer pools can be modest. mDNS
advertises `_modbus._tcp` (optional) in addition to the unique hostname.

## Register map (authoritative)

| Object | Addr | Source | Notes |
|--------|------|--------|-------|
| Input Reg | 0 | flow_lpm ×10 | unsigned 16-bit |
| Input Reg | 1 | pressure_bar ×100 | unsigned 16-bit |
| Input Reg | 2 | motor_temp_c ×10 | signed 16-bit |
| Input Reg | 3 | rpm | unsigned 16-bit, direct |
| Input Reg | 4 | vibration_mms ×100 | unsigned 16-bit |
| Input Reg | 10-11 | run_hours | uint32 counter, BE pair |
| Input Reg | 20-21 | flow_lpm | IEEE-754 float, BE pair |
| Input Reg | 22-23 | pressure_bar | IEEE-754 float, BE pair |
| Input Reg | 24-25 | motor_temp_c | IEEE-754 float, BE pair |
| Holding Reg | 0 | speed_setpoint (RW) | clamped 0–100 |
| Holding Reg | 1 | mode (RW) | clamped 0–2 |
| Coil | 0 | running (RW) | |
| Discrete In | 0 | running mirror | |
| Discrete In | 1 | fault (sim) | |
| Discrete In | 2 | network connected | |

## Risks / Trade-offs

- [Zephyr modbus TCP is raw-ADU, not turnkey] → Mirror `samples/subsys/modbus/
  tcp_server` closely; validate against `pymodbus`/`mbpoll` early.
- [native_sim NSOS select() limits TCP accept, as with OPC-UA] → Treat native_sim
  as build-only for this app; verify end-to-end on Wi-Fi hardware.
- [Float register byte order confuses clients] → Document BE register pair
  explicitly; both scaled-int and float representations provided.
- [16-bit register can't hold a double] → Scale to fixed-point ints and provide
  float pairs; document scaling factors in the spec/README.
- [Reconnect / half-open sockets] → Single-client model + retry-not-fatal on
  transient socket errors; close stale client on new accept.

## Migration Plan

1. Scaffold `lib/modbus` (module.yml, Kconfig, CMake) + `apps/modbus-server`
   (compose lib/common + lib/modbus). Build `native_sim` (build-only).
2. Implement the raw-ADU TCP server + object callbacks + register map; map writes
   to `app_control_*`. Build `native_sim`.
3. Board conf for `esp32_devkitc/esp32/procpu`; flash a WROOM; verify with
   `pymodbus`/`mbpoll`: read IRs (scaled + float), read DIs, write+clamp HR0,
   toggle Coil0, illegal-address exception, and client reconnect.
4. Docs (README/SCOPE) + a client test recipe.

Rollback: additive change on a branch (new lib + app only; no edits to lib/common
or lib/opcua). Revert the branch if needed.

## Open Questions

- Advertise `_modbus._tcp` over mDNS? (Proposal: yes, cheap and consistent with
  the OPC-UA DNS-SD advertisement.)
- Expose a second writable holding register or leave `HR0` only? (Start minimal;
  extend if the collector needs more control points.)
