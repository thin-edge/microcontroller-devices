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
| ESP32-CAM | `rpi5` `/dev/ttyUSB1` | SNMP (ota + parameters, certificate renewal, log upload; traps to tedge-dot) | `tedge-snmpe465b86f97cc` |
| ESP32-WROOM-32 | `rpi5` `/dev/ttyUSB0` | OPC-UA | — |
| ESP32-WROOM-32 | `rpi5` `/dev/ttyUSB2` | Modbus (ota + parameters); the WROOM capacity test board | `tedge-modbus3c71bf10c2e4` |

`rpi5` is `root@rpi5-d83add9f145a.local` (192.168.68.72). Flash from there
with `/root/espenv/bin/esptool`. The Pi 4 (`rpi4-d83add90fe56.local`,
192.168.68.57) has **no boards attached** — it runs thin-edge.io itself.

## Features per board

`full.conf` = every implemented feature: telemetry, health, restart,
firmware update, shell command, log upload, crash dumps, remote access,
parameters, certificate renewal.

| Feature | C6 | S3-DevKitC | QT Py S3 | ESP32-CAM (SNMP) | WROOM (Modbus) |
|---|:--:|:--:|:--:|:--:|:--:|
| Connection + inventory | ✅ | ✅ | ✅ | ✅ | ✅ |
| Telemetry + health | ✅ | ✅ | ✅ | ✅ | ✅ |
| Restart | ✅ | ✅ | ✅ | ✅ | ✅ |
| **Firmware update** | ✅ | ✅ | ✅ | ✅ | ✅ |
| Parameters | ✅ | ✅ | ✅ | ✅ | ✅ |
| Certificate renewal | ✅ | ✅ | ✅ | ✅ | ✅ |
| Shell command | ✅ | ✅ | ✅ | ❌ | ❌ |
| Log upload | ✅ | ✅ | ✅ | ✅ | ❌ |
| Crash dumps | ✅ | ✅ | ✅ | ❌ | ❌ |
| **Remote access** | ✅ | ✅ | ✅ | ❌ | ❌ |
| RAM (tightest region) | 79.1% | 62.7% | 73.8% | 84.7% (dram1) | 92.2% (dram0) |

The ESP32-CAM and WROOM columns are their release builds as of 2026-09-23,
each run on the board (below). Remote access is left out on both on purpose: it links, but the
receive buffers they have room for are too shallow for an interactive
tunnel.

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

### WROOM and ESP32-CAM release builds (measured 2026-09-23)

After the footprint work (`reduce-memory-footprint`). Both boards on the
rpi5, provisioned fresh over ZTP (`overlay-ztp.conf`), thin-edge-io.eu-latest.

| Board | Build | RAM | Run |
|---|---|---|---|
| WROOM-32 (3c:71:bf:10:c2:e4) | modbus-server ota + parameters | dram0 91.7%, dram1 82.9%, 16.3 KB `malloc` arena | enrolled; telemetry; OTA `ctl1` → `ctl2` → `ctl3`, each confirmed; the pump run from `zephyr_modbus_control` (running, manual 60 %, flow and rpm follow over Modbus); Modbus write allowed, then refused with exception 2 once `local_writes` is off; an out-of-range setpoint refused by the operation; the state kept across a restart; `zephyr_tedge` change (whole set) accepted |
| ESP32-CAM (e4:65:b8:6f:97:cc) | modbus-server ota + parameters | dram0 63.3%, dram1 88.3%, 72.2 KB arena | enrolled as `tedge-modbuse465b86f97cc`; pump run from the cloud at 45 %, a locked Modbus write refused, 29/29 reads |
| ESP32-CAM | snmp-agent ota + parameters + certificate renewal + log upload | dram0 77.8%, dram1 84.7%, 43.7 KB arena | enrolled as `tedge-snmpe465b86f97cc`; OTA `cam2` → `cam3` → `cam4`, confirmed; full snmpwalk (66 objects); a `tedge-log` request uploaded; `zephyr_snmp_telemetry` and `zephyr_tedge` changes accepted; certificate renewal armed (364 days); client heap 16,092 of 16,384 B free after connecting |

