## Context

`lib/common/net.c` brings up Wi-Fi station mode and reconnects **only** in response
to management events (`l4_event_handler`, `wifi_event_handler`), with
`reconnect_handler` calling `wifi_connect()` once and discarding its result. A 3 s
`status_work` timer already runs to refresh diagnostics but takes no connectivity
action. When a blip fails to deliver the expected follow-up event (associated but
DHCP never binds; a connect request that no-ops because the driver is still
associated; a silent lease loss), the state machine dead-ends and the device stays
offline until a manual reboot. This was observed on two field WROOMs.

## Goals / Non-Goals

**Goals:**
- Autonomous recovery from any network disruption within seconds, no manual reboot.
- Board-agnostic fix in `lib/common` (helps OPC-UA and Modbus firmwares).
- No steady-state behavior change when the network is healthy.

**Non-Goals:**
- Frontend/data-model/simulation changes; native_sim path; new protocols/boards;
  Phase 3.

## Decisions

### D1: Connectivity watchdog reusing the 3 s `status_work`

Each `status_work` tick evaluates connectivity directly: `bad = !connected ||
read_iface_ipv4()==0`. A consecutive-bad counter drives escalation; a good tick
resets it. This is event-independent, so it covers the dead-ends where no
management event arrives. `status_work_handler` sits above the NSOS/Wi-Fi `#if`
split, so it calls a small `connectivity_watchdog()` hook that is the real logic in
the Wi-Fi branch and a no-op on native_sim (offloaded sockets) — native_sim
unaffected.

- *Alternative — a dedicated watchdog thread/timer:* redundant; `status_work`
  already ticks at a good cadence.

### D2: Escalation thresholds

- After `N_TRIGGER` consecutive bad ticks (default 2 ≈ 6 s), (re)schedule
  `reconnect_work` if not already connected — forces recovery.
- After the configured offline window (`CONFIG_APP_NET_REBOOT_TIMEOUT_S`, default
  300 s) of continuous bad ticks, `sys_reboot(SYS_REBOOT_COLD)` if
  `CONFIG_APP_NET_RECONNECT_REBOOT=y`.
- Any good tick (connected + IPv4) resets both counters.

### D3: Robust reconnect (disconnect-then-connect, reschedule on error)

`reconnect_handler`: if the driver still reports associated, first request
`NET_REQUEST_WIFI_DISCONNECT` so the subsequent `WIFI_CONNECT` can't be a no-op;
then call `wifi_connect()`. If `wifi_connect()` returns non-zero (e.g. `-EALREADY`,
transient error), reschedule `reconnect_work` (short backoff) instead of
discarding the result. Guard against reschedule storms (the watchdog is the
backstop; the event handlers still schedule too — reconnect stays idempotent via
`k_work_reschedule`).

- *Alternative — always full disconnect first:* simpler but adds an extra
  association cycle on every retry; only disconnect when state warrants.

### D4: IP-loss detection folded into the watchdog

Rather than a separate DHCP-renewal hook, the watchdog's `no IPv4 address` check
covers silent lease loss: if `connected==true` but the address is gone, mark
disconnected and recover. Keeps one code path.

### D5: Last-resort self-reboot is opt-out, generously timed

`CONFIG_APP_NET_RECONNECT_REBOOT` (default y) + `CONFIG_APP_NET_REBOOT_TIMEOUT_S`
(default 300). `select REBOOT` when enabled. A long default avoids rebooting
through transient outages while still self-healing a wedged radio/stack. Disabling
it keeps infinite retries (for benches where a reboot would hide a problem).

### D6: Status LED driven by connectivity, via the `led0` alias

A small `lib/common` status-LED module drives the board's LED from connectivity
state: **blink** (e.g. ~4 Hz via a `k_timer`/work item) while `!connected || no
IPv4`, **steady on** once connected/serving. It uses `DT_ALIAS(led0)` with the
GPIO API (`gpio_dt_spec`); it is compiled only when `CONFIG_APP_STATUS_LED=y` and
the `led0` alias exists, and is a no-op otherwise (and on native_sim). It
complements — does not replace — the TFT status on display boards. The blink is
driven independently of the 3 s `status_work` so the pattern is visibly fast.

- Boards: `esp32_devkitc` (WROOM) exposes an onboard LED (typically GPIO2) — add a
  `led0` alias in the app's board overlay if not already present. The QT Py S3 has
  a NeoPixel (addressable) rather than a plain GPIO LED; driving RGB LEDs is out of
  scope here (GPIO `led0` only) and can be added later. Display boards (S2 TFT)
  already show status on-screen.
- *Alternative — reuse `status_work` (3 s) to toggle the LED:* too slow to read as
  a blink; use a dedicated fast timer.
- *Alternative — an RGB/NeoPixel status (colour = stage):* nicer but needs the
  `led_strip` driver per board; deferred.

## Risks / Trade-offs

- [Reboot during a real but recoverable outage] → generous default (5 min of
  *continuous* offline) and opt-out; the watchdog+robust-reconnect should recover
  long before the timeout in normal cases.
- [Reboot loop if credentials/AP are permanently gone] → acceptable (device keeps
  trying after each boot); disable the reboot on benches. Document it.
- [Watchdog racing the event handlers] → all paths funnel through
  `k_work_reschedule(&reconnect_work,...)` which coalesces; reconnect is
  idempotent.
- [`sys_reboot` needs `CONFIG_REBOOT`] → `select REBOOT` under the enable symbol.
- [native_sim] → `connectivity_watchdog()` is a no-op there; no behavior change.

## Migration Plan

1. Add `connectivity_watchdog()` hook + counters; call from `status_work_handler`;
   implement real logic in the Wi-Fi branch, no-op on native_sim.
2. Make `reconnect_handler`/`wifi_connect` robust (disconnect-then-connect,
   reschedule on error); add IP-loss detection.
3. Add Kconfig (`APP_NET_RECONNECT_REBOOT`, `APP_NET_REBOOT_TIMEOUT_S`) + last-
   resort `sys_reboot`.
4. Build native_sim + WROOM; on a WROOM force AP-down / deauth / DHCP loss and
   confirm auto-recovery within seconds; confirm the reboot fires after the window;
   confirm steady-state + 300-cycle churn unaffected.

Rollback: isolated to `lib/common/net.c` + Kconfig on a branch; revert if needed.

## Open Questions

- Default reboot window — 5 min proposed. Longer (10 min) if transient outages are
  common on the target networks? (Tunable via Kconfig regardless.)
- Should a successful recovery be surfaced (e.g. a log/among the display
  diagnostics) for field debugging? (Proposal: log reconnect + reboot events; the
  display already shows connectivity stage.)
