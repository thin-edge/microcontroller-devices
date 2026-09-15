## Why

Two field OPC-UA (WROOM) devices went offline after a network blip and needed a
manual power-cycle to recover. The cause is in the shared connectivity layer
(`lib/common/net.c`): reconnection is **purely event-driven**, so a dropout that
doesn't deliver the expected follow-up Wi-Fi/L4 event dead-ends the reconnect
state machine and strands the device. It is not the data collector and not a CPU
deadlock (the server loop yields) — it is a recovery gap. Because the layer is
shared, the fix hardens every firmware (OPC-UA and Modbus, all Wi-Fi boards).

## What Changes

- **Add a periodic connectivity watchdog** (reuse the existing 3 s `status_work`):
  treat "not connected **or** no IPv4 address" for a few consecutive ticks as a
  trigger to force a reconnect. This closes the gaps where no management event
  ever arrives (associated-but-no-IP; silent IP/lease loss).
- **Make reconnect robust** (`reconnect_handler`/`wifi_connect`): before
  re-issuing `WIFI_CONNECT`, request a `WIFI_DISCONNECT` (or check driver state)
  so a stale association can't block a fresh connect; and **reschedule on any
  `wifi_connect()` error** instead of discarding the return value.
- **Detect IP loss while nominally connected**: if the interface loses its IPv4
  address while `connected` is true, mark disconnected so recovery kicks in.
- **Add a configurable last-resort self-reboot**: if the device stays offline for
  a prolonged period despite retries, `sys_reboot()` so it self-recovers instead
  of needing a manual power-cycle. Kconfig-tunable timeout, and an option to
  disable it.
- **Add an at-a-glance status LED** so it's obvious whether the *device* is on the
  network (vs. a collector-side problem): a status LED **blinks while
  disconnected/connecting** and is **steady once connected and serving**. Uses the
  board's `led0` alias (gpio-leds); board-gated (no-op where no LED is defined) and
  complementary to the existing TFT status on display boards.

## Non-goals

- Changing protocol frontends (OPC-UA/Modbus), the data model, simulations, or
  the register/node maps. This is connectivity-layer only.
- native_sim behavior (host networking; no Wi-Fi association) — unaffected.
- New protocols, boards, or Phase 3 (thin-edge.io / Cumulocity).
- Reworking the mDNS/DNS-SD advertisement.

## Capabilities

### New Capabilities
- `wifi-connectivity`: Wi-Fi station bring-up plus **resilient recovery** — a
  periodic connectivity watchdog, robust (disconnect-then-connect, error-
  rescheduling) reconnection, IP-loss detection, and a configurable last-resort
  self-reboot — so a device autonomously returns online after any network
  disruption without a manual reboot; plus a **status LED indicator** that shows
  connectivity at a glance (blink = not connected, steady = connected).

### Modified Capabilities
<!-- None materialised. Connectivity was previously implicit in the firmware; this
     introduces it as an explicit capability. No other capability's requirements
     change. -->

## Impact

- **Code:** `lib/common/net.c` (watchdog in `status_work_handler`, robust
  `reconnect_handler`/`wifi_connect`, IP-loss detection in `mark_connected`/poll,
  last-resort reboot); a small status-LED module in `lib/common` driven by the
  connectivity stage; and `lib/common/Kconfig` (watchdog thresholds, reboot
  timeout/enable, status-LED enable). No frontend or app code changes required;
  boards may add a `led0` alias / `aliases`+`leds` node to light their LED.
- **Boards:** all Wi-Fi targets (WROOM co-primary, QT Py S3); native_sim path
  (offloaded sockets) is untouched. Fixes both `apps/opcua-server` and
  `apps/modbus-server` by virtue of living in `lib/common`.
- **Resource constraints:** negligible — reuses the existing `status_work` timer
  and a few counters; `sys_reboot` needs `CONFIG_REBOOT=y`. No measurable
  flash/RAM impact and no steady-state behavior change when the network is
  healthy.
- **Verification:** on a WROOM, force AP-down / deauth / DHCP loss and confirm the
  device auto-recovers within seconds (no manual reboot); confirm the last-resort
  reboot triggers after the configured offline window; confirm steady-state OPC-UA
  browse/read and the 300-cycle churn are unaffected.
