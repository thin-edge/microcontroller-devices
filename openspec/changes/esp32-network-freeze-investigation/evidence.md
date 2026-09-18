# Evidence: ESP32 network freeze investigation

Every decision in `design.md` and `tasks.md` cites a run from the table below.
Runs are made with `scripts/soak/run.sh` (see `scripts/soak/README.md`). Raw
logs stay out of the repo, under `scripts/soak/runs/` (git-ignored); the
file stem is given in the **Run** column so a run can be found again.

Clock: the Mac's local time (UTC+2). The tedge-dot journal on the gateway Pi
is in UTC.

## Run table

Column meanings:
- **Variant**: `baseline`, `diag` (with `overlay-diag.conf`), `live` (liveness
  on), or the bisection variable from tasks section 5.
- **Outages**: count of outages (3 consecutive failed probes) during the run.
- **TTF**: time to failure for each outage, measured from boot (SNMP uptime,
  or run start for console-reset boards).
- **Recovered**: whether each outage ended within the recovery window
  (liveness + last-resort + 120 s = 450 s), and how long it took.

The 2026-09-16 investigation runs were recorded in the session scratchpad
rather than `scripts/soak/runs/`; the stems are kept for reference. Runs
labelled "scratchpad `name`" are console captures taken without the poller.
"n/a" in **Outages** means no poller ran.

| Date | Board | App | SHA | Variant | Duration | Outages | TTF | Recovered | Run | Notes |
|---|---|---|---|---|---|---|---|---|---|---|
| 2026-09-16 | esp32-cam | snmp | d63aaf1 (clean HEAD worktree; the summary says `-dirty` because run.sh checks the main tree) | baseline-head | 60 min | 0 | – (no stall in 60 min) | – | `20260916T190617-esp32-cam-snmp-baseline-head` | 715/721 probes, 238 traps, 1 boot. The stall did not reproduce in this run: the overflow's effect depends on what it overwrites (see Findings). mDNS was dead throughout (`ZVFS_POLL_MAX=3`), so name-based polling failed while the IP answered. |
| 2026-09-16 | esp32-cam | snmp | d63aaf1-dirty | diag + liveness, fix not yet applied | ~1 min × 4 boots | n/a (console only) | 5–20 s after DHCP, every boot | yes: TG0 hardware watchdog | scratchpad `diag2` | Every boot locked up with interrupts masked right after DHCP. Cause: the overlay's immediate logging pushed the 1 KB system workqueue past its limit. |
| 2026-09-16 | esp32-cam | snmp | d63aaf1-dirty | diag only (no watchdog) | 90 s | n/a | 7 s | no: halted | scratchpad `diagonly` | `FATAL EXCEPTION` in `main`, garbage PC (stack smashed) right after DHCP. |
| 2026-09-16 | esp32-cam | snmp | d63aaf1-dirty | diag + stack sentinel | 60 s | n/a | 3.1 s | no: halted | scratchpad `sent` | `Stack overflow on CPU 0`, current thread `sysworkq`, at the first status tick. |
| 2026-09-16 | esp32-cam | snmp | d63aaf1-dirty | base-sentinel (release logging, sentinel) | 5 min | never reachable | 3.1 s | no: halted | `20260916T181455-…-base-sentinel` | `Invalid SP`, current thread `k_sys_work_q`: the overflow is there **without** diagnostics too. |
| 2026-09-16 | esp32-cam | snmp | d63aaf1-dirty | wq4096-sentinel (system workqueue 4 KB) | 4 min | 0 | – | – | `20260916T182147-…-wq4096-sentinel` | 16 traps. Thread analyzer: `sysworkq` peak **1076 B**, more than the 1024 B default. |
| 2026-09-16 | esp32-cam | snmp | d63aaf1-dirty | noping-wq1024-sentinel (reachability ping compiled out) | 4 min | never reachable | 3.1 s | no: halted | `20260916T182712-…-noping-wq1024-sentinel` | Still `Stack overflow`, `sysworkq`. The ping alone is not the cause (bisect 5.4). |
| 2026-09-16 | esp32-cam | snmp | d63aaf1-dirty | fix-sentinel (connectivity on `net_wq`) | 5 min | 0 | – | – | `20260916T183346-…-fix-sentinel` | 20 traps; `sysworkq` peak 772/1024. |
| 2026-09-16 | esp32-cam | snmp | d63aaf1-dirty | fix-sentinel-printk / -locked | 3.3 + 1.7 min | 0 | – | – | `…-fix-sentinel-printk`, `…-fix-sentinel-locked` | 13 + 6 traps. Log thread peak ~900/1024. |
| 2026-09-16 | esp32-cam | snmp | d63aaf1-dirty | live only (release logging, liveness, no fix) | 90 s | 0 | – | – | scratchpad `live` | Served normally: corruption from the overflow is latent, not immediate. |
| 2026-09-16 | esp32-cam | snmp | d63aaf1-dirty | fix-live-stacks (fix + liveness + stack report) | 5 min | 0 | – | – | `20260916T185807-…-fix-live-stacks` | 18 traps. Headroom: `net_wq` 2000/3072 free, `sysworkq` 1248/2048, log 1136/2048, **`net_socket_service` 16/1408** (then raised to 2400). |

