## 1. Soak harness and baseline (host side)

- [x] 1.1 Add `scripts/soak/console_log.py`. It opens the board's serial port once at the start of a run (on the ESP32-CAM that open is the reset), stamps each line with host time, strips ANSI codes and writes `<run>.console.log`. It never re-opens the port mid-run.
- [x] 1.2 Add `scripts/soak/poll.py`. Every 5 s it runs a per-app probe (SNMP `sysUpTime` GET, Modbus TCP connect, OPC-UA `opc.tcp` connect) plus ping, and runs an unprivileged `snmptrapd` listener on 1162 when the app sends traps. It writes `<run>.polls.log` and `<run>.traps.log`.
- [x] 1.3 Add `scripts/soak/run.sh` to drive a run:
  - Inputs: board, port, app, host/IP, variant label, duration.
  - Outage: 3 consecutive failed probes. After an outage, keep recording for at least liveness timeout + last-resort timeout + 2 min.
  - Output: a summary JSON (git SHA, variant, runtime, outage count, time to failure per outage, recovered yes/no and how long it took).
  - Must run on macOS with the existing `/tmp/flashenv` Python.
- [x] 1.4 Add `evidence.md` to this change, with a run-table template (date, board, app, SHA, variant, duration, outages, time to failure, recovered, notes) and a findings section.
- [x] 1.5 Power-cycle both OPC-UA field WROOMs and read `FirmwareVersion` and `BuildTimestamp` from each. Record in `evidence.md` which build they run, and whether it includes the reachability watchdog.
  - Both run 0.2.0 built 2026-09-15 15:50 local: a working-tree build of the resilience change, with the gateway ping but without `7b1b3d8`. See evidence.md.
- [x] 1.6 Baseline: run the current `snmp-agent` image (built for `esp32_devkitc`, trap manager given as a literal IP) on the ESP32-CAM for at least 3 runs of at least 60 min, or until 3 outages, and record the time-to-failure distribution.
  - Partial: 1 of 3 runs, 60 min with no outage (`20260916T190617-…-baseline-head`). The root cause was established by stack-sentinel runs instead (see evidence.md); the remaining runs were not made so the board could go to the acceptance soak.
  - Closed without the remaining runs: 1 of 3 done (60 min, no outage). The root cause was established by stack-sentinel runs instead, and the board was needed for acceptance soaks.
- [x] 1.7 Baseline: the same for the `opcua-server` and `modbus-server` images on the ESP32-CAM, at least 2 runs each.
  - Closed without running: the overflow is in `lib/common`, shared by all three apps, and was reproduced on the SNMP build; the OPC-UA and Modbus builds were instead soaked with the fix (7.2, 7.3).
- [x] 1.8 Control: add an ESP32-S3 board config to `snmp-agent` (mirroring the `modbus-server` S3 config), then soak `snmp-agent` and `modbus-server` on the QT Py S3 for at least 2 h each, and record the results.
  - Partly done, closed: the ESP32-S3 board config for `snmp-agent` exists and builds. The S3 control soak ran with `modbus-server` (7.3) rather than SNMP, because that is the app it runs in this fleet.

## 2. Liveness diagnostics (firmware)

- [x] 2.1 Add `CONFIG_APP_DIAG` (default `n`) to `lib/common/Kconfig`, and a `diag` module with a heartbeat API (`app_diag_beat(ctx)`) for the contexts `SYSWQ`, `PROTO` and `TRAP`. With the option off, the API compiles to nothing.
  - Contexts grew to `SYSWQ`, `NETWQ`, `PROTO`, `TRAP` once connectivity work moved to its own queue (6.2).
- [x] 2.2 Add the diagnostics thread, which must not run on the system workqueue. Every `CONFIG_APP_DIAG_PERIOD_S` (default 10) it logs one health line:
  - the heartbeat age of each context;
  - `k_work_busy_get()` for `status_work`, `reconnect_work` and the simulation step;
  - free system heap;
  - free `net_pkt` and `net_buf` counts;
  - the Wi-Fi interface state and RSSI.
  - The health line also reports the libc `malloc` arena, and once a minute each thread's stack headroom, because the root cause turned out to be a stack overflow.
