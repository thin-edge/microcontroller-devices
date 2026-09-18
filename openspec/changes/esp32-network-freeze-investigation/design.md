## Context

**What was observed (2026-09-16).** Mac clock, UTC+2; the tedge-dot journal on the
gateway Pi is in UTC.

| Board | App | Behaviour |
|---|---|---|
| ESP32-CAM (bare ESP32-D0WD-V3, console on `/dev/cu.usbserial-210`) | `snmp-agent` | Went unreachable at ~15 s, ~1 min, ~2 min and ~20 min uptime across runs; ran 5+ min cleanly in between. |
| WROOM `tedge-opcua3c71bf10c2e4` | `opcua-server` | Gone from 17:09:40 and never back: no mDNS answer, tedge-dot gets `BadConnectionClosed`. |
| WROOM `tedge-opcua30aea4e87ee0` | `opcua-server` | Gone from 17:10:40, a minute after the first, the same way. |
| QT Py ESP32-S3 (inferred from the `f4:12:fa` MAC), `tedge-modbusf412fa5a9424` | `modbus-server` | Stayed up. Three short blips recovered in 1–4 s. |

The one full console capture (ESP32-CAM, `snmp-agent`) showed this timeline:

- 0–9 s: boot. Two "association failed (-1)" retries, then DHCP. The agent and
  the trap sender start.
- 24 s and 39 s: the trap sender logs `linkDown` and then `linkUp` for ifIndex 4.
- ~54 s: the next flap is due, but **nothing is logged**. SNMP polls still get
  answers up to this point.
- ~58 s onward: the host gets no replies and ARP stays incomplete.
- For the next 7 minutes the console prints **nothing at all**: no
  `Wi-Fi disconnected`, no `Connectivity lost`, no fatal error, no reboot.

**Why the silence is informative** (thread map of the current firmware):

- *System workqueue.* Runs `net.c`'s `status_work`, a 3 s connectivity watchdog
  that includes the gateway reachability ping and, after 300 s offline, the
  last-resort reboot. It also runs `reconnect_work`, which issues blocking
  `net_mgmt` Wi-Fi requests and `esp_wifi_*` calls, and the simulation step
  (`sim_switch` / `sim_pump`).
- *Dedicated threads.* The protocol server (`snmp_agent`, `modbus_server`,
  `opcua_server`) and the SNMP trap sender. The trap sender logs only when the
  *simulation* changes an interface state, and those changes happen on the
  system workqueue.
- *Interrupt context.* The status LED `k_timer`.

The trap sender going quiet while the agent thread kept answering is therefore
what a **stopped system workqueue** looks like: the simulation stopped stepping,
so there was nothing to trap. A stopped workqueue would also explain why
`Connectivity lost` never appeared, even though the reachability probe logs it
within about 15 s of replies stopping, and why the 300 s reboot never fired. This
is the leading hypothesis, not a finding. A fully wedged scheduler or log
thread would look similar on this evidence.

**Constraints**
- **Resources.** The WROOM `snmp-agent` build uses ~13% flash and ~63% DRAM, and
  the OPC-UA build is RAM-tight (~68 KB heap). Diagnostics must be gated;
  the watchdog must be cheap.
- **Console access.**
  - Only the ESP32-CAM has a usable serial console, and opening it resets the
    board, so a capture has to start at boot.
  - The WROOMs have no console.
  - The watchdog peripheral `wdt0` (`espressif,esp32-watchdog`, TIMG0) is
    enabled and aliased `watchdog0` on `esp32_devkitc`, and Zephyr's `task_wdt`
    with hardware fallback is available.
- **Platform.** Zephyr v4.4.2 with the `hal_espressif` pinned by `west.yml`.
  Builds run in the `zephyr-dev` container; flashing and captures run on the
  macOS host.

## Goals / Non-Goals

**Goals:**
- Make the stall reproducible on demand and measurable: time to failure, and
  whether and how the board recovers.
- Make a stall explain itself: which context stopped, what it was running, and
  what resources looked like beforehand.
- Find and fix the root cause, and prove it with a long soak.
- Guarantee self-recovery from any future stall with a watchdog that does not
  depend on the stalled system.