## Observations before the harness existed (2026-09-16)

Context for the run table; these observations are not controlled runs.

- **ESP32-CAM, `snmp-agent`** (`build_snmp_wroom`, 14:26 build; extra conf:
  Wi-Fi credentials, trap manager `192.168.68.51:1162`, and
  `CONFIG_ZVFS_POLL_MAX=6`). **The image that froze already had
  `ZVFS_POLL_MAX=6`**, so variable 5.2 is already partly tested: 6 does not
  prevent the stall. The captured stall (`drop1`): boot at 17:04:34,
  `linkDown`/`linkUp` traps logged at 24 s and 39 s, then the console went
  silent and SNMP stopped answering at ~58 s.
- **Modbus QT Py S3** (`tedge-modbusf412fa5a9424`, 192.168.68.80, MAC
  `f4:12:fa:5a:94:24`). At 17:47, about 40 minutes after it was reported
  healthy, it **answered ping but not Modbus TCP**: two probe timeouts on port
  502, and `nc -z` failed. It also no longer answered mDNS browsing for
  `_modbus._tcp`. ARP resolved. So the S3 is not simply immune: its IP input
  path still answered ICMP while the TCP listener and the mDNS responder did
  not. No console was attached to confirm the cause.
  **Correction (20:30):** with the S3 on the Pi's USB, its console shows
  tedge-dot connecting over Modbus at boot and holding the connection. The
  server serves one client at a time and the listen backlog is 1, so a second
  client (the probe, `nc`) gets no answer while tedge-dot is connected. The
  17:47 symptom was most likely this contention, not a stall. The missing mDNS
  answers are explained by `ZVFS_POLL_MAX=3` (Findings).
- **OPC-UA WROOMs** (`3c:71:bf:10:c2:e4`, `30:ae:a4:e8:7e:e0`): at 17:47 neither
  was in the Mac's ARP cache and neither answered mDNS for `_opcua-tcp._tcp`.
  They are still down and need the power-cycle from task 1.5.

## Field device firmware (task 1.5)

| Device | MAC | FirmwareVersion | BuildTimestamp | Has reachability watchdog? |
|---|---|---|---|---|
| `tedge-opcua3c71bf10c2e4` | 3c:71:bf:10:c2:e4 | 0.2.0 (`zephyr-opcua-server`) | Sep 15 2026 13:50:09 | Partly (see below) |
| `tedge-opcua30aea4e87ee0` | 30:ae:a4:e8:7e:e0 | 0.2.0 (`zephyr-opcua-server`) | Sep 15 2026 13:50:09 | Partly (see below) |

Read over OPC-UA on 2026-09-16 at about 20:20, after the boards were moved
(power-cycled) to the Pi's USB ports. Both also printed a boot log for the
first time. `BuildTimestamp` is the build container's clock (UTC), so the image
was built at 15:50 local on 2026-09-15:
- That is 3 minutes before the resilience commit `7027479` (15:53), so it is a
  working-tree build of that change.
- It predates `ab33ce1` (16:12) and the reachability gate `7b1b3d8` (16:37).

It does send the gateway ping: the boot log shows the `Gateway not set ... ARP`
lines from `ping_gateway_once()`. So these images have the same overflowing
status tick, which is consistent with both freezing. The exact source is
inferred from timestamps; the image doesn't record a git revision.

