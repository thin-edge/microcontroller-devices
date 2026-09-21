# Device fleet

The boards in use, what each one runs, and which device-management features
its image carries. Kept here because "does that board have remote access?"
was costing a reflash to answer.

Last verified **2026-09-20** against `tedge-dev05.preprod.c8y.io`.

Authentication is **x.509 from the Cumulocity CA** everywhere
(`CONFIG_TEDGE_AUTH_C8Y_CA`); no board uses bootstrap basic-auth.

All three protocol applications — `modbus-server`, `snmp-agent` and
`opcua-server` — can carry the client: each has a `src/tedge_glue.c` built
when `CONFIG_TEDGE=y`; `apps/tedge-agent` is the client with no protocol.
Whether a given *board* can is a separate question, answered below.

**What is released** for each board is [`release/devices.yml`](release/devices.yml);
every `tedge-*` build listed there points at the entry below that records
its run on the board.

## Where they are

| Board | Port | App | Cumulocity external ID |
|---|---|---|---|
| ESP32-C6 DevKitC | local `/dev/cu.usbmodem1101` | Modbus | `tedge-modbuse8f60afc320c` |
| ESP32-S3-DevKitC-1 (N16R8) | local `/dev/cu.usbmodem5CE60429731` | Modbus | `tedge-modbus7c0c5f5a6eb8` |
| QT Py ESP32-S3 (N4R2) | `rpi5` `/dev/ttyACM0` | Modbus | `tedge-modbusf412fa5a9424` |
| ESP32-CAM | `rpi5` `/dev/ttyUSB1` | SNMP | `tedge-snmpe465b86f97cc` |
| ESP32-WROOM-32 | `rpi5` `/dev/ttyUSB0` | OPC-UA | — |
| ESP32-WROOM-32 | `rpi5` `/dev/ttyUSB2` | SNMP, no client (Wi-Fi from ZTP); the WROOM capacity test board | — |

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

### What a WROOM can carry (measured 2026-09-21)

192 KB dram0 and no PSRAM. The client fits beside **Modbus** or with **no
protocol** (`apps/tedge-agent`), at the ota level plus remote access; not
beside SNMP or OPC-UA at any level. Measured on the rpi5 WROOM
(3c:71:bf:10:c2:e4) with `lib/common/tedge-boards/esp32-devkitc.conf`:
8 KB TLS records, a 56 KB mbedTLS heap in internal RAM, 10 network
connections. Each image was flashed `--app-only`, run against
thin-edge-io.eu-latest and the board restored from a full flash backup
afterwards.

| App | Build | dram0 | Result |
|---|---|---|---|
| modbus-server | ota | 96.3% | enrolled; OTA 800 KB in 32 s, confirmed; 23 KB system heap free |
| modbus-server | ota + remote access | 99.9% | OTA confirmed; SSH tunnel 409 KB in 19 s with 86/86 Modbus reads alongside; min 21.6 KB heap; 76 B `malloc` arena |
| modbus-server | + certificate renewal | no room at 10 connections | — |
| modbus-server | full | over by 8.4 KB | — |
| tedge-agent | ota + remote access + certificate renewal | 99.7% | enrolled (CA registration); restart; SSH tunnel 405 KB in 21 s; OTA 0.4.0 → 0.4.1 confirmed; `main` used 1.2 KB of its 4 KB stack |
| tedge-agent | + parameters | over by 440 B | — |
| tedge-agent | full | over by 3.7 KB | — |
| snmp-agent | ota / + remote access / full | over by 24 / 27 / 40 KB | — |
| opcua-server | ota / + remote access / full | over by 16 / 19 / 30 KB | — |

What only a run showed: at the WROOM application default of 6 network
connections the image links, but a tunnel request fails ("Not enough
connection contexts", operation "the cloud connection failed (-2)"); the
board file raises it to 10. HTTPS firmware download and the tunnel both work
with 8 KB TLS records.

SNMP carries ~33 KB of static tables and buffers (MIB leaves 13.5 KB,
varbinds 6.7 KB, request buffers 8.5 KB) and open62541 more; either would have
to shrink by 16–24 KB before the client fits beside it.

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
OVR="/ws/app/tedge-zephyr/profiles/<full|ota>.conf;\
/ws/app/lib/common/tedge-boards/<device>.conf;\
/ws/app/overlay-wifi-credentials.conf;/ws/app/tedge.local.conf"

docker exec -w /ws/app -e ZEPHYR_SDK_INSTALL_DIR=$SDK zephyr-dev \
  west build --sysbuild -b <board> apps/<app> --pristine -d build-x \
  -- "-DEXTRA_CONF_FILE=$OVR"

ESPTOOL=~/flashenv/bin/esptool ./scripts/flash.sh build-x --port <port> --app-only
```

The profile picks the features (`tedge-zephyr/profiles/`); the board file
(`lib/common/tedge-boards/`, one per device, whatever the app) sizes the TLS
heap, PSRAM, connections and network buffers, and goes after the profile so
its sizes win. The ESP32-CAM also needs
`-DEXTRA_DTC_OVERLAY_FILE=/ws/app/lib/common/dts/esp32cam-status-led.overlay`
(and the same with the `wifi-provisioner_` prefix).

A board moving from a plain build to MCUboot needs `--erase-all` once,
which also erases its credentials. A device with no certificate comes up in
`awaiting-registration` and logs a registration URL; register it with:

```sh
c8y deviceregistration register-ca --id <external-id> --one-time-password <otp>
```

If onboarding misbehaves, delete the stale device user first:
`c8y users delete --id device_<external-id>`.

A device provisioned through **lab-ztp-provisioner** (`overlay-ztp.conf`)
prints no registration URL: the ZTP server registered it before it booted the
application, and it enrols on its first connection. It logs `Cumulocity tenant
from ZTP: …` and `Enrolling as "<external-id>" with the one-time password from
ZTP` at boot. Its external ID is the one in the bundle (e.g. `zephyr-<host>`
with the profile's prefix), not the hostname. If it does not enrol, check that
the provisioner left its data — the application logs `ZTP one-time password
not accepted (…)` and keeps it for the next boot when the client refuses it —
and that the server's token has not expired before the device first came
online.