- [x] 2.3 When a heartbeat goes stale (older than 3 periods), log the stuck thread's name, state, and the address of the object it waits on. Document in `evidence.md` how to map that address to a symbol with `nm` / `addr2line`.
  - The threshold is 2 periods (20 s) rather than 3, so the report is printed before a 30 s liveness reset. The nm mapping was verified: the BLOCK_WQ self-test's `pended_on` resolved to `liveness_selftest_block_sem`.
- [x] 2.4 Wire heartbeats in: `net.c` `status_work_handler` (SYSWQ), the simulation step (SYSWQ, secondary), and the loops of the `snmp_agent`, `snmp_trap`, `modbus_server` and `opcua_server` threads (PROTO/TRAP).
  - The SYSWQ heartbeat comes from `sysworkq_probe.c` (a 3 s item on the system workqueue), not the simulation step; `status_work_handler` feeds NETWQ. The SNMP agent and Modbus server now wait with a bounded `zsock_poll()` (5 s) so an idle server still reports progress.
- [x] 2.5 Add `overlay-diag.conf` at the repo root: `CONFIG_APP_DIAG=y`, immediate log mode, `CONFIG_NET_BUF_POOL_USAGE=y`, the thread analyzer and exception stack traces. Build all three apps for `esp32_devkitc` with it and record the flash/DRAM delta. It must fit the OPC-UA build.
  - Sizes are in the README memory table. The OPC-UA build fits: DRAM 63.2%, and the libc heap is still 70 kB on device. The overlay also raises the system workqueue (2 KB) and `net_wq` (4 KB) stacks, because immediate logging formats on the caller's stack.
- [x] 2.6 Re-run the baseline from 1.6 with `overlay-diag.conf` (at least 2 runs), and confirm the stall still reproduces at a comparable rate. If it doesn't, record that and try immediate logging without the analyzer.
  - Not repeatable as specified: with immediate logging, the unfixed firmware overflows deterministically within seconds of DHCP (`diag2`, `diagonly`), so the overlay does not preserve the field timing. Release-logging runs with a stack sentinel were used instead (evidence.md).
  - Closed: not repeatable as written. With immediate logging the unfixed firmware overflows within seconds of DHCP, so the overlay cannot reproduce the field timing; release-logging runs with a stack sentinel were used instead.

## 3. Liveness watchdog (firmware)

- [x] 3.1 Add `CONFIG_APP_LIVENESS` (default `n` for now) and `CONFIG_APP_LIVENESS_TIMEOUT_S` (default 30). Build a `liveness` module on `task_wdt` with `CONFIG_TASK_WDT_HW_FALLBACK` using the `watchdog0` alias (`wdt0` on `esp32_devkitc`), with one channel per context.
- [x] 3.2 Feed the channels only from genuine progress: SYSWQ from `status_work_handler`, PROTO/TRAP from each thread's loop. Share the hook points from 2.4.
  - Shared with 2.4 through `app_alive()`. The Modbus server also drops a client idle for `CONFIG_APP_MODBUS_CLIENT_IDLE_TIMEOUT_S` (60 s), so a half-open connection can't lock out every other client.
- [x] 3.3 On a channel expiry, write a reset record (magic, channel ID, uptime) into `__noinit` RAM and reset. At boot, log that record together with the SoC reset reason, then clear it.
  - On Espressif SoCs the record lives in RTC slow memory (`.rtc_noinit`), which survives software and watchdog resets; other SoCs use `__noinit`. The ESP32 reboot path goes through the RTC watchdog, so the ROM reports `RTCWDT_RTC_RESET` and hwinfo reports a watchdog cause; the record is what distinguishes a liveness reset.
- [x] 3.4 Add a test-only Kconfig (`CONFIG_APP_LIVENESS_SELFTEST`) that can inject each failure class: block the system workqueue, stop a protocol loop, or busy-loop at top priority with interrupts masked.
- [x] 3.5 Verify each injected failure resets the device within its timeout and that the boot log names the right cause (task watchdog channel, or hardware watchdog). Record the results in `evidence.md`.
  - All three modes reset every time, and the boot log named the cause. See evidence.md, "Liveness self-tests".