**One feature set per board.** The release manifest gives every tedge
build on a board the same extras: the WROOM gets `ota` + parameters +
certificate renewal, the ESP32-CAM `ota` + parameters + certificate renewal
+ log upload. Each release build was then run on the rpi5 boards exactly as
the manifest builds it (version suffix `rt1`/`rt2`), chained over the air on
one enrolment per board: WROOM Modbus → agent → Modbus, CAM Modbus → agent
→ SNMP. Every step is a firmware update that confirmed (73-91 s).

| Build (release extras) | dram0 | dram1 | `malloc` arena | Board run (2026-09-23) |
|---|---|---|---|---|
| WROOM modbus-server | 92.2% | 82.9% | 15.3 KB | ✅ OTA confirmed (twice); telemetry; `zephyr_tedge` change (whole set); certificate renewal armed; pump run from `zephyr_modbus_control` at 55 %, locked Modbus write refused (exception 2), 29/29 reads alongside; restart |
| WROOM tedge-agent | 91.5% | 79.7% | 16.8 KB | ✅ OTA confirmed; `c8y_Device` telemetry every 60 s; `zephyr_tedge` change; certificate renewal armed; restart |
| ESP32-CAM snmp-agent | 77.8% | 84.7% | 43.7 KB | ✅ OTA confirmed; full snmpwalk before and after a restart; `zephyr_tedge` change; certificate renewal armed; `tedge-log` upload (20 lines) |
| ESP32-CAM modbus-server | 73.7% | 80.5% | 51.7 KB | ✅ OTA confirmed; telemetry; `zephyr_tedge` change; certificate renewal armed; pump control and locked write as on the WROOM, 29/29 reads; `tedge-log` upload (26 lines; `app-status` offered too); restart |
| ESP32-CAM tedge-agent | 72.9% | 77.4% | 53.2 KB | ✅ OTA confirmed; `c8y_Device` telemetry; `zephyr_tedge` change; certificate renewal armed; `tedge-log` upload (22 lines); restart |

On the CAM the device keeps the identity ZTP gave it (`tedge-snmpe465b86f97cc`)
while it runs the other applications; each application reports its own
firmware name and type.

What made these fit: the client's private heap moved from `.noinit`
(dram1 on a classic ESP32) to `.bss` (dram0) on the ESP32-CAM, whose
mbedTLS heap is in PSRAM; the WROOM keeps it in `.noinit`
(`CONFIG_TEDGE_HEAP_NOINIT`) because its dram0 holds the mbedTLS heap. Log
upload's 8 KB stack then fits the CAM's dram1. On the CAM the full profile
misses dram1 by 5.7 KB (33 KB before the footprint work); SNMP with log
upload and the heap still in dram1 missed by 1.3 KB.

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

**Remote access is not released for the WROOM (2026-09-23).** After the
footprint work (`reduce-memory-footprint`) the rpi5 WROOM was tried again,
provisioned over ZTP, as `tedge-modbus3c71bf10c2e4`:

| Build | dram0 / dram1 | Result |
|---|---|---|
| modbus-server full (client heap cut to 15 KB to link) | 96.7% / 99.3%, 6.4 KB `malloc` arena | enrolled; telemetry; log upload; OTA `mf1` → `mf2` 819 KB in 30 s, confirmed. Tunnel: a 400 KB SSH copy went through, but the first session broke after 19 s ("sending to the cloud failed"), 117 × `esp32_wifi: Failed to allocate net buffer`, MQTT reconnected once, 7 of 75 Modbus reads failed alongside |
| modbus-server ota + remote access (the release build until then) | 94.0% / 85.2%, 11.8 KB arena | connected; `htop` over the tunnel failed, worse than the full build |