The reachability watchdog (gateway ping in `net.c`'s `status_work`) landed with
`wifi-reconnect-resilience`. The last local WROOM OPC-UA build, `build_wroom`
from 2026-09-15 16:36, includes it.

## Test setup from 2026-09-16 20:30: boards on the gateway Pi

All four boards were moved to the USB ports of the gateway Pi
(`rpi5-d83add9f145a`, 192.168.68.65). `scripts/soak/remote.sh` flashes them
there and runs the harness detached. Records are under `/opt/soak/runs` and are
pulled to `scripts/soak/runs/`. **The Pi's clock is Europe/London (BST, UTC+1)**,
so its run stems are 1 hour behind this Mac's CEST. Traps go to the Pi (`soak-trap-pi.local.conf`) and
are recorded by poll.py's built-in listener.

| Pi port (`/dev/serial/by-path/platform-…`) | Board | MAC | Firmware under test | IP |
|---|---|---|---|---|
| `xhci-hcd.1-usb-0:2:1.0-port0` (CH340) | ESP32-CAM (D0WD-V3) | e4:65:b8:6f:97:cc | `snmp-agent` | 192.168.68.74 |
| `xhci-hcd.1-usb-0:1:1.0-port0` (CP2102) | WROOM "3c71" (D0WDQ6 v1.0) | 3c:71:bf:10:c2:e4 | `opcua-server` (tedge-dot polls it) | 192.168.68.68 |
| `xhci-hcd.0-usb-0:1:1.0-port0` (CP2102) | WROOM "30ae" (D0WDQ6 v1.0) | 30:ae:a4:e8:7e:e0 | `modbus-server` | 192.168.68.76 |
| `xhci-hcd.0-usb-0:2:1.0` (USB-JTAG) | QT Py S3 | f4:12:fa:5a:94:24 | `modbus-server`, probed through tedge-dot (see below) | 192.168.68.80 |

tedge-dot on the Pi polls the S3 over Modbus (`tedge-modbusf412fa5a9424.local`,
every 10 s) and both WROOMs over OPC-UA.

tedge-dot holds the S3's only Modbus connection, so its soak uses
`poll.py --app mqtt`. That probe counts the device as serving while tedge-dot
has published one of its measurements (`te/device/tedge-modbusf412fa5a9424///m/+`)
in the last 30 s, which tests the real collector path.

Collector config changes on the Pi: the `tedge-opcua30aea4e87ee0` entry in
`/etc/tedge/plugins/ot/opcua.toml` is disabled, because that board now runs the
Modbus firmware. The original is `opcua.toml.bak-20260916`, and tedge-dot was
restarted.

Incidental fix (not part of the freeze): the pump point library used
`decimal_shift = 1` / `2`, but tedge-dot computes `value x 10^decimal_shift`, so
readings were published 100 to 10,000 times too large (`motor_temp_c: 2500`).
The shifts are now negative and `motor_temp_c` is `int16`, matching the
firmware's signed encoding. The Pi's copy
(`/usr/share/tedge-dot/points.d/modbus/zephyr-pump.toml`, backup in
`/root/zephyr-pump.toml.bak-20260916`) was replaced at 20:47 and tedge-dot
restarted. `vibration_mms: 6553200` was a second bug: the simulation's jitter
went negative for a stopped pump and wrapped in the unsigned register. It is
now clamped in `sim_pump.c`; that takes a reflash, deferred until the running
soaks end.

The first local acceptance run, `20260916T200921-esp32-cam-snmp-accept-fix-live`,
lasted 13.5 minutes with no outage before the board was unplugged to move it.

## Liveness self-tests (task 3.5)

`CONFIG_APP_LIVENESS_SELFTEST` images (`snmp-agent`, `overlay-diag.conf`,
liveness on, injection 45 s after boot) ran on the ESP32-CAM. Each injection
repeated on every boot of a 150 s capture (scratchpad `st/*.console.log`).

| Mode | Resets | Time from injection to reset | Boot log | ROM reset |
|---|---|---|---|---|
| `BLOCK_WQ` (work item blocks on a semaphore) | 2 of 2 | 27 s (the last beat came ~3 s before injection) | `LIVENESS RESET: context wq stalled at uptime 72 s` | `RTCWDT_RTC_RESET` |
| `STOP_PROTO` (SNMP agent loop blocks) | 2 of 2 | 27–28 s | `LIVENESS RESET: context proto stalled at uptime 73 s` | `RTCWDT_RTC_RESET` |
| `IRQ_LOCK` (coop busy loop, interrupts masked) | 3 of 3 | 4–5 s | `hardware watchdog reset: no liveness record, the task watchdog never ran` | `TG0WDT_SYS_RESET` |

- Before each liveness reset, the expiry callback printed a thread dump. In
  `BLOCK_WQ`, `sysworkq ... pended_on 0x3ffb3d80` resolved with `nm` to
  `3ffb3d80 00000018 d liveness_selftest_block_sem`, which is exactly the
  injected object.
- The health lines showed `wq` ageing (8 → 18 → 28 s) while `probe` and `sim`
  stayed `Q`.
- The hardware fallback resets after ~5 s rather than the configured 10 s,
  because the Zephyr ESP32 watchdog driver programs the millisecond timeout into
  a 0.5 ms tick (`MWDT_TICK_PRESCALER` 40000), so each of the two stages lasts
  2.5 s. That is fine as a lockup detector; it is recorded here so nobody
  "fixes" the Kconfig numbers to match.
- A software reset on the ESP32 goes through the RTC watchdog, so hwinfo
  reports the cause as `watchdog` (`esp_reason 7`). The RTC-memory record is
  what identifies a liveness reset.

## Reading diagnostic output

### Health line (`CONFIG_APP_DIAG`)

The diagnostics thread prints one line every `CONFIG_APP_DIAG_PERIOD_S`:

```
<inf> app_diag: HEALTH up=120 n=12 beat[wq=1 proto=0 trap=0] work[status=- reconn=- sim=-] heap=9876/16384 pkt[rx=8/8 tx=8/8] buf[rx=24/24 tx=24/24] min[rx=19 tx=21]
<inf> app_diag: WIFI st=9 rssi=-61 ch=11
```

- `beat[...]`: seconds since each watched context last made progress. `-`
  means the context has not started yet (for example, no trap sender).
- `work[...]`: the `k_work_busy_get()` state of the firmware's own work items:
  `R` running, `Q` queued, `D` delayed (scheduled), `C` cancelling, `-` idle.
  A workqueue stall shows as `wq` growing while one item stays `R`.
- `heap`: free and total bytes of the Zephyr system heap.
- `pkt` and `buf`: free and total counts of the net packet slabs and net
  buffer pools; `min` is the lowest free buffer count seen so far.
- The `WIFI` line comes from a separate `net_mgmt` query, printed after the
  health line. If the Wi-Fi driver is wedged, that query can block the
  diagnostics thread. The evidence for that is a `HEALTH` line with no `WIFI`
  line after it, followed by silence.

### Stale report

When a heartbeat is older than 2 periods (20 s by default, so the report comes
before a 30 s liveness reset), the diagnostics thread prints a `STALE` line for
that context and dumps every thread:

```
<wrn> app_diag: STALE wq age=35s thread=sysworkq state=pending pended_on=0x3ffb1234
<wrn> app_diag:   thread sysworkq prio -1 state pending pended_on 0x3ffb1234 stack 612/2048
```

`pended_on` is the address of the wait queue inside the kernel object the
thread is blocked on (a mutex, semaphore, condition variable or queue). To
name it, look the address up in the ELF of **the exact build that ran**:

```sh
# in the zephyr-dev container, from the build directory
nm -n -S zephyr/zephyr.elf | awk '$1 <= "3ffb1234"' | tail -3
# or, when the object is a static symbol with debug info:
xtensa-espressif_esp32_zephyr-elf-addr2line -e zephyr/zephyr.elf 0x3ffb1234
```

`nm -n` sorts symbols by address; the last symbol at or below the address is
the object, provided the address falls within that symbol's size (`-S`). The
wait queue is usually a few bytes into the object: `wait_q` is the first member
of `struct k_sem` and follows the owner pointer in `struct k_mutex`. An address
inside the heap or a thread stack has no symbol, and means a dynamically
allocated object. In that case, the owning thread's name and state are the
evidence to use.

### Reset record (`CONFIG_APP_LIVENESS`)

At boot the liveness module prints the SoC reset cause and, if the previous
reset was its own, the record it saved:

```
<err> app_liveness: LIVENESS RESET: context wq stalled at uptime 1234 s (boot 3)
<inf> app_liveness: reset cause 0x2 (software) esp_reason 3
```

`hardware watchdog reset` in place of the first line means the task watchdog
never ran its callback: a hard lockup that only `wdt0` caught.

## Findings

### Root cause: system workqueue stack overflow (classic ESP32 builds)

**Mechanism.**
- `lib/common/net.c` runs its 3 s status tick and Wi-Fi reconnects as work
  items. Until this change those items ran on the Zephyr system workqueue,
  whose stack is `CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=1024` on every ESP32
  build here.
- One tick does the following on that stack:
  - renders the status lines: a `NET_REQUEST_WIFI_IFACE_STATUS` query into a
    large `struct wifi_iface_status`, plus `snprintf` formatting;
  - runs the reachability watchdog, which can issue blocking
    `NET_REQUEST_WIFI_CONNECT` / `DISCONNECT` requests into the Espressif
    driver;
  - sends the gateway ping. With `CONFIG_NET_TC_TX_COUNT=0`, the whole
    IPv4/ARP/driver transmit path runs on the caller's stack, including the
    `net_arp` "Gateway not set" error log.
- The measured peak is **1076 bytes**. The overflow runs off the bottom of the
  system workqueue stack into whatever the linker placed below it. Nothing
  checks for that in a release build, so the corruption is silent. What
  happens next depends on which bytes get hit and on timing: a thread that
  later runs on a smashed frame, a corrupted kernel object, or a lock held with
  interrupts masked.
- That fits every field observation: the console goes silent with no fault
  message, the device stops answering, it never recovers, and the 300 s
  last-resort reboot (which runs on the same, now dead, workqueue) never fires.
- The time to failure varies (15 s to 20 min), because the deepest paths
  (association retries, a disconnect, a failed ping) depend on the network.

**Evidence** (run table above):
1. The stack sentinel reports `Stack overflow ... sysworkq` at the first status
   tick (3.1 s), both with the diagnostic overlay and with release logging
   (`base-sentinel`: `Invalid SP` in `k_sys_work_q`).
2. With a 4 KB system workqueue, the thread analyzer measures a 1076-byte peak
   and the device serves normally (`wq4096-sentinel`).
3. Compiling the ping out does not remove the overflow (`noping-wq1024-sentinel`),
   so the status tick as a whole is too deep, not one call.
4. Moving the connectivity work to its own 3 KB queue removes the overflow; the
   queue peaks at 1072 bytes (`fix-sentinel`, `fix-live-stacks`).
5. Immediate logging (the diagnostic overlay) formats every message on the
   caller's stack, which makes the same overflow deterministic: a lockup or
   `main` crash within seconds of DHCP on every boot (`diag2`, `diagonly`).
   Release logging leaves it latent (`live`), which matches the minutes-long
   field time to failure.

**Why the QT Py S3 looked healthier.** Its build has the same 1 KB system
workqueue, so it overflows too. The S3's different memory map decides what the
overflow hits, which makes it plausible that the damage there is milder or
slower rather than absent. The 17:47 observation (ping answered, Modbus and
mDNS not) is consistent with partial corruption. The S3 control soak with the
fix (task 7.3) will show it.

### Contributing findings, fixed alongside

- **`CONFIG_ZVFS_POLL_MAX=3` (bisect 5.2).** Every field build logged
  `You have 4 services to monitor but 3 poll entries configured`, and
  `Socket service thread not running`. The mDNS responder and DNS-SD therefore
  never ran, which is why the devices stopped answering mDNS browsing even
  while they served. Now set to 6.
- **`net_socket_service` stack.** Once the socket service thread runs, it peaks
  at 1392 of its 1408 bytes (16 bytes free) with release logging. The frozen
  ESP32-CAM image had `ZVFS_POLL_MAX=6`, so this second near-overflow was live
  there too. Now 2400.
- **Log thread stack.** Peaks at ~900 of 1024 bytes. Now 2048 on ESP32 Wi-Fi
  builds (a `configdefault` in `lib/common/Kconfig`).
- **System workqueue.** With the connectivity work moved off it, it still peaks
  at ~800 bytes of 1024 (Zephyr's own items plus the simulation step). Now 2048.
- **Modbus single-client server.** A half-open client (one that vanished
  during an outage) held the only connection slot forever, while ping still
  answered. The server now drops a client that is silent for
  `CONFIG_APP_MODBUS_CLIENT_IDLE_TIMEOUT_S` (60 s).

### Mid-soak check (2026-09-17 12:42 CEST, ~16 h)

- **Totals:** ~11,500 probes per board. No reboots, no liveness or hardware
  watchdog resets, no fatal errors, no `Failed to allocate`.
- **Outages:** one, on the CAM (09:01:43, 15 s, recovered, uptime unbroken;
  heartbeats fresh, RSSI -85 to -88 dBm). Most likely radio loss.
- **`STALE` lines:** all false alarms from an age-computation race (task 9.10).
- **Socket-service stack:** the lowest headroom seen on the soak boards was
  672 of 2400 bytes, meaning ~1,730 bytes used. That is more than the
  1,408-byte default, so the raise to 2400 was necessary, not just margin.

### 24 h acceptance soaks, finished 2026-09-17 ~20:36-20:45 CEST (tasks 7.1-7.3)

All four boards ran the fixed firmware with the liveness watchdog on, for
24.0 h each, 17,281 probes each (5 s apart). **Zero unrecovered outages.**

| Board | App | Probes OK | Outages (all recovered) | Boots | Liveness resets | Last-resort reboots | Crashes / hw-wdt |
|---|---|---|---|---|---|---|---|
| ESP32-CAM | `snmp-agent` | 17,097 | 3: 15 s, 235 s, 15 s | 3 | 2 | 0 | 0 / 0 |
| WROOM "3c71" | `opcua-server` | 17,221 | 2: 175 s, 20 s | 1 | 0 | 0 | 0 / 0 |
| WROOM "30ae" | `modbus-server` | 17,198 | 1: 230 s | 3 | 2 | 0 | 0 / 0 |
| QT Py S3 | `modbus-server` (via tedge-dot) | 17,228 | 1: 245 s | 2 | 1 | 0 | 0 / 0 |

- **Every outage except two** falls in the 11:50-11:54 access-point restart
  (see the next section). The exceptions are the CAM's two 15 s blips
  (09:01:39 and 19:03:44), both with unbroken uptime and fresh heartbeats:
  radio loss on a weak link (RSSI averages -84.5 dBm).
- **The CAM's traps kept flowing:** 5,712 received over the run.
- **No memory drift over 24 h.** The CAM's system heap read 23,056 bytes free
  at both ends; the OPC-UA board's malloc arena went 26,884 -> 26,872 bytes
  free (12 bytes, flat in practice); the S3 and "30ae" were unchanged.
- **Net buffers:** the three 24-buffer builds each hit `min[rx=0]` at some
  point with no allocation failure logged; the 40-buffer OPC-UA build bottomed
  out at 16 (task 9.11).
- **Tightest stacks at the end:** `conn_mgr_monitor` 192 of 512 free on every
  board, then `net_socket_service` 672 of 2400 and `rx_q[0]` 736 of 2048
  (task 9.7).
- **False `STALE` lines** (`age=4294967s`) still appeared: 196 on the OPC-UA
  board, 3-6 elsewhere (task 9.10, fixed in the tree).
- **The S3's ROM reset reasons** read `RTC_SW_CPU_RST` and
  `USB_UART_CHIP_RESET` rather than the classic ESP32's `RTCWDT_RTC_RESET`;
  its reset record still named the stalled context correctly.

### Access-point restart during the soak (2026-09-17, tasks 3.6 / 7.4): FAILED the "no false reset" criterion

All Deco units were rebooted from 12:49:39 CEST (11:49:39 on the Pi's BST
clock; times below are on the Pi's clock). The Pi itself is on the same Wi-Fi.
It lost its link at 11:50:06, re-associated at 11:51:25, and had an address
again at 11:52:18.

| Board | Liveness resets (`netwq`) | Service back at | Outage |
|---|---|---|---|
| WROOM "3c71" (OPC-UA) | 0 | 11:53:03 (one more 20 s blip at 11:53:27) | 175 s |
| WROOM "30ae" (Modbus) | 2: 11:50:39 (uptime 58473 s), 11:53:50 (uptime 189 s) | 11:53:57 | 230 s |
| ESP32-CAM (SNMP) | 2: 11:50:39 (uptime 58480 s), 11:53:52 (uptime 192 s) | 11:54:04 | 235 s |
| QT Py S3 (Modbus, via tedge-dot) | 1: 11:53:52 (uptime 58180 s) | 11:54:42 | 245 s |

- **Recovery:** every board recovered by itself, well inside the 450 s bound.
  But the spec scenario "healthy device is never reset, including reconnects
  after an access-point restart" is **not met**.
- **What stalled:** only the connectivity queue (`netwq`), in two waves. The
  first came ~30 s after the APs vanished. The second came at ~11:53:20, while
  the mesh was coming back and the boards were associating: all three stalls
  expired within 3 s of each other at ~11:53:51.
- **Which boards stalled:** the two boards that stalled in the first wave never
  got a Wi-Fi disconnect event. They saw only the L4 `Network connectivity
  lost`, followed by `esp32_wifi: Failed to send packet` on the CAM. The OPC-UA
  WROOM and the S3 got `Wi-Fi disconnected (reason 0)` right away.
- **Signature:** on the CAM, the diagnostics thread's last `HEALTH` line
  (16:14:09.8) has no `WIFI` line after it, and no further health lines
  follow. The Wi-Fi status query (`net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS)`)
  blocked in the diagnostics thread. `net_wq` makes the same query in every
  status tick (`render_status()`), and it also issues the blocking connect and
  disconnect requests.
- **The query itself:** the Espressif driver's `esp32_wifi_status()` calls
  `esp_wifi_get_config()` and `esp_wifi_sta_get_ap_info()`. Those wait on the
  Wi-Fi library's own task, which is busy with the lost or returning AP.
- **Gap:** which call blocked is not proven. The pre-reset thread dump printed
  only its header with deferred logging (task 9.14).
- **Conclusion:** `net_wq` can legitimately block for more than 30 s inside
  Espressif Wi-Fi driver calls while an AP disappears or returns. Design D3
  assumed Wi-Fi management calls don't hold the queue that long; that
  assumption is wrong. See tasks 9.13 to 9.16.

### Fixes and reflash, 2026-09-17 20:18 CEST

Deployed to all four boards, and fresh 24 h soaks started (`fixed2-live`):
- the connectivity queue keeps its own 120 s liveness timeout (`netwq`), the
  other contexts stay at 30 s;
- the blocking Wi-Fi status query is compiled out where there is no display;
- `app_step()` breadcrumbs name the step in stall reports, the reset message
  and the reset record;
- the pre-reset thread dump moved out of the timer ISR into the
  `app_liveness_reaper` thread, so it prints with deferred logging;
- the false-stall age fix, the pump vibration clamp and the DNS-SD TXT record.

Verified on hardware before the reflash (BLOCK_WQ self-test, deferred logging):
`STALE wq age=28s step='probe'`, then `liveness: wq made no progress for 30 s
in step 'probe' - resetting`, the full 15-line thread dump, and on the next
boot `LIVENESS RESET: context wq stalled in step 'probe' at uptime 72 s`.

### Final-image soak, 2026-09-17 22:02 -> 2026-09-18 07:58 CEST (9.93 h)

The build that also passed the AP-restart test, on all four boards
(`20260917T2102*-*-final-live*`). Shortened from 24 h at the user's request.

| Board | App | Probes OK | Outages | Boots | Liveness resets | STALE lines | Crashes / hw-wdt |
|---|---|---|---|---|---|---|---|
| ESP32-CAM | `snmp-agent` | 7,088 / 7,148 | 1 (15 s, recovered) | 1 | 0 | 0 | 0 / 0 |
| WROOM "3c71" | `opcua-server` | 7,145 / 7,148 | 0 | 1 | 0 | 0 | 0 / 0 |
| WROOM "30ae" | `modbus-server` | 7,138 / 7,148 | 0 | 1 | 0 | 0 | 0 / 0 |
| QT Py S3 | `modbus-server` (via tedge-dot) | 7,143 / 7,148 | 0 | 1 | 0 | 0 | 0 / 0 |

- **No board rebooted**: one power-on each, from the flash, and ~9.93 h of
  unbroken uptime.
- **Zero `STALE` lines**, against 196 on the OPC-UA board in the previous soak:
  the age-wrap fix (9.10) works.
- **The CAM's single 15 s outage** (02:01:29) left uptime unbroken: another
  radio blip on its weak link (9.12).
- **Memory flat**: system heap 23,120 -> 23,012 bytes free on the CAM, malloc
  arenas identical at both ends on all four.
- **Net buffers**: the 24-buffer builds dipped to `min[rx=4]` (previous soak:
  0), the 40-buffer OPC-UA build to 20. Still the tightest resource (9.11).
- **Tightest stack** remains `conn_mgr_monitor` at 192 of 512 bytes free (9.7).
- **Pump readings are correct** with the clamp and the corrected point library
  (9.1): a running pump reports `vibration_mms: 3` and `motor_temp_c: 76.5`,
  matching the float copy 76.07. No wrapped values.
- **No traps were received** (previous soak: 5,712). The firmware sent 2,381
  linkUp/linkDown events to `192.168.68.65:1162`, the Pi's Wi-Fi address when
  the image was built; after the AP restart the Pi moved to `eth0`
  (192.168.68.70). A harness address issue, not a firmware fault: rebuild with
  the current manager IP before the next trap-carrying run.

### Access-point restart #2, after the fixes (2026-09-17 21:25:50 CEST): PASSED

Decos restarted again, 7 minutes into the `fixed2-live` soaks. Times below are
on the Pi's clock (CEST - 1 h).

| Board | Liveness resets | STALE reports | Reboots | Board reconnected | Probe outage |
|---|---|---|---|---|---|
| ESP32-CAM (snmp) | 0 | 0 | 0 | 20:29:25 | 325 s |
| WROOM "3c71" (opcua) | 0 | 0 | 0 | 20:29:11 | 325 s |
| WROOM "30ae" (modbus) | 0 | 0 | 0 | 20:29:07 | 325 s |
| QT Py S3 (modbus via tedge-dot) | 0 | 0 | 0 | 20:26:25 (link), readings from 20:31:57 | 305 s |

- **Against five liveness resets in the 2026-09-17 11:50 restart**, this time
  there were none, and no context ever went stale: removing the blocking Wi-Fi
  status query stopped the stall happening at all, rather than just raising the
  timeout past it.
- **The boards beat the gateway.** They had addresses again by 20:29:03-20:29:25,
  about 3 minutes after the restart, while the Pi only got a lease at 20:31:44.
  The recorded outages (305-325 s) are bounded by the Pi's own downtime, not by
  the devices; from the Mac, all four answered at their old addresses while the
  Pi was still offline.
- **Every board got a clean `Wi-Fi disconnected (reason 0)` event this time**,
  including the two that got none in the first restart. Whether that is the
  fix or AP-side variation is not established; it removes the case that
  previously wedged them either way.
- The Pi came back on `eth0` with a new address (192.168.68.70, was
  192.168.68.65 on `wlan0`), so the gateway path changed between the two tests.

### mDNS / DNS-SD: devices verified, macOS browsing is a client-side quirk

Checked 2026-09-17 21:40-22:00 with the boards on the Pi, tcpdump on the Pi and
a `zeroconf` browser in `/opt/soakenv`.

**The devices are discoverable.** From the Pi, a DNS-SD browse finds all four
with address, port and TXT:

```
tedge-modbus30aea4e87ee0._modbus._tcp.local.   192.168.68.76:502   txtvers=1
tedge-modbusf412fa5a9424._modbus._tcp.local.   192.168.68.80:502   txtvers=1
tedge-opcua3c71bf10c2e4._opcua-tcp._tcp.local. 192.168.68.68:4840  txtvers=1
tedge-snmpe465b86f97cc._snmp._udp.local.       192.168.68.74:161   txtvers=1
```

Raw PTR queries from either host are answered with PTR + TXT + SRV + A.
Hostname (A) lookups work everywhere, which is what tedge-dot uses.

**macOS `dns-sd -B` lists nothing for our three service types, and that is
local to the Mac.** Captures on the Pi show it sends **no query at all** for
`_snmp._udp`, `_opcua-tcp._tcp` or `_modbus._tcp`, while it queries normally
for a made-up type and browses `_ssh._tcp` fine (4 hosts). Restarting
`mDNSResponder` (`sudo killall -HUP mDNSResponder`) changed which types
answered from cache but did not make it query for ours. Not pursued further:
the firmware side is proven.

**Correction to the earlier entry here:** the zero-length TXT record was *not*
the cause. A board still running the pre-fix image (empty TXT) was found by
both the Pi's browser and, in one window, by macOS. The TXT fix
(`txtvers=1`) is kept because RFC 6763 6.1 requires at least one byte, but it
did not change discoverability.

**Not reported upstream:** neither the zero-length TXT nor the unanswered
SRV/TXT queries is proven to break a real client, so nothing was filed.

**Still true of Zephyr 4.4.2:** direct SRV and TXT queries are unanswered
(`TODO` in `subsys/net/lib/dns/dns_sd.c`), so `dns-sd -L`-style lookups that
ask for those records directly get nothing; browsers that read SRV/TXT from
the PTR response's additional section (the Pi's) work.

### Discarded or deprioritised hypotheses

- **Net buffer starvation (5.1).** The health lines never showed the pools
  near empty (`min[rx=14..24 tx=20..22]` of 24) during these runs, and no
  `Failed to allocate net buffer` was logged. Deeper pools are not needed for
  the stall.
- **Reachability ping (5.4).** It is on the overflowing path but is not
  required for the overflow.
- **`net.c` before `wifi-reconnect-resilience` (5.6).** The status render with
  the Wi-Fi status query and the ping predate that change (`d0258da`,
  `3edc7eb`). The resilience work added reconnect calls to the same tick,
  making the deep path more frequent, but it did not create it. The OPC-UA
  field boards starting to freeze after the resilience build fits this.
- **DNS/mDNS resolver (5.3), Wi-Fi power save (5.5), upstream
  `hal_espressif` (5.7).** Not needed: the overflow fully explains the
  reproduced failures, and the fixed image runs clean. They stay open only if
  the acceptance soak shows a residual stall.