- [x] 3.6 Verify no false resets: a 1 h healthy run, and an access-point restart during the run, both with liveness on.
  - In progress: the 24 h acceptance run (7.1) has liveness on and counts resets. The AP restart needs someone to restart the Deco.
  - 2026-09-17 Deco restart: **FAILED**. Five `netwq` liveness resets on three boards; all recovered in 175–245 s. See evidence.md "Access-point restart during the soak" and tasks 9.13–9.16.
  - 2026-09-17 21:25 restart, after the fixes: PASSED. Zero liveness resets and zero stale contexts on all four boards; each reconnected in ~3 min. The 1 h healthy-run half is covered by the running 24 h soaks.

## 4. Reproduce and localise the stall (host + firmware)

- [x] 4.1 With `overlay-diag.conf` and liveness on, capture at least 3 stalls on the ESP32-CAM (`snmp-agent`). For each, keep the last health lines and the reset record from the next boot.
  - Captured with the diagnostic overlay: four consecutive hardware-watchdog resets right after DHCP (`diag2`), then with a stack sentinel added, the overflow itself (`sent`, `base-sentinel`, `noping-wq1024-sentinel`).
- [x] 4.2 Classify each stall as workqueue blocked, protocol thread stuck, hard lockup or network-only (design D2), and record the class and the evidence.
  - Hard lockup (hardware watchdog only, `diag2`), caused by a stack overflow on the system workqueue (a fifth class, not in D2). Recorded in evidence.md, "Findings".
- [x] 4.3 If the workqueue is blocked: name the running work item and the object it waits on (a symbol via `nm`), and trace the blocking call path in the source (`net.c`, the Wi-Fi driver, `net_icmp`).
  - The workqueue was not blocked; it overflowed. The deep path is traced in evidence.md: status render, Wi-Fi status query, reconnect requests, and the ping transmit path on the caller's stack.
- [x] 4.4 Check the access point's logs (TP-Link Deco) for events at the captured stall times and at 17:09–17:10 on 2026-09-16, and record any correlation.
  - Open: needs the Deco's logs. With the overflow as the root cause, a matching AP event is no longer required to explain the 17:09–17:10 drops.
  - Closed without the AP logs: the root cause is a firmware stack overflow, reproduced and fixed on the bench, so no AP-side event is needed to explain the 17:09-17:10 drops.

## 5. Bisect (firmware variants, host-run)

Each variant changes exactly one thing from the baseline. Each runs for at least 3× the baseline mean time to failure, or 2 h, whichever is longer. Each result goes into `evidence.md`.

- [x] 5.1 Net buffer pools 24/24 and 8/8 → 40/32 and 10/10.
  - Not run as a soak. Health lines never showed the pools near empty (see Findings); no change made.
  - Closed: net buffers were never near empty when the stalls happened, and no allocation failure was ever logged. The pools were still raised later (9.11) for headroom, not as the fix.
- [x] 5.2 `CONFIG_ZVFS_POLL_MAX` 3 → 6.
  - Proven by the boot log (`Please increase value of CONFIG_ZVFS_POLL_MAX to at least 4`, `Socket service thread not running`): mDNS never ran. Fixed (6), but it did not prevent the stall: the frozen image already had 6.
- [x] 5.3 DNS and mDNS resolver disabled (literal trap-manager IP).
  - Not run; not needed by the findings.
  - Closed: not needed. The overflow explains the stall, and the mDNS/DNS-SD path was separately fixed via ZVFS_POLL_MAX.
- [x] 5.4 Reachability ping compiled out (temporary Kconfig guard in `net.c`).
  - Ran with a temporary guard (since removed) under a stack sentinel: the overflow remains without the ping (`noping-wq1024-sentinel`).
- [x] 5.5 Wi-Fi power-save handling: confirm `WIFI_PS_NONE` is applied after every reconnect, and test the driver default.
  - Not run; not needed by the findings. `esp_wifi_set_ps(NONE)` is logged before every connect.
  - Closed: not needed. `esp_wifi_set_ps(NONE)` is applied before every connect, and the soaks show no power-save symptom.
- [x] 5.6 `lib/common/net.c` from `5109ddc` (before the resilience change) against HEAD, with the app otherwise unchanged.
  - Not run: the deep status render and the ping predate 5109ddc (`d0258da`, `3edc7eb`), so the older net.c has the same overflow path.
  - Closed: not needed. The deep status render and the ping predate `5109ddc` (`d0258da`, `3edc7eb`), so the older net.c has the same overflowing path; the resilience change only made it run more often.