**Non-Goals:**
- The Pico W port and its LED-from-interrupt bug, the ESP32-S2 upstream data-path
  bug, and the `.local` trap-manager resolution issue, unless bisection ties
  them to this stall.
- A Zephyr or `hal_espressif` upgrade. If evidence points upstream it is recorded
  here, and the upgrade becomes its own change.
- Remote log shipping or crash upload. The reset reason is reported locally;
  field reporting is an open question.

## Decisions

### D1. Reproduce first, on the ESP32-CAM, with one harness for every experiment
The investigation is only as good as its measurements, so a host-side harness
runs every experiment the same way:
- It opens the console before the board starts. Opening the port *is* the reset,
  so the whole boot log is captured.
- It stamps every console line with host time.
- It polls the app's endpoint (SNMP GET, Modbus TCP connect, OPC-UA TCP connect)
  and ping every 5 s, and records traps where the app sends them.
- It declares an outage after 3 consecutive failures, and keeps recording for
  at least the recovery window (liveness timeout + last-resort timeout + rejoin).
- It writes a per-run summary: board, app, git SHA, config variant, duration,
  outages, and time to recover or "not recovered".

Results go into `evidence.md` in this change as a run table, so decisions cite
runs. Raw logs stay out of the repo.
*Alternatives:* ad-hoc captures, which is what we have now. They produced one
good capture and several misleading ones: a board reset by opening its console,
and output lost to pipe buffering.

### D2. Diagnostics that run *outside* the thing they observe
A Kconfig-gated diagnostics module (`CONFIG_APP_DIAG`, default `n`) with its own
low-overhead thread, deliberately **not** on the system workqueue.
- Monitored contexts (the system workqueue and each protocol thread) bump a
  heartbeat counter.
- Every N seconds the diagnostics thread logs a health line:
  - the age of each heartbeat;
  - which of the firmware's own work items is currently executing
    (`k_work_busy_get` on `status_work`, `reconnect_work` and the simulation
    step);
  - free system heap;
  - free net_pkt/net_buf counts (`CONFIG_NET_BUF_POOL_USAGE`);
  - Wi-Fi association state and RSSI.
- When a heartbeat goes stale, it also logs the stuck thread's state and the
  object it is waiting on. That address maps to a symbol with `nm`, which
  identifies the mutex or semaphore involved.

A diagnostic overlay (`overlay-diag.conf`) turns this on together with
**immediate log mode**, so lines logged just before a stall are not lost in the
deferred log buffer, plus the thread analyzer and exception stack traces.

The health line classifies each stall into one of four cases:
- **Workqueue blocked:** the workqueue heartbeat goes stale, the diagnostics
  thread keeps printing, and a work item shows as running.
- **Protocol thread stuck:** that thread's heartbeat goes stale and the
  workqueue stays fresh.
- **Hard lockup:** the diagnostics thread goes silent too, and only the
  hardware watchdog fires.
- **Network-only:** everything stays fresh while Wi-Fi reports disassociated or
  the reachability probe fails.

*Alternatives:* a JTAG debugger (none on these boards), or reading
Zephyr's coredump (possible later, but it needs flash space and tooling
we don't have set up).

### D3. A liveness watchdog built on `task_wdt` with hardware fallback
A `lib/common` liveness module (`CONFIG_APP_LIVENESS`) registers one `task_wdt`
channel per monitored context and is fed **only by genuine progress**:
- the system-workqueue channel from `status_work` (every 3 s);
- each protocol channel from its server loop.

`CONFIG_TASK_WDT_HW_FALLBACK` arms `wdt0`, so a lockup that stops even the
`task_wdt` timer still resets the device.

On a channel expiry, the callback saves a small reset record (channel ID and
uptime) in `__noinit` RAM and triggers a reset. At boot the module logs that
record plus the SoC reset reason, which distinguishes a hardware-watchdog reset
from a brownout or power-on.

