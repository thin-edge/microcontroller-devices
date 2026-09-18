## Why

Classic-ESP32 devices running this firmware stop communicating after anything
from ~15 seconds to ~20 minutes and **never come back** without a power-cycle,
which makes every protocol app unusable in the field. On 2026-09-16 both OPC-UA
WROOMs dropped off within a minute of each other and stayed gone, and the
ESP32-CAM running the SNMP agent froze repeatedly. A serial capture of the SNMP
board shows a **whole-firmware stall** rather than a Wi-Fi drop: the console goes
silent, SNMP stops answering ~15 s later, and nothing follows — no Wi-Fi
disconnect, no crash, no brownout, no reboot for 7+ minutes. The existing
last-resort self-reboot (`wifi-connectivity`) cannot help, because it runs on the
system that has stalled. The one ESP32-S3 board (Modbus), running the same
`lib/common` connectivity code, stayed up and recovered its blips in 1–4 s.

The root cause is not yet known. This change is an investigation with a defined
exit: reproduce the stall on demand, instrument the firmware until the stall
explains itself, fix the cause, and add a safety net so any future stall
recovers on its own.

**Targets:** `esp32_devkitc/esp32/procpu` builds on the ESP32-WROOM-32 and the
bare ESP32-D0WD-V3 ESP32-CAM, which has a serial console and is the primary
reproduction board. The QT Py ESP32-S3 is the control board. All three
protocol apps are in scope: `opcua-server` (OPC-UA), `modbus-server` (Modbus
TCP) and `snmp-agent` (SNMPv2c). **Phase:** a reliability fix for Phase 1/2
firmware.

## What Changes

- **A soak harness** (host-side scripts): starts a console logger at boot (on the
  ESP32-CAM, opening the port resets the board). It polls the device's protocol
  endpoint and ping every few seconds, records traps where the app sends them,
  and timestamps everything on one host clock. It detects an outage, keeps
  recording through the firmware's recovery window, and writes a summary, so
  every run yields a comparable record.
- **Firmware liveness diagnostics** (Kconfig-gated, off by default in release
  builds):
  - A heartbeat that proves the system workqueue and the app threads are still
    scheduled.
  - A periodic health line: free heap, free net-buf/net-pkt counts, per-thread
    stack headroom, and Wi-Fi state/RSSI.
  - On a watchdog expiry, a dump of every thread's state before reset, so the
    stall leaves evidence behind.
  - The reset reason logged at boot.
- **A hardware/task watchdog** in `lib/common`: it is fed only while the system
  workqueue and the connectivity watchdog are demonstrably running, and resets
  the device if they stop. This is the safety net: whatever the root cause, a
  stalled device comes back by itself. It complements the network-level
  last-resort reboot rather than replacing it.
- **Hypothesis bisection** on the reproduction board. Each variable is changed
  on its own under the harness:
  - network buffer pools (the SNMP app's 24/24 and 8/8 against the OPC-UA fix's
    40/32 and 10/10);
  - `CONFIG_ZVFS_POLL_MAX` (3 against 6);
  - the DNS/mDNS resolver on or off;
  - the reachability ping on or off;
  - Wi-Fi power save;
  - `net.c` from before and after `wifi-reconnect-resilience`.
- **The root-cause fix**, wherever the evidence points: the shared network layer,
  board configs, an app, or a documented upstream workaround in Zephyr 4.4.2 or
  `hal_espressif`.
- **An acceptance soak**: every affected board and app must run for a sustained
  period with zero unrecovered outages before the change is done.

## Non-goals

- The Raspberry Pi Pico W port, including the status LED's `k_timer` callback,
  which drives the LED GPIO from interrupt context and hangs the firmware when
  that GPIO sits behind the CYW43 bus. That is a separate change, noted here
  because it was found during the same session.
- The ESP32-S2 Feather's upstream Wi-Fi data-path bug.
- The on-device `.local` resolution failure for the SNMP trap manager
  (`getaddrinfo` -11), unless bisection shows it is part of the stall.
- New protocol features, MIB/register/node changes, or Phase 3
  (thin-edge.io / Cumulocity) work.
- Moving to a different Zephyr release. It may be recorded as a finding if
  bisection implicates an upstream bug, but the upgrade is its own change.

## Capabilities

### New Capabilities
- `firmware-liveness`: the device detects that its own scheduling has stalled and
  recovers by resetting, independent of which thread or subsystem is stuck. It
  also leaves enough diagnostic evidence (reset reason, thread states at expiry,
  periodic resource health) for the cause of a stall to be found afterwards.

### Modified Capabilities
- `wifi-connectivity`: adds a **sustained-connectivity** requirement. A
  Wi-Fi device must stay reachable over a long soak, and any outage must end in
  automatic recovery rather than a device that stays gone. The existing
  last-resort self-reboot is clarified as the *network-level* recovery, with
  `firmware-liveness` covering the case where the firmware itself has stopped.

## Impact

- **Code:** `lib/common` (new liveness/watchdog module, health reporting, hooks
  in `net.c`'s status tick; possibly the root-cause fix itself),
  `lib/common/Kconfig` (new options: diagnostics default off; the watchdog lands
  default off and becomes default-on for Wi-Fi targets once the acceptance soak
  passes, with an opt-out for bench use), board configs under `apps/*/boards/` for the ESP32
  targets, and devicetree overlays to enable the ESP32 watchdog peripheral where
  it is not already on.
- **Tooling:** new host-side soak and console-logging scripts under `scripts/`,
  runnable on macOS.
- **Resources:** the WROOM's SNMP build uses ~13% flash and ~63% DRAM, and the
  OPC-UA build is RAM-tight (~68 KB heap). The watchdog must cost negligible
  RAM. Diagnostics must be Kconfig-gated, compile out of release builds, and
  stay within a few KB when enabled. Deeper net-buf pools, if they turn out to
  be the fix, have to fit each app's existing RAM budget.
- **Docs:** README (connectivity/troubleshooting section and how to run the
  soak), and each affected app's board notes.
- **Operations:** the two OPC-UA field devices need a power-cycle now, and a
  reflash once the fix lands.