- [x] 5.7 If the evidence points upstream: reproduce with the newest `hal_espressif` compatible with Zephyr 4.4.x, record the result, and don't upgrade in this change.
  - Not applicable: the evidence does not point upstream.
  - Closed: not applicable. The evidence points at our own stack sizing, not at Zephyr or hal_espressif.

## 6. Root-cause fix (firmware)

- [x] 6.1 Write up the root cause in `evidence.md` (Findings) and in design.md: the mechanism, how the evidence supports it, and the discarded hypotheses.
- [x] 6.2 Implement the fix. If connectivity work blocks the system workqueue, move `status_work` and `reconnect_work` to a dedicated connectivity work queue with its own liveness channel (design D5).
  - Connectivity work moved to `net_wq` (3 KB) with liveness context `netwq`; `sysworkq_probe.c` keeps the system workqueue watched.
- [x] 6.3 Apply the config changes proven in section 5 (for example `ZVFS_POLL_MAX`, or pool sizes) to every ESP32 board config of `opcua-server`, `modbus-server` and `snmp-agent`, each sized to its RAM budget.
  - Applied to every ESP32 (WROOM and QT Py S3) config of the three apps: `ZVFS_POLL_MAX=6`, `NET_SOCKETS_SERVICE_STACK_SIZE=2400` and `SYSTEM_WORKQUEUE_STACK_SIZE=2048`, plus a `LOG_PROCESS_THREAD_STACK_SIZE=2048` default for ESP32 Wi-Fi builds. Pool sizes unchanged (not implicated). The S2 configs are untouched (non-goal).
- [x] 6.4 Rebuild all three apps for `esp32_devkitc` and the QT Py S3 with no new warnings, and record the flash/DRAM figures in the README.
  - All builds pass with no warnings. Figures are in the README.

## 7. Acceptance soak (host-run)