Timeouts sit above legitimate worst-case blocking, but that case turned out to
be far larger than assumed for connectivity work. **Measured 2026-09-17:**
Espressif Wi-Fi management and status calls hold the connectivity queue for
more than 30 s while an access point disappears or returns — an AP restart
produced five resets across three boards, all false positives. Association and
DHCP themselves are asynchronous and do not, but `esp32_wifi_status()`
(`esp_wifi_get_config()`, `esp_wifi_sta_get_ap_info()`) and the connect and
disconnect requests wait on the Wi-Fi library's own task.

So the timeouts are per context: 30 s for the system workqueue and the protocol
threads, and `CONFIG_APP_LIVENESS_NETWQ_TIMEOUT_S` (default 120 s) for the
connectivity queue. The blocking status query was also removed from the status
tick where no display needs it, which stopped the stall occurring at all: the
repeat AP restart produced no reset and no stale context.

The hardware fallback is shorter than the ESP32 driver's nominal figure: it
programs the millisecond timeout into a 0.5 ms tick, so the two stages expire
in about 5 s rather than 10 s. That is fine for a lockup detector.

The module lands default-off while it is validated, and becomes **default-on
for Wi-Fi targets** once the soak passes (task 7.4).
*Alternatives:*
- Feeding the hardware watchdog from a timer: rejected, because it keeps
  feeding while the workqueue is dead.
- Extending the existing 300 s software reboot: rejected, because it runs on the
  workqueue that stalls.
- The ESP-IDF task watchdog: not available in the Zephyr build.

### D4. Bisect one variable at a time, cheapest first, against a measured baseline
**Baseline.** The current `snmp-agent` image on the ESP32-CAM, measured over
several runs to get a time-to-failure distribution. The same image with
diagnostics on is measured too, because immediate logging changes timing and
could hide the bug.

**Variables**, in order. Each is changed alone on top of the baseline and run
for at least several times the baseline's mean time to failure.
1. Net buffer pools (24/24 and 8/8 → 40/32 and 10/10, the OPC-UA wedge fix).
2. `CONFIG_ZVFS_POLL_MAX` 3 → 6 (the socket-service registration error).
3. DNS/mDNS resolver off, with a literal trap-manager IP.
4. Reachability ping compiled out.
5. Wi-Fi power save: confirm it stays off, and try the ESP32 driver's
   defaults.
6. `net.c` from before `wifi-reconnect-resilience` (`5109ddc`) against HEAD.
   The two OPC-UA boards most likely run a build with the resilience code.

**Control.** The same harness on the QT Py S3, to establish whether it is
immune or merely luckier.

**Order.** The stall classes from D2 set the order: a blocked-workqueue finding
jumps straight to D5.

### D5. If the workqueue is blocked by driver calls, isolate connectivity work
If the evidence shows a blocking `net_mgmt` / `esp_wifi_*` / ICMP call wedging
the system workqueue, the structural fix is to run `net.c`'s status and
reconnect work on a **dedicated connectivity work queue** with its own liveness
channel. A blocked driver call then can't freeze the simulations or other
workqueue users, and the watchdog names it precisely. This is a candidate
outcome, not a pre-decision. The root-cause fix follows the evidence, and if
the cause is upstream it gets a documented workaround.

### D6. Acceptance is a long soak, not a short demo
- **Unrecovered outage:** unreachable for longer than the recovery bound
  (liveness timeout + bring-up for a stall; last-resort timeout + bring-up for a
  network outage). This is the hard failure.
- **Acceptance:** a 24 h soak per ESP32 board and app with **zero unrecovered
  outages**. For the ESP32-CAM `snmp-agent`, which failed within ~20 min, the
  24 h run is at least 70× the longest observed time to failure.
- **Watchdog resets** are allowed only if they are counted and attributed. A
  recurring reset that points at an unfixed cause blocks completion.
- **Soak scenarios:** an AP restart and brief out-of-range periods.

## Outcome (root cause)