Both run with 32 receive buffers of 128 B, about three Wi-Fi frames in
flight. An interactive tunnel needs about sixteen (96 × 256 B on the C6,
see [Remote access / interactive sessions](#remote-access--interactive-sessions)):
linked with those buffers, the full build misses `dram1` by 28,552 B and
the ota + remote access build by 14,720 B. So the WROOM releases ship
**ota**, plus certificate renewal on the agent, and no remote access;
firmware update stays, because it is how these boards get new images.

SNMP carries ~33 KB of static tables and buffers (MIB leaves 13.5 KB,
varbinds 6.7 KB, request buffers 8.5 KB) and open62541 more; either would have
to shrink by 16–24 KB before the client fits beside it.

### Release builds measured on the boards (2026-09-21)

Every `tedge-*` build in `release/devices.yml` points here or at the WROOM
table above. Each ran on the board, installed **over the air** from the
previous one (so each image's HTTPS download and the next one's
boot-and-confirm are both exercised), then served its protocol to a client
for 10 minutes; builds with remote access also carried an SSH session
(400 KB) through a tunnel with the protocol read alongside. C6, S3-DevKitC
and QT Py were provisioned through lab-ztp-provisioner with the release
image; the CAM was registered through the Cumulocity CA. RAM is the fullest
internal region (`scripts/release/size.py`).

| Board | Build | RAM | Result |
|---|---|---|---|
| ESP32-C6 | modbus full | 91.9% | ✅ ZTP → enrolled, reports `0.4.0-rc1`; tunnel 405 KB/14 s with Modbus alongside |
| ESP32-C6 | modbus ota | 86.0% | ✅ 571/571 reads |
| ESP32-C6 | snmp full | 99.2% | ✅ 567/567; tunnel 405 KB/10 s, 38/38 alongside |
| ESP32-C6 | snmp ota | 93.3% | ✅ 567/567 |
| ESP32-C6 | tedge-agent full | 90.1% | ✅ tunnel 405 KB/10 s |
| ESP32-C6 | tedge-agent ota | 84.2% | ✅ |
| ESP32-C6 | opcua full | 99.1% | ❌ `UA_Server_newWithConfig() failed` (no memory) |
| ESP32-C6 | opcua ota | 93.2% | ❌ server starts, every session `BadOutOfMemory` |
| S3-DevKitC | modbus full | — | ✅ ZTP → enrolled; tunnel 405 KB/12 s with Modbus alongside |
| S3-DevKitC | modbus ota | 81.9% | ✅ 573/573 |
| S3-DevKitC | snmp full | 98.8% | ✅ 570/570; tunnel 405 KB/8 s, 38/38 alongside |
| S3-DevKitC | snmp ota | 91.2% | ✅ 568/568 |
| S3-DevKitC | tedge-agent full | 87.2% | ✅ tunnel 405 KB/8 s |
| S3-DevKitC | tedge-agent ota | 79.7% | ✅ |
| S3-DevKitC | opcua full | 98.7% | ❌ server did not start; the app refused the update and it rolled back |
| S3-DevKitC | opcua ota | 91.1% | ❌ 0/561 reads (out of memory per session, as on the C6) |
| QT Py S3 | modbus full | 88.5% | ✅ ZTP → enrolled |
| QT Py S3 | modbus ota | 81.0% | ✅ serving (the Pi's tedge-dot holds its one Modbus connection) |
| QT Py S3 | opcua ota | 90.2% | ✅ 490/490 |
| QT Py S3 | snmp full | 97.8% | ✅ 569/569; tunnel 405 KB/12 s, 34/35 alongside |
| QT Py S3 | snmp ota | 90.3% | ✅ 562/562 |
| QT Py S3 | tedge-agent full | 86.2% | ✅ tunnel 405 KB/9 s (a first attempt moved nothing; the retest passed) |
| QT Py S3 | tedge-agent ota | 78.7% | ✅ |
| QT Py S3 | opcua full | 97.7% | ❌ server did not start; refused and rolled back |
| ESP32-CAM | modbus ota | dram1 92.5% | ✅ 567/567 |
| ESP32-CAM | tedge-agent ota | dram1 85.2% | ✅ installed over the air from Modbus |
| ESP32-CAM | snmp ota | dram1 96.6% | ✅ in service since 2026-09-20 (above) |
| ESP32-CAM | every full, opcua ota | — | ❌ do not link (dram1 over by 2.6–23 KB) |

OPC-UA is the one protocol that does not live beside the client on these
boards: open62541 allocates from the libc heap, which the client's TLS and
network buffers leave too small — at `full` it cannot create the server, at
`ota` it cannot open sessions on the C6 and the S3-DevKitC. The QT Py's `ota`
image served, but with the margin that close, the S3-DevKitC result is the
one to believe for a new board. OPC-UA ships `standalone` on every board
except the QT Py's `tedge-ota`.

What fits on top of the CAM's `tedge-ota` (link only, not yet run):
certificate renewal and parameters beside SNMP or Modbus; remote access and
log upload do not (over by 1–5.4 KB). The agent has room for all four.

Telemetry was published throughout, but the test tenant had no mapping for
`te/.../m/...`, so measurements could not be seen in Cumulocity; the
`remoteAccess` twin mapping did work.

### Shell diagnostics measured (2026-09-22)

`lib/common/tedge-boards/extras/shell-diagnostics.conf` adds the cloud shell
command (allow-list: `kernel uptime`, `kernel version`, `net iface`,
`net conn`, `wifi status`, `tedge params list`, `tedge diag`; `help` lists
them). It costs about 16 KB of internal RAM.

| Board | Build | RAM | Result |
|---|---|---|---|
| S3-DevKitC | modbus full + shell | 93.6% | ✅ OTA in; 570/570 reads over 10 min; tunnel 405 KB/11 s with 36/36 reads alongside; every command answers; a command off the list is refused; the next OTA download from it succeeds |
| S3-DevKitC | tedge-agent full + shell | 91.1% | ✅ OTA in; tunnel 405 KB/7 s; every command answers; OTA out of it succeeds |
| ESP32-C6 | modbus full + shell | 95.3% | ⚠️ passes the same checks, but logs `esp32c6_wifi_adapter: memory allocation failed` at boot |
| ESP32-C6 | tedge-agent full + shell | 93.3% | ❌ after ~30 min up (a tunnel and the commands had run), three firmware downloads in a row failed (`download failed (-5)`); fine again after a reboot. A device in that state needs a cable to change image |
| any | snmp full + shell | — | ❌ does not link (over by 7–13 KB) |
| QT Py S3 | modbus full + shell | 92.6% | ✅ ZTP → enrolled; clean boot (no Wi-Fi allocation failure); every command answers, one off the list refused; OTA out to the agent and back in; tunnel 405 KB/9 s. **Long soak (downloads after 30+ min up) pending**, after the release |
| QT Py S3 | tedge-agent full + shell | 90.2% | ✅ OTA in from Modbus + shell, confirmed; OTA out back to Modbus succeeds. Long soak pending, as above |

So the Modbus and agent `tedge-full` images of the S3-DevKitC and the QT Py
carry it. On the C6, with no PSRAM, the shell's RAM takes the Wi-Fi driver
and the second TLS session past their margin. The QT Py's run was short; the
C6's failure only showed after half an hour up, so a longer soak on the QT Py
follows the first release.

Test note: a TCP probe of a local `c8y remoteaccess server` port opens a
tunnel of its own, which holds the device's single session for a moment; an
SSH connection straight after it can find the slot busy.

### Static sizes after the footprint work, link only (2026-09-22)

Every `release/devices.yml` build, rebuilt from the `reduce-memory-footprint`
working tree against the `v0.5.0` baseline (`release/size-baseline.json`
before and after; `scripts/release/size.py`). These are link results, not
board runs: the tier-1 items (AES tables in flash, const open62541 type
tables, the compact SNMP MIB, a 4 KB `main` stack, the tedge heap default,
no POSIX layer, the no-op system-heap lines gone) plus the split TLS output
buffer, the TLS server role off, open62541 without description strings and
the provisioner's leaner log path. "RAM" is the fullest internal region;
"arena" is what is left for libc `malloc`, which open62541 and the shell
draw on. Images grow by about 2.7 KB where the AES tables moved to flash
and by 13-16 KB on OPC-UA, where the 21 KB of type tables left RAM; an ESP
image pads its RAM-loaded segments to a 64 KB boundary, so the file moves in
steps (the S3 Modbus `tedge-ota` images crossed one downwards, -62.7 KB).

| Build | Tightest RAM region | RAM before | RAM after | Δ RAM | Arena before | Arena after | Image before | Image after | Δ image |
|---|---|---|---|---|---|---|---|---|---|
| `modbus-server-standalone-esp32-devkitc` | dram0_0_seg | 99,812 | 97,152 | -2,660 | 96,792 | 99,440 | 590,471 | 590,167 | -304 |
| `modbus-server-standalone-esp32c6-devkitc` | sram0_0_seg | 213,496 | 205,780 | -7,716 | 295,936 | 303,664 | 733,963 | 733,803 | -160 |
| `modbus-server-standalone-esp32s3-devkitc` | dram0_0_seg | 203,128 | 196,104 | -7,024 | 195,964 | 202,996 | 583,946 | 583,787 | -159 |
| `modbus-server-standalone-qtpy-esp32s3` | dram0_0_seg | 200,520 | 193,500 | -7,020 | 198,564 | 205,596 | 584,090 | 583,931 | -159 |
| `modbus-server-tedge-full-esp32c6-devkitc` | sram0_0_seg | 449,140 | 432,368 | -16,772 | 60,304 | 77,072 | 959,146 | 961,947 | +2,801 |
| `modbus-server-tedge-full-esp32s3-devkitc` | dram0_0_seg | 359,492 | 342,848 | -16,644 | 39,604 | 56,252 | 931,498 | 934,203 | +2,705 |
| `modbus-server-tedge-full-qtpy-esp32s3` | dram0_0_seg | 355,868 | 339,244 | -16,624 | 43,220 | 59,852 | 931,691 | 934,396 | +2,705 |
| `modbus-server-tedge-ota-esp32-cam` | dram0_0_seg | 134,992 | 123,348 | -11,644 | 61,600 | 73,248 | 800,054 | 795,080 | -4,974 |
| `modbus-server-tedge-ota-esp32-devkitc` | dram0_0_seg | 196,460 | 184,796 | -11,664 | 136 | 11,800 | 811,687 | 806,616 | -5,071 |
| `modbus-server-tedge-ota-esp32c6-devkitc` | sram0_0_seg | 420,412 | 397,560 | -22,852 | 89,024 | 111,872 | 887,482 | 890,298 | +2,816 |
| `modbus-server-tedge-ota-esp32s3-devkitc` | dram0_0_seg | 314,772 | 292,616 | -22,156 | 84,324 | 106,476 | 801,771 | 739,035 | -62,736 |
| `modbus-server-tedge-ota-qtpy-esp32s3` | dram0_0_seg | 311,156 | 289,004 | -22,152 | 87,932 | 110,084 | 801,914 | 739,178 | -62,736 |
| `opcua-server-standalone-esp32-devkitc` | dram0_0_seg | 123,176 | 101,492 | -21,684 | 73,424 | 95,104 | 750,454 | 750,008 | -446 |
| `opcua-server-standalone-esp32c6-devkitc` | sram0_0_seg | 255,620 | 229,836 | -25,784 | 253,808 | 279,600 | 840,523 | 854,138 | +13,615 |
| `opcua-server-standalone-esp32s3-devkitc` | dram0_0_seg | 245,244 | 219,464 | -25,780 | 153,844 | 179,620 | 754,635 | 768,651 | +14,016 |
| `opcua-server-standalone-qtpy-esp32s3` | dram0_0_seg | 242,636 | 216,856 | -25,780 | 156,444 | 182,236 | 754,779 | 768,794 | +14,015 |
| `opcua-server-tedge-ota-qtpy-esp32s3` | dram0_0_seg | 346,572 | 305,896 | -40,676 | 52,524 | 93,196 | 906,714 | 923,196 | +16,482 |
| `snmp-agent-standalone-esp32-devkitc` | dram0_0_seg | 131,912 | 105,600 | -26,312 | 64,680 | 90,992 | 578,935 | 578,757 | -178 |
| `snmp-agent-standalone-esp32c6-devkitc` | sram0_0_seg | 243,712 | 212,344 | -31,368 | 265,728 | 297,088 | 732,220 | 731,995 | -225 |
| `snmp-agent-standalone-esp32s3-devkitc` | dram0_0_seg | 233,356 | 202,688 | -30,668 | 165,732 | 196,404 | 582,635 | 582,410 | -225 |
| `snmp-agent-standalone-qtpy-esp32s3` | dram0_0_seg | 230,748 | 200,084 | -30,664 | 168,348 | 199,004 | 582,764 | 582,555 | -209 |
| `snmp-agent-tedge-full-esp32c6-devkitc` | sram0_0_seg | 485,068 | 444,648 | -40,420 | 24,368 | 64,784 | 957,675 | 960,395 | +2,720 |
| `snmp-agent-tedge-full-esp32s3-devkitc` | dram0_0_seg | 379,472 | 339,764 | -39,708 | 19,628 | 59,324 | 806,491 | 809,210 | +2,719 |
| `snmp-agent-tedge-full-qtpy-esp32s3` | dram0_0_seg | 375,848 | 336,152 | -39,696 | 23,244 | 62,932 | 806,635 | 809,355 | +2,720 |
| `snmp-agent-tedge-ota-esp32-cam` | dram0_0_seg | 166,720 | 131,420 | -35,300 | 29,872 | 65,176 | 795,527 | 790,712 | -4,815 |
| `snmp-agent-tedge-ota-esp32c6-devkitc` | sram0_0_seg | 456,380 | 409,864 | -46,516 | 53,056 | 99,568 | 886,283 | 889,035 | +2,752 |
| `snmp-agent-tedge-ota-esp32s3-devkitc` | dram0_0_seg | 350,596 | 304,784 | -45,812 | 48,500 | 94,308 | 735,435 | 738,155 | +2,720 |
| `snmp-agent-tedge-ota-qtpy-esp32s3` | dram0_0_seg | 346,972 | 301,172 | -45,800 | 52,116 | 97,916 | 735,580 | 738,298 | +2,718 |
| `tedge-agent-tedge-full-esp32c6-devkitc` | sram0_0_seg | 440,340 | 427,688 | -12,652 | 69,104 | 81,744 | 890,284 | 893,083 | +2,799 |
| `tedge-agent-tedge-full-esp32s3-devkitc` | dram0_0_seg | 350,200 | 337,668 | -12,532 | 48,884 | 61,420 | 928,363 | 931,066 | +2,703 |
| `tedge-agent-tedge-full-qtpy-esp32s3` | dram0_0_seg | 346,576 | 334,064 | -12,512 | 52,516 | 65,036 | 928,555 | 931,258 | +2,703 |
| `tedge-agent-tedge-ota-esp32-cam` | dram0_0_seg | 133,516 | 121,888 | -11,628 | 63,088 | 74,704 | 785,814 | 780,887 | -4,927 |
| `tedge-agent-tedge-ota-esp32-devkitc` | dram0_0_seg | 196,024 | 184,384 | -11,640 | 568 | 12,208 | 799,334 | 794,486 | -4,848 |
| `tedge-agent-tedge-ota-esp32c6-devkitc` | sram0_0_seg | 411,676 | 392,896 | -18,780 | 97,760 | 116,544 | 884,635 | 887,436 | +2,801 |
| `tedge-agent-tedge-ota-esp32s3-devkitc` | dram0_0_seg | 306,128 | 288,084 | -18,044 | 92,964 | 111,004 | 733,900 | 736,684 | +2,784 |
| `tedge-agent-tedge-ota-qtpy-esp32s3` | dram0_0_seg | 302,512 | 284,472 | -18,040 | 96,588 | 114,612 | 734,043 | 736,826 | +2,783 |

Total over 36 builds: RAM -832,724 B, image -50,095 B
C6 ZTP provisioner: 1,015,243 -> 948,348 B (96.8 % -> 90.4 % of prov)

The C6 ZTP provisioner went from 1,015,243 to 948,348 B (96.8 % to 90.4 % of
its 1 MB `prov` partition). Its flash-mapped code now ends 32 B before a
64 KB boundary; the release size gate (92 % budget) is what catches a
regression there.

## What decides whether a board fits

**Not flash** — that never exceeded 26%. It is internal DRAM, and
specifically whether the 96 KB mbedTLS heap can be moved into PSRAM with
`CONFIG_MBEDTLS_HEAP_CUSTOM_SECTION=y`.

Both are now gated: `scripts/release/size.py` compares every release build's
RAM regions, malloc arena and image against `release/size-baseline.json`
and fails a pull request that grows past it (README, "Size gate and
baseline"). Flash has a budget of its own per partition, 80 % of `slot0`
for an application and 92 % of `prov` for the provisioner.

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