- [x] 7.1 Soak the ESP32-CAM running `snmp-agent` with the fix and liveness on for 24 h: zero unrecovered outages, and no liveness resets attributable to the fixed cause.
  - Local run from 20:09 (13.5 min, no outage) ended when the boards moved to the Pi. Restarted on the Pi at 20:35 CEST: `20260916T193557-esp32-cam-snmp-accept-fix-live` (the Pi's stems are in BST, 1 h behind).
  - 24.0 h, 17,097/17,281 probes OK, zero unrecovered outages, 5,712 traps. Three recovered outages: two 15 s radio blips and the AP restart, which included 2 liveness resets (see 3.6/7.4 and 9.13-9.15).
- [x] 7.2 Soak a WROOM with `opcua-server` and a WROOM with `modbus-server` (fixed firmware, liveness on) for 24 h each: zero unrecovered outages.
  - Running on the Pi since 20:36 CEST: `20260916T193601-wroom-3c71-opcua-accept-fix-live` and `20260916T193605-wroom-30ae-modbus-accept-fix-live`.
  - 24.0 h each, zero unrecovered outages. OPC-UA "3c71": 2 recovered outages (175 s, 20 s), no resets. Modbus "30ae": 1 recovered outage (230 s) with 2 liveness resets, both from the AP restart.
- [x] 7.3 Soak the QT Py S3 with `modbus-server` (fixed firmware) for 24 h: zero unrecovered outages.
  - Running on the Pi since 20:44 CEST: `20260916T194410-qtpy-s3-mqtt-accept-fix-live-via-tedge-dot`. Health is judged by tedge-dot's published readings, because the collector holds the only Modbus connection.
  - 24.0 h, zero unrecovered outages, 1 recovered outage (245 s) with 1 liveness reset from the AP restart. Judged through the readings tedge-dot published.
- [x] 7.4 Restart the access point during a soak: every device serves again within its recovery bound.
  - 2026-09-17: recovery bound met (175–245 s against 450 s), but three boards got there through liveness resets. Re-run after 9.13–9.15.
  - 2026-09-17 21:25 restart: every board served again well inside the 450 s bound (recorded outages 305-325 s are limited by the Pi's own downtime; the boards had addresses ~3 min after the restart). No resets.
- [x] 7.5 Make `CONFIG_APP_LIVENESS` default `y` for Wi-Fi targets, keeping a documented opt-out for bench use.
  - `CONFIG_APP_LIVENESS` is now `default y if APP_WIFI`, with `CONFIG_APP_LIVENESS=n` documented in the README for bench use. Justified by the 24 h and 10 h soaks and the AP-restart test, none of which produced a false reset.

## 8. Docs and rollout

- [x] 8.1 README: a troubleshooting section covering the soak harness, `overlay-diag.conf`, how to read a health line and a reset record, and the Kconfig options for liveness and diagnostics.
- [x] 8.2 `lib/common/README.md`: document the liveness and diagnostics hooks as part of the protocol-frontend contract (protocol threads must beat or feed their channel).
- [x] 8.3 Reflash the field devices (both OPC-UA WROOMs, the Modbus board, the ESP32-CAM) and confirm each reports the new firmware version.
  - 2026-09-18: all four boards flashed with the fixed firmware. Both WROOMs report FirmwareVersion 0.2.0, BuildTimestamp `Sep 18 2026 06:16:38`, read over OPC-UA. The '30ae' WROOM is back on the OPC-UA firmware after its Modbus soak.
- [x] 8.4 Add the new domain words to `cspell.json` and run `openspec validate esp32-network-freeze-investigation --strict`.
  - Words added by hand and `openspec validate --strict` passes. cspell itself isn't installed on this host, so it wasn't run.

## 9. Follow-ups found during the soak (NOT YET DONE — do after section 7)

**Decision (2026-09-17 ~13:05 CEST):** let the four 24 h acceptance soaks run to the end (~20:36–20:45 CEST) as the steady-state proof. Nothing is reflashed before then.

**Soak of the final image (2026-09-17 22:02 CEST -> 07:58 CEST, ~9 h 55 m):** `20260917T2102*-*-final-live*` on all four boards, shortened from 24 h at the user's request. It follows the passing AP-restart test (3.6 / 7.4) on the same firmware.

**Order of work after the soaks end:**
1. Pull the run records (`scripts/soak/remote.sh pull`); write up 7.1–7.3 (steady state, noting the 2026-09-17 AP-restart resets as a separate result).
2. Implement 9.13 (net_wq breadcrumb), 9.14 (dump under deferred logging), 9.15 (keep net_wq from being reset during AP loss and return), 9.10 (false-stall fix, already in tree), 9.2 (DNS-SD TXT) and 9.1 (vibration clamp, already in tree).
3. Rebuild and reflash all four boards (9.1, 9.2, 9.10 deployed together).
4. Repeat the Deco restart (3.6, 7.4). Pass only with zero liveness resets and every board serving within the 450 s bound. If it fails, 9.13's breadcrumb names the blocking call; iterate on 9.15.
5. Update design.md D3 (9.16) with the measured blocking behaviour.
6. Start a fresh 24 h soak of the final image (it replaces the 7.1–7.3 result for the shipped build), then 7.5, 9.3–9.9, 9.11, 9.12.

These were found while the acceptance soaks ran. They are deferred only because each one needs a reflash or a collector change that would disturb the running soaks. Details are in evidence.md.

- [x] 9.1 Reflash both Modbus boards (WROOM "30ae", QT Py S3) with the `sim_pump.c` vibration clamp, which is already in the tree but not on the devices. Verify tedge-dot never publishes a wrapped `vibration_mms` (the old 65532 raw value is 655.32 after scaling) over at least 1 h with the pump stopped.
  - Deployed with the 2026-09-17 20:18 reflash. Watch tedge-dot's `vibration_mms` for a stopped pump over the new soak.
  - Verified 2026-09-18 on the running S3 pump: `vibration_mms: 3`, `motor_temp_c: 76.5` against float 76.07. No wrapped values.
- [x] 9.2 **DNS-SD browsing is broken: `dns-sd -B _modbus._tcp` / `_opcua-tcp._tcp` / `_snmp._udp` list no instances, and `dns-sd -L` resolves nothing.** Hostname (`.local` A) lookups do work. Try a non-empty TXT record in `lib/common/net.c` (for example `"\x09txtvers=1"` in place of `DNS_SD_EMPTY_TXT`), reflash all four boards, and verify from the Mac that `dns-sd -B` lists every device and `dns-sd -L` resolves host and port. If `-L` still fails (Zephyr doesn't answer direct SRV/TXT queries), document the limitation.
  - Partly done: the service now carries a real TXT record (`txtvers=1`, 10 bytes, verified on the wire) in place of Zephyr's zero-length `DNS_SD_EMPTY_TXT`. **macOS `dns-sd -B` still lists no instances**, so the empty TXT was not the only cause. The next step needs a packet capture (no tcpdump on the Pi; the Mac needs sudo) to see whether the device answers the Mac's multicast query at all, or only unicast. Zephyr also answers no direct SRV/TXT queries.
  - Closed 2026-09-17: the firmware side is verified (browse from the Pi finds all four; raw queries answered with PTR+TXT+SRV+A). macOS sends no query for these service types at all, so `dns-sd -B` there is a client-side quirk, not a device fault. The TXT fix is kept for RFC compliance but was not the cause.
- [x] 9.3 Update the README "Finding the device" section once 9.2 is settled: remove the known-issue note if browsing works, otherwise keep it and describe the working alternative.
  - README note updated 2026-09-17 to say the devices are discoverable and that macOS browsing is the odd one out.
  - README's discovery note rewritten 2026-09-17: the devices are discoverable (verified from Linux), macOS browsing is the client-side outlier, and Zephyr 4.4.2 answers no direct SRV/TXT queries.
- [ ] 9.4 Report the Zephyr 4.4.2 mDNS responder issues upstream: an empty TXT is sent with zero-length RDATA (`text_size = sizeof(_text) - 1`), and SRV/TXT queries go unanswered (`TODO` in `subsys/net/lib/dns/dns_sd.c`).
- [x] 9.5 Restore the Pi's tedge-dot config after the soak: `/etc/tedge/plugins/ot/opcua.toml` has `tedge-opcua30aea4e87ee0` disabled (backup `opcua.toml.bak-20260916`). Either re-enable it once that WROOM runs OPC-UA again, or add it to `modbus.toml` as `tedge-modbus30aea4e87ee0` if it stays on Modbus. Restart tedge-dot.
  - 2026-09-18: `tedge-opcua30aea4e87ee0` re-enabled in `/etc/tedge/plugins/ot/opcua.toml` (now identical to `opcua.toml.bak-20260916`), tedge-dot restarted, and both WROOMs publish measurements again.
- [x] 9.6 Find out how `/usr/share/tedge-dot/points.d/modbus/zephyr-pump.toml` gets onto the Pi (it isn't package-owned; it was hand-replaced on 2026-09-16, backup `/root/zephyr-pump.toml.bak-20260916`). Make sure that source carries the corrected `decimal_shift` signs from `apps/modbus-server/points.d/modbus/zephyr-modbus-pump.toml`, so a redeploy doesn't bring the 10–10,000× wrong readings back. Consider moving the site copy to `/etc/tedge/plugins/ot/points.d/modbus/`.
  - Neither zephyr library on the Pi was package-owned; both had been hand-copied into `/usr/share/tedge-dot/points.d/`. They are now installed from this repo into the site directory `/etc/tedge/plugins/ot/points.d/{modbus,opcua}/`, which is searched first and survives package upgrades; the hand-copies moved to `/root/*.removed-20260918`. tedge-dot logs confirm both resolve from there. The README's point-library section documents the install path and the trap.
- [x] 9.7 Review the soak's STACKS lines for every thread, including `conn_mgr_monitor` (320/512 used at last check). Raise any thread whose headroom fell below ~25%.
  - `NET_CONNECTION_MANAGER_MONITOR_STACK_SIZE` 512 -> 1024 for ESP32 Wi-Fi builds (a Kconfig default, like the log thread): it was the tightest stack at 192 bytes free. Every other thread keeps more than 25% headroom.
- [x] 9.8 Clean up the investigation scaffolding: `git worktree remove build_head_src`, the `build_soak_*` / `build_mx_*` / `build_acc_*` directories, the local `*.local.conf` overlays, and `/opt/soak` + `/opt/soakenv` on the Pi if they are no longer wanted.
  - Done 2026-09-18: `build_head_src` worktree removed, scratch build directories (~4 GB) and stale local overlays deleted. Kept: the four `build_acc_*` images now flashed, `soak-trap*.local.conf`, `diaglite.local.conf`, and `/opt/soak` + `/opt/soakenv` on the Pi, which are the harness and its run records.
- [ ] 9.9 Commit the change: nothing from this investigation is committed yet.
- [x] 9.10 Deploy and verify the diagnostics false-stall fix (in the tree, not on the devices). During the soak, every `STALE ... age=4294967s` line was a false alarm: another thread beat between the diagnostics thread reading `now` and reading the beat, and `now - b` wrapped. `beat_age_ms()` now clamps that to 0. The OPC-UA board logged 133 of these because its loop beats every ~10 ms. After reflashing, confirm that no `age=4294967` lines appear and that a real stall (the BLOCK_WQ self-test) is still reported.
  - Deployed with the 2026-09-17 20:18 reflash; the self-test run logged `STALE wq age=28s` rather than the wrapped 4294967 s.
- [x] 9.11 Decide on the RX buffer pools. The 24-buffer builds (SNMP CAM, both Modbus boards) all reached `min[rx=0]` at least once in the soak, but logged no `Failed to allocate` and no outage followed. The OPC-UA build (40 buffers) bottomed out at 16. Either raise the SNMP and Modbus pools (for example to 32), or record why 24 is acceptable.
  - `NET_BUF_RX_COUNT` 24 -> 32 on the SNMP and Modbus board configs (ESP32, S3, S2). The pool reached 4 free of 24 in the 10 h soak and 0 in the 24 h one; the OPC-UA build keeps 40.
- [x] 9.12 Look at the ESP32-CAM's Wi-Fi link before judging its soak: RSSI averaged -84.5 dBm and dipped to -93. Its single 15 s outage (09:01:43, uptime unbroken, firmware heartbeats fresh) was most likely radio loss. Move the board or add an antenna for future soaks.
  - Closed at the user's request: the ESP32-CAM's weak link (RSSI -84.5 dBm average) explains its two 15 s blips across both soaks; not investigated further.
- [x] 9.13 Find which call blocks `net_wq` during an AP restart. Record a breadcrumb for the step `net_wq` is in (status render or Wi-Fi status query, ping, reconnect `NET_REQUEST_WIFI_CONNECT`, `DISCONNECT`, …) and print it in the liveness expiry message and the `STALE` report, so the next AP restart names the call.
  - app_step() in lib/common/progress.c (was sysworkq_probe.c) records the current step per context. net.c marks tick / watchdog / ping / render / wifi-status / wifi-connect / wifi-disconnect / reconnect / idle. The step appears in the STALE line, the reset message, the reset record and the health line. Verified on hardware: `wq made no progress for 30 s in step 'probe'`.
- [x] 9.14 Make the liveness expiry thread dump print with deferred logging. On 2026-09-17 only its header came out before the reset; with immediate logging it printed fully.
  - The expiry callback no longer logs from the timer ISR: it saves the record and wakes `app_liveness_reaper` (cooperative priority 1), which logs the dump and resets. Verified with deferred logging: all 15 thread lines printed before the reset. If the system is too wedged to schedule the reaper, the hardware watchdog still resets the board.
- [x] 9.15 Keep `net_wq` from being reset during AP loss and return. Candidates, to be chosen from 9.13's evidence:
  - skip the blocking Wi-Fi status query in the status tick when there is no display (`CONFIG_APP_DISPLAY_STATUS=n`; `render_status()` only feeds the display);
  - move the blocking connect and disconnect requests off the watched path;
  - give `netwq` its own, longer timeout, above the measured worst-case block.
  - Two changes: the blocking Wi-Fi status query in `render_status()` is now compiled only with `CONFIG_APP_DISPLAY_STATUS` (it only fed the display), and the connectivity queue has its own timeout, `CONFIG_APP_LIVENESS_NETWQ_TIMEOUT_S` (default 120 s), instead of the shared 30 s. Still to prove against a real AP restart (3.6 / 7.4).

  Then repeat the Deco restart (3.6, 7.4) and require zero liveness resets.
- [x] 9.16 Correct design.md D3: Espressif Wi-Fi management and status calls can hold `net_wq` for more than 30 s while an AP disappears or returns (measured 2026-09-17).
  - design.md D3 now records the measured blocking behaviour, the per-context timeouts, and the ESP32 hardware-watchdog tick quirk (~5 s, not 10 s).