The stall was a **system workqueue stack overflow**. `net.c`'s status tick
(Wi-Fi status query, formatting, reconnect requests and the gateway ping, whose
transmit path runs on the caller's stack) peaks at 1076 bytes. The ESP32
builds give the system workqueue 1024. The overflow silently corrupts
neighbouring memory, and the firmware later locks up with no fault message.
Because the last-resort reboot ran on the same workqueue, it could never fire.
The stack sentinel caught it at the first tick, with and without the diagnostic
overlay. Full evidence and the discarded hypotheses are in `evidence.md`
("Findings").

D5 was taken: connectivity work runs on a dedicated `net_wq` queue
(`CONFIG_APP_NET_WORKQ_STACK_SIZE`, default 3072, measured peak 1072) with its
own liveness context (`netwq`). A small probe item keeps the system workqueue
itself watched (`wq`). Measurement also found three more stacks at or near
their limits, all raised on the ESP32 targets:
- the system workqueue (2048);
- the deferred log thread (2048);
- the socket-service thread, which only runs once `ZVFS_POLL_MAX` is raised
  from 3 to 6. Without that raise, mDNS never worked (2400).

Two details from the implementation:
- The diagnostics `STALE` threshold is 2 periods, not 3, so the report is
  printed before a 30 s liveness reset.
- The ESP32 hardware watchdog fallback fires after ~5 s, not the configured
  10 s, because of the driver's 0.5 ms tick.

## Risks / Trade-offs

- **[Heisenbug]** Immediate logging and a diagnostics thread change timing and
  may hide the stall. → Measure a baseline with and without diagnostics, keep
  the health period long (≥ 10 s), and fall back to reset records alone if the
  stall disappears under diagnostics.
- **[The watchdog masks the bug]** A self-resetting device looks healthy.
  → Every watchdog reset is recorded and reported at boot, the soak counts them,
  and the change is not done until the cause is fixed or documented with
  evidence.
- **[False resets / reset loops]** A slow association or DHCP could trip the
  watchdog. → Timeouts sit far above the observed worst case (associating took
  ≤ 12 s), the AP-restart and out-of-range scenarios are part of acceptance,
  and a reset record from the previous boot is logged so a loop is visible.
- **[No console on the WROOMs]** Their stalls stay opaque. → Reproduce on the
  ESP32-CAM, which uses the same `esp32_devkitc` target and chip family. For
  the WROOMs, rely on reset records and the soak (see Open Questions).
- **[The cause is upstream]** It may be in Zephyr 4.4.2 or `hal_espressif`.
  → Apply a workaround (D5) and document it; the upgrade becomes its own change.
- **[RAM budget]** Deeper pools may not fit the OPC-UA build. → Size per app,
  and measure against the existing DRAM and heap numbers.
- **[Console reset]** Opening the ESP32-CAM's console resets it. → The harness
  treats the open as the start of a run and never re-opens mid-run.

## Migration Plan

1. Land the harness, diagnostics (default off) and liveness module (default off)
   with no behaviour change for existing builds.
2. Run the investigation using the diagnostic overlay and liveness enabled
   through overlays.
3. Land the root-cause fix and the config changes proven by bisection across
   all ESP32 board configs of the three apps.
4. After the soak passes, make liveness default-on for Wi-Fi targets.
5. Reflash the field devices: the two OPC-UA WROOMs, the Modbus board and the
   ESP32-CAM.

**Rollback:** `CONFIG_APP_LIVENESS=n` and `CONFIG_APP_DIAG=n` restore today's
behaviour. The root-cause fix is an ordinary revertible commit.

## Open Questions

- Which firmware are the two OPC-UA field devices actually running? Once
  they're power-cycled, read `FirmwareVersion`/`BuildTimestamp` from them
  (task 1.5).
- Is the S3 immune, or did it just survive this window? (control soak)
- Did the AP (TP-Link Deco, `192.168.68.0/24`) log anything around 17:09–17:10
  (roaming, channel change, DHCP)? Both OPC-UA boards dropped a minute apart.
- Should the last reset reason be exposed through each protocol, so
  console-less field devices can report a stall remotely? For example an SNMP
  enterprise object, an OPC-UA node, or a Modbus input register. That would be
  a follow-up change if the answer is yes.
- Is making the watchdog default-on the right default for every Wi-Fi board,
  including development boards on the bench? The existing reboot has an opt-out
  for bench use, and the same would apply.
