## 1. Connectivity watchdog + robust reconnect (lib/common/net.c)

- [x] 1.1 Add a `connectivity_watchdog()` hook called from `status_work_handler` (above the NSOS/Wi-Fi split); real logic in the Wi-Fi branch, no-op on native_sim
- [x] 1.2 Watchdog: compute `bad = !app_net_is_connected() || read_iface_ipv4()==0`; track consecutive bad ticks; reset on a good tick
- [x] 1.3 After `N_TRIGGER` bad ticks (~2), `k_work_reschedule(&reconnect_work,...)` to force recovery; treat "connected but IPv4 lost" as disconnected (`mark_connected(false)`)
- [x] 1.4 Make `reconnect_handler`/`wifi_connect` robust: request `NET_REQUEST_WIFI_DISCONNECT` (or check state) before re-issuing connect; reschedule `reconnect_work` on any `wifi_connect()` error instead of discarding it

## 2. Last-resort self-reboot

- [x] 2.1 Kconfig: `APP_NET_RECONNECT_REBOOT` (bool, default y, `select REBOOT`) and `APP_NET_REBOOT_TIMEOUT_S` (int, default 300)
- [x] 2.2 In the watchdog, after continuous-offline ticks exceed the timeout, `sys_reboot(SYS_REBOOT_COLD)` when enabled; log the reason; reset counters on recovery

## 3. Status LED indicator

- [x] 3.1 Kconfig `APP_STATUS_LED` (default y if `$(dt_alias_enabled,led0)`, else n); add a `lib/common/status_led.[ch]` module compiled under `APP_STATUS_LED`
- [x] 3.2 Drive `DT_ALIAS(led0)` via `gpio_dt_spec` from a fast timer/work item: blink while not connected (no IPv4 / (re)connecting), steady on when connected+serving; no-op if unavailable
- [x] 3.3 Hook LED state to connectivity transitions (`mark_connected`) and the current display stage; ensure it's a no-op on native_sim
- [x] 3.4 Add a `led0` alias in `apps/*/boards/esp32_devkitc_esp32_procpu.conf`/overlay if the board lacks one (WROOM onboard LED, typically GPIO2); note S3 NeoPixel is out of scope

## 4. Verification (device side, on a WROOM)

- [x] 4.1 Build native_sim (watchdog no-op) + WROOM for both apps; confirm clean build
- [ ] 4.2 Steady state: connected, LED steady; OPC-UA browse/read normal; run the 300-cycle churn — unaffected (no spurious reconnects/reboots)
- [ ] 4.3 Force AP down (or deauth): confirm LED blinks, device auto-recovers within seconds of the AP returning — no manual reboot
- [ ] 4.4 Force associated-but-no-IP / DHCP loss: confirm the watchdog forces a reconnect and the device regains an IP
- [ ] 4.5 Prolonged outage: with a short test `APP_NET_REBOOT_TIMEOUT_S`, confirm the last-resort reboot fires and the device comes back; confirm disabling it keeps retrying without reboot

## 5. Documentation

- [x] 5.1 README: connectivity resilience (watchdog, self-reboot Kconfig) and the status LED (meaning of blink vs steady, `led0` alias, per-board notes); commit
