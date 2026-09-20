# Device fleet

The boards in use, what each one runs, and which device-management features
its image carries. Kept here because "does that board have remote access?"
was costing a reflash to answer.

Last verified **2026-09-20** against `tedge-dev05.preprod.c8y.io`.

Authentication is **x.509 from the Cumulocity CA** everywhere
(`CONFIG_TEDGE_AUTH_C8Y_CA`); no board uses bootstrap basic-auth.

## Where they are

| Board | Port | App | Cumulocity external ID |
|---|---|---|---|
| ESP32-C6 DevKitC | local `/dev/cu.usbmodem1101` | Modbus | `tedge-modbuse8f60afc320c` |
| ESP32-S3-DevKitC-1 (N16R8) | local `/dev/cu.usbmodem5CE60429731` | Modbus | `tedge-modbus7c0c5f5a6eb8` |
| QT Py ESP32-S3 (N4R2) | `rpi5` `/dev/ttyACM0` | Modbus | `tedge-modbusf412fa5a9424` |
| ESP32-CAM | `rpi5` `/dev/ttyUSB1` | SNMP | `tedge-snmpe465b86f97cc` |
| ESP32-WROOM-32 | `rpi5` `/dev/ttyUSB0` | OPC-UA | — |
| ESP32-WROOM-32 | `rpi5` `/dev/ttyUSB2` | remote-access enabler | `tedge-3c71bf10c2e4` |

`rpi5` is `root@rpi5-d83add9f145a.local` (192.168.68.72). Flash from there
with `/root/espenv/bin/esptool`. The Pi 4 (`rpi4-d83add90fe56.local`,
192.168.68.57) has **no boards attached** — it runs thin-edge.io itself.

## Features per board

`full.conf` = every implemented feature: telemetry, health, restart,
firmware update, shell command, log upload, crash dumps, remote access,
parameters, certificate renewal.

| Feature | C6 | S3-DevKitC | QT Py S3 | ESP32-CAM | WROOM ×2 |
|---|:--:|:--:|:--:|:--:|:--:|
| Connection + inventory | ✅ | ✅ | ✅ | ✅ | ✅ |
| Telemetry + health | ✅ | ✅ | ✅ | ✅ | ✅ |
| Restart | ✅ | ✅ | ✅ | ✅ | ✅ |
| **Firmware update** | ✅ | ✅ | ✅ | ✅ | ❌ |
| Parameters | ✅ | ✅ | ✅ | ❌ | ❌ |
| Certificate renewal | ✅ | ✅ | ✅ | ❌ | ❌ |
| Shell command | ✅ | ✅ | ✅ | ❌ | ❌ |
| Log upload | ✅ | ✅ | ✅ | ❌ | ❌ |
| Crash dumps | ✅ | ✅ | ✅ | ❌ | ❌ |
| **Remote access** | ✅ | ✅ | ✅ | ❌ | ❌ |
| DRAM used | 79.1% | 62.7% | 73.8% | **96.6%** (dram1) | **96.3%** |

The WROOM column is what it *would* take to fit, not what is flashed: even
with those three removed it links at 96.3% with 7 KB spare, which is not
worth deploying. Both WROOMs are **left on their existing images**.

### The ESP32-CAM is deliberately small

A classic ESP32 splits its RAM into dram0 (192 KB) and dram1 (96 KB), and
the net_buf pools and `.dram0.noinit` all land in dram1. Everything enabled
overflows it by 33 KB, so the set is chosen around **firmware update** —
the one that matters, because without it the board can only be changed with
a cable. That needs a second concurrent TLS session for the HTTPS download,
which is what the TLS context budget is spent on, so nothing else that
wants one is built in.

Consequences to expect in the UI: **no remote-access tab, no shell tab, and
log requests will not work** — those capabilities are genuinely absent, not
failing. An earlier image left log upload compiled in while capping TLS
contexts at 1, so it advertised a capability it could not perform; that is
what "unresponsive in the cloud" looked like.

**Certificate renewal is off**, so this board needs re-onboarding before its
certificate expires (2027-09). Firmware update works, so a later image can
trade something else for it.

Verified 2026-09-20: `c8y_Firmware` 0.2.0 → 0.2.1 through Cumulocity,
operation SUCCESSFUL, image confirmed after it reconnected.

### Firmware updates take minutes, and that is the swap

Operation delivery is **sub-second** (measured: created → SEND in 0.1 s).
The wall-clock time goes on the HTTPS download (~800 KB) and then MCUboot's
swap. `apps/*/sysbuild.conf` uses `SB_CONFIG_MCUBOOT_MODE_SWAP_SCRATCH`,
which copies the image sector by sector through a scratch area — minutes at
this image size. `SWAP_USING_MOVE` is faster and still supports rollback;
`OVERWRITE_ONLY` is fastest and gives up rollback. Do not read a slow update
as a stuck operation.

### Why the WROOMs cannot

192 KB DRAM segment and no PSRAM, against an OPC-UA server plus the client.
`full.conf` overflows by **47,560 B**. The features that need a *second*
concurrent TLS session — firmware update (HTTPS download) and remote access
(tunnel) — are what has to go, and it is still marginal.

## What decides whether a board fits

**Not flash** — that never exceeded 26%. It is internal DRAM, and
specifically whether the 96 KB mbedTLS heap can be moved into PSRAM with
`CONFIG_MBEDTLS_HEAP_CUSTOM_SECTION=y`.

| Board | PSRAM | Heap location |
|---|---|---|
| S3-DevKitC | 8 MB octal | PSRAM |
| QT Py S3 | 2 MB quad | PSRAM |
| ESP32-CAM | 4 MB quad | PSRAM |
| ESP32-C6 | none | internal (fits, ~69 KB libc heap left) |
| WROOM-32 | none | internal — does not fit |

**Trap:** `CONFIG_SPIRAM_TYPE_*` alone leaves `CONFIG_ESP_SPIRAM_SIZE=0`, so
the `ext_dram` region is empty and the link fails with "External SPIRAM
overflowed". State the size. The QT Py board file went unnoticed for months
at 99.81% DRAM — 764 bytes spare — because its PSRAM was never configured.

## Remote access / interactive sessions

Running a TUI (`htop`) over a tunnel needs more than the defaults give.
Verified on the C6 at 122×61 and the S3-DevKitC at both 122×61 and 200×50.

Each board file carrying remote access sets:

```
CONFIG_HEAP_MEM_POOL_SIZE=40960     # websocket masking k_malloc + Wi-Fi adapter
CONFIG_NET_BUF_DATA_SIZE=256        # 128 means 12 buffers per 1500-byte frame
CONFIG_NET_PKT_RX_COUNT=16
CONFIG_NET_PKT_TX_COUNT=16
CONFIG_NET_BUF_RX_COUNT=96          # ~16 full packets in flight
CONFIG_NET_BUF_TX_COUNT=64
```

Without them a session paints one screen, freezes, and the device drops off
the cloud and reconnects. The failure is **burst depth, not bandwidth** —
the same image streams 1 MB happily. See
`tedge-zephyr/docs/debugging-against-real-devices.md`.

## Known board quirks

- **ESP32-C6:** opening its USB-Serial-JTAG console **resets the board**
  (`esp_reason 11`). Attach before a test, not during. Its Wi-Fi
  association is also unreliable (`reason -1`, 2–4 attempts, watchdog
  reboots between them) — unrelated to anything above.
- **Classic ESP32 (WROOM, CAM):** hang and drop off the network under
  Zephyr 4.4.2. The S3 does not. See the project notes on SNMP/ESP32
  network issues.
- **ESP32-S2 Feather:** associates but passes no IPv4 traffic under Zephyr
  4.4.2. Not in service.

## Rebuilding one

```sh
OVR="/ws/app/tedge-zephyr/profiles/full.conf;\
/ws/app/apps/<app>/boards/<board>_tedge.conf;\
/ws/app/overlay-wifi-credentials.conf;/ws/app/tedge.local.conf"

docker exec -w /ws/app -e ZEPHYR_SDK_INSTALL_DIR=$SDK zephyr-dev \
  west build --sysbuild -b <board> apps/<app> --pristine -d build-x \
  -- "-DEXTRA_CONF_FILE=$OVR"

ESPTOOL=~/flashenv/bin/esptool ./scripts/flash.sh build-x --port <port> --app-only
```

A board moving from a plain build to MCUboot needs `--erase-all` once,
which also erases its credentials. A device with no certificate comes up in
`awaiting-registration` and logs a registration URL; register it with:

```sh
c8y deviceregistration register-ca --id <external-id> --one-time-password <otp>
```

If onboarding misbehaves, delete the stale device user first:
`c8y users delete --id device_<external-id>`.
