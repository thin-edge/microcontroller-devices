# OPC-UA Server Firmware (Zephyr RTOS)

Phase 1 firmware that turns a Wi-Fi microcontroller into an **OPC-UA server**,
so a separate collector/gateway can read its data over OPC-UA. Built on
[Zephyr RTOS](https://www.zephyrproject.org/) with the
[open62541](https://www.open62541.org/) OPC-UA stack.

See [`SCOPE.md`](SCOPE.md) for scope/phasing, and
[`openspec/changes/opcua-server-firmware/`](openspec/changes/opcua-server-firmware/)
for the proposal, design, specs and task breakdown/status.

## Status

**Verified end-to-end on real ESP32-WROOM-32 hardware:** the board joins Wi-Fi,
runs the OPC-UA server, advertises over mDNS, and a standard OPC-UA client on
the LAN discovers it as `tedge-opcua.local`, browses the Device object, and
reads live, updating measurements (temperature/humidity/pressure).

## Targets

| Board | Zephyr board target | Role | Status |
|-------|---------------------|------|--------|
| ESP32-WROOM-32 | `esp32_devkitc/esp32/procpu` | co-primary | **verified on hardware** |
| Adafruit QT Py ESP32-S3 | `adafruit_qt_py_esp32s3/esp32s3/procpu` | co-primary | **verified on hardware** |
| Feather ESP32-S2 TFT | `adafruit_feather_esp32s2_tft/esp32s2` | co-primary | builds & flashes; **Wi-Fi data path broken upstream — see note** |
| Raspberry Pi Pico W | `rpi_pico/rp2040/w` | stretch | config authored, not yet built |
| Host simulation | `native_sim/native/64` | dev / CI | builds & runs (see NSOS note) |

All hardware targets are Wi-Fi-only (station mode).

> **ESP32-S2 Feather Wi-Fi note:** the same firmware that works end-to-end on
> the WROOM builds and flashes on the S2, and the S2 *associates* (correct SSID,
> RSSI, and it obtains a DHCP lease), but its **IP data path does not pass
> traffic** under Zephyr 4.4.2: no default gateway is installed, ICMP echo
> requests send but never get a reply, and the network path hangs on the first
> transmit. Systematically ruled out as causes: DHCP-vs-static IP, MAC
> block/override, Wi-Fi power-save, router L2 isolation, Kconfig (`WIFI_ESP32` /
> `NET_L2_WIFI_MGMT` / `NET_L2_ETHERNET` are correct and identical to the working
> WROOM), and RAM starvation (server disabled + enlarged Wi-Fi buffers still
> hang). Diagnose in the field with `CONFIG_APP_PING_TARGET` + the on-display
> `GWPING` line. This is single-core ESP32-S2 Wi-Fi immaturity upstream, not
> firmware config. Paths forward if the S2 is needed: try a newer Zephyr /
> `hal_espressif`, or use ESP-IDF for the S2, or track it via a Zephyr issue.
> **Use the WROOM for real deployments.**

Built and verified with **Zephyr v4.4.2** and **Zephyr SDK 1.0.1** (as shipped
in the `zephyrprojectrtos/zephyr-build` image).

> **native_sim note:** `native_sim` uses host-offloaded sockets (NSOS). The
> firmware builds and the OPC-UA server starts, but the OPC-UA handshake cannot
> complete under NSOS because its `select()` does not report readability on
> accepted sockets. Use real hardware for end-to-end OPC-UA testing.

## Prerequisites (macOS)

The dev machine is **macOS**. `native_sim` is a *Linux* binary and the Zephyr
SDK cross-toolchains are large, so builds run inside the official Zephyr build
container. Flashing runs on the host (Docker on macOS can't reach USB serial).

```sh
brew install --cask docker            # or: brew install colima docker && colima start
python3 -m venv ~/flashenv && ~/flashenv/bin/pip install esptool pyserial asyncua
```

### Linux differences

On Linux you can install Zephyr natively (`pip install west` + Zephyr SDK) and
run `native_sim` directly. Serial devices are `/dev/ttyUSB*` / `/dev/ttyACM*`
(vs. `/dev/cu.usbserial-*` on macOS), and your user must be in the `dialout`
group to flash.

## First-time workspace setup

This repo is a Zephyr *workspace application* (T2 topology): it is the west
manifest repo, and `west update` fetches Zephyr + modules + open62541.

```sh
# Persistent build container with this repo mounted at /ws/app.
docker run -dit --name zephyr-dev -u root \
  -v "$PWD":/ws/app -w /ws/app \
  zephyrprojectrtos/zephyr-build:latest sleep infinity

export SDK=/opt/toolchains/zephyr-sdk-1.0.1   # adjust if the image differs

# Initialise + populate the workspace (once; several minutes).
docker exec -w /ws/app zephyr-dev west init -l .
docker exec -w /ws     zephyr-dev west update
docker exec -w /ws     zephyr-dev west zephyr-export

# ESP32 Wi-Fi needs Espressif binary blobs, and mbedtls (pulled in by Wi-Fi)
# needs its PSA-crypto submodule.
docker exec -w /ws zephyr-dev west blobs fetch hal_espressif
docker exec zephyr-dev bash -lc 'cd /ws/modules/crypto/mbedtls && git submodule update --init --recursive'
```

## Build & run on `native_sim`

```sh
docker exec -w /ws/app -e ZEPHYR_SDK_INSTALL_DIR=$SDK zephyr-dev \
  west build -b native_sim/native/64 apps/opcua-server --pristine
docker exec -w /ws/app zephyr-dev ./build/zephyr/zephyr.exe   # boots, starts server
```

## Build & flash the ESP32-WROOM-32 (from macOS)

1. Provide Wi-Fi credentials (see below), then cross-compile in the container:

   ```sh
   docker exec -w /ws/app -e ZEPHYR_SDK_INSTALL_DIR=$SDK zephyr-dev \
     west build -b esp32_devkitc/esp32/procpu apps/opcua-server --pristine \
     -- -DEXTRA_CONF_FILE=/ws/app/overlay-wifi-credentials.conf
   ```

   > The build targets the application directory `apps/opcua-server`. The Wi-Fi
   > credentials overlay stays at the repo root and is shared across apps, so
   > pass it by absolute path (`/ws/app/...`) rather than relative to the app.

   The build output lands in `./build/` on the host (the repo is mounted).

2. Flash from the host with `esptool` (the merged image goes at offset `0x1000`):

   ```sh
   ~/flashenv/bin/python -m esptool --chip esp32 --port /dev/cu.usbserial-0001 \
     --baud 460800 write_flash 0x1000 build/zephyr/zephyr.bin
   ```

   Find the port with `ls /dev/cu.*` (ESP32 boards use a CP210x/CH34x USB-serial
   bridge; install its driver if the port doesn't appear). Watch logs with
   `~/flashenv/bin/python -m serial.tools.miniterm /dev/cu.usbserial-0001 115200`.

> **Older ESP32 silicon:** rev-1.0 WROOM chips need
> `CONFIG_ESP32_USE_UNSUPPORTED_REVISION=y` (already set in the board conf) or
> the v4.4 bootloader refuses to boot.

Raspberry Pi Pico W flashes via UF2: hold BOOTSEL, plug in, copy
`build/zephyr/zephyr.uf2` to the `RPI-RP2` volume (same on macOS/Linux).

## Build & flash the Adafruit QT Py ESP32-S3

Dual-core ESP32-S3 — Wi-Fi works end-to-end (unlike the S2). The `&wifi` node is
disabled by default in the S3 SoC devicetree, so the app supplies a board overlay
(`apps/opcua-server/boards/adafruit_qt_py_esp32s3_esp32s3_procpu.overlay`) to
enable it.

```sh
docker exec -w /ws/app -e ZEPHYR_SDK_INSTALL_DIR=$SDK zephyr-dev \
  west build -b adafruit_qt_py_esp32s3/esp32s3/procpu apps/opcua-server --pristine \
  -- -DEXTRA_CONF_FILE=/ws/app/overlay-wifi-credentials.conf
```

The S3 uses **native USB (USB-Serial-JTAG)**, so flash from the host at offset
**`0x0`** (not `0x1000`) and use `--before usb_reset` (plain `default_reset`
drops the CDC port with "Device not configured"):

```sh
P=$(ls /dev/cu.usbmodem* | head -1)
~/flashenv/bin/python -m esptool --chip esp32s3 --port "$P" --baud 460800 \
  --before usb_reset --after hard_reset write_flash 0x0 build/zephyr/zephyr.bin
```

The console (and DHCP IP) is on the same native-USB port at 115200; open it
**without toggling DTR/RTS** so you don't reset the board.

## Build & flash the ESP32-S2 Feather TFT

```sh
docker exec -w /ws/app -e ZEPHYR_SDK_INSTALL_DIR=$SDK zephyr-dev \
  west build -b adafruit_feather_esp32s2_tft/esp32s2 apps/opcua-server --pristine \
  -- -DEXTRA_CONF_FILE=/ws/app/overlay-wifi-credentials.conf
```

The Feather S2 has **native USB only** (no USB-serial bridge), so:

- **Enter the ROM bootloader before flashing:** hold `BOOT`/DFU, tap `RESET`,
  release `BOOT`. It enumerates as `/dev/cu.usbmodem*`.
- **Flash without letting esptool touch reset** (native USB drops otherwise):
  ```sh
  P=$(ls /dev/cu.usbmodem* | head -1)
  ~/flashenv/bin/python -m esptool --chip esp32s2 --port "$P" \
    --before no_reset --after hard_reset --baud 460800 \
    write_flash 0x1000 build/zephyr/zephyr.bin
  ```
- There is **no serial console** on this board under Zephyr (its console is on
  `uart1`/GPIO39, and the S2 has no USB-Serial-JTAG). Use the on-board **TFT
  status display** (below) to read connectivity state instead.

### TFT status display

On the Feather ESP32-S2 TFT (`CONFIG_APP_DISPLAY_STATUS=y`, enabled in its board
conf) the screen shows the boot/connectivity stage and the device IP so it can
be read without a console:

| Screen | Meaning |
|--------|---------|
| **Red** | Booting |
| **Blue** | Connecting to Wi-Fi (a number = a Wi-Fi disconnect reason code) |
| **Green** + 4 stacked numbers | Connected; the numbers are the IPv4 octets |
| **Teal** + IP | OPC-UA server running |
| **Orange** (+ number) | Error stage (number = Wi-Fi connect failure code) |

## Wi-Fi credentials (never commit secrets)

```sh
cp overlay-wifi-credentials.conf.example overlay-wifi-credentials.conf
# edit → CONFIG_APP_WIFI_SSID / CONFIG_APP_WIFI_PSK  (2.4 GHz network)
```

`overlay-wifi-credentials.conf` is git-ignored and lives at the repo root
(shared across apps). Pass it by absolute path on the hardware build:
`-- -DEXTRA_CONF_FILE=/ws/app/overlay-wifi-credentials.conf`.

## Connectivity resilience & status LED

The connectivity layer (`lib/common/net.c`) recovers autonomously from network
disruptions — you should not need to power-cycle a device to get it back online:

- A watchdog on the 3 s status tick treats "not connected **or** no IPv4 address"
  as offline (independent of Wi-Fi events), forces reconnects, and detects a
  silently-lost DHCP lease.
- Reconnect is robust: a stale association is cleared before retrying, and a
  failed connect is retried rather than abandoned.
- **Last-resort self-reboot** — if a device stays offline past
  `CONFIG_APP_NET_REBOOT_TIMEOUT_S` (default 300 s) despite retries, it reboots to
  recover. Disable with `CONFIG_APP_NET_RECONNECT_REBOOT=n` (e.g. on the bench).
  This is *network-level* recovery: it runs on the status tick, so it cannot help
  if the firmware itself has stalled. That case is the liveness watchdog's job
  (see [Stalls, liveness watchdog & diagnostics](#stalls-liveness-watchdog--diagnostics)).
- The status tick and reconnects run on a dedicated **connectivity work queue**
  (`net_wq`, `CONFIG_APP_NET_WORKQ_STACK_SIZE`, default 3072), not the system
  workqueue, whose 1 KB stack they overflowed on the ESP32.

**Status LED** (`CONFIG_APP_STATUS_LED`, on by default): tells you at a glance
whether the *device* is on the network — **blinking = not connected**
(booting/associating/reconnecting), **steady = connected and serving**. So if the
LED is steady but a collector can't read the device, the problem is the
collector/network path, not the device. It uses the board's `led0` alias
(WROOM: on-board LED on GPIO2, see the board `.overlay`); it's a no-op on boards
without an LED (the S2 TFT shows the same state on-screen; the S3 NeoPixel is not
yet wired up).

## Stalls, liveness watchdog & diagnostics

A device that stops answering and **never comes back without a power-cycle** has
a stalled firmware, not just a dropped network. The ESP32 builds had exactly that
until the system workqueue stack overflow was fixed; the record is in
`openspec/changes/esp32-network-freeze-investigation/evidence.md`. Three tools
help with the next one.

### Liveness watchdog (`CONFIG_APP_LIVENESS`)

Each watched context (the system workqueue, the connectivity queue `net_wq`, the
protocol server thread and, for SNMP, the trap sender) gets a task watchdog
channel that is fed only when that context makes progress. If one stops for
`CONFIG_APP_LIVENESS_TIMEOUT_S` (default 30 s), the device resets. The SoC
watchdog (`watchdog0`) backs this up, so a lockup with interrupts masked also
resets the device, after about 5 s. The next boot says why:

```
<err> app_liveness: LIVENESS RESET: context netwq stalled at uptime 1234 s (boot 3)
<inf> app_liveness: reset cause 0x2 (software) esp_reason 3 boot 3
```

- `LIVENESS RESET: context <ctx>` means the named context stopped making
  progress (`wq`, `netwq`, `proto` or `trap`), with the uptime when it did.
- `hardware watchdog reset` means the task watchdog never got to run: a hard
  lockup that only the SoC watchdog caught.
- `boot N` counts resets since the last power-on, so a reset loop shows up.

The watchdog is **on by default for Wi-Fi builds**, since the acceptance soaks
(24 h and 10 h on four boards, plus an access-point restart) produced no false
reset. For bench work under a debugger, where a reset would hide the problem,
set `CONFIG_APP_LIVENESS=n`.

The connectivity queue has its own, longer timeout
(`CONFIG_APP_LIVENESS_NETWQ_TIMEOUT_S`, default 120 s): Espressif Wi-Fi driver
calls block it for well over 30 s while an access point disappears or returns,
and resetting for that is a false positive. `CONFIG_APP_LIVENESS_SELFTEST` (test builds only)
injects a failure after `CONFIG_APP_LIVENESS_SELFTEST_DELAY_S`: a blocked
system workqueue, a stopped protocol loop, or a busy loop with interrupts
masked.

A protocol frontend feeds its channel by calling `app_alive(APP_CTX_PROTO)` from
its own loop. Blocking waits must be bounded so an idle server still feeds it;
see `lib/common/README.md`.

### Diagnostic overlay (`overlay-diag.conf`)

Add `overlay-diag.conf` to `EXTRA_CONF_FILE` to get `CONFIG_APP_DIAG` with
immediate logging, net buffer usage, heap statistics and the thread analyzer.
Every `CONFIG_APP_DIAG_PERIOD_S` (default 10 s) a thread that is not on the
system workqueue logs:

```
<inf> app_diag: HEALTH up=120 n=12 beat[wq=1 netwq=2 proto=0 trap=0] work[status=D reconn=- sim=D probe=D] heap=23216/56112 malloc=71324/71324 pkt[rx=8/8 tx=8/8] buf[rx=24/24 tx=24/24] min[rx=19 tx=21] net=up
<inf> app_diag: WIFI st=9 rssi=-61 ch=11
```

- `beat[...]`: seconds since each context last made progress (`-` means it has
  not started).
- `work[...]`: state of the firmware's work items: `R` running, `Q` queued,
  `D` delayed, `C` cancelling, `-` idle.
- `heap` and `malloc`: free and total bytes. `pkt`, `buf` and `min`: free net
  packets and buffers, and the lowest buffer count seen.
- A `HEALTH` line with no `WIFI` line after it means the Wi-Fi status query
  blocked.

When a context has not made progress for three periods, a `STALE` line names its
thread, state and the wait queue it is blocked on (`pended_on`), followed by a
dump of every thread. Map the address to a kernel object with
`nm -n -S build/zephyr/zephyr.elf`, using the ELF of the build that ran.

Immediate logging and the analyzer change timing and use more stack. The overlay
raises the workqueue stacks to match; don't ship it.

### Soak harness (`scripts/soak/`)

`scripts/soak/run.sh` runs one timed experiment against a device and writes
comparable records:

```sh
# build (inside the container) and flash from the host
scripts/soak/build.sh build_soak snmp-agent esp32_devkitc/esp32/procpu soak-trap.local.conf
scripts/soak/flash.sh build_soak /dev/cu.usbserial-210
# one hour, console captured from boot, SNMP polled every 5 s, traps recorded
scripts/soak/run.sh --board esp32-cam --app snmp --host 192.168.68.74 \
    --port /dev/cu.usbserial-210 --variant baseline --duration 3600
```

The run's files go to `scripts/soak/runs/` (git-ignored):
- `*.console.log`: the console, stamped with host time.
- `*.polls.log`: one probe and one ping every 5 s.
- `*.traps.log`: traps received by an unprivileged `snmptrapd` on port 1162.
- `*.summary.json`: outages (3 failed probes in a row), time to failure,
  recovery time, whether the device ever answered, and counts of boots and
  liveness resets from the console.

See [`scripts/soak/README.md`](scripts/soak/README.md) for the options and the
per-protocol probes.

On the ESP32-CAM, opening the serial port resets the board, so the harness opens
it once, at the start of the run. When soaking the Modbus server, stop other
Modbus clients first: the server serves one client at a time.

### Liveness and diagnostics options

| Kconfig | Default | Purpose |
|---------|---------|---------|
| `APP_LIVENESS` | `y` on Wi-Fi builds | Reset the device when a watched context stalls |
| `APP_LIVENESS_NETWQ_TIMEOUT_S` | `120` | Same, for the connectivity queue |
| `APP_LIVENESS_TIMEOUT_S` | `30` | Seconds without progress before a reset |
| `APP_LIVENESS_SELFTEST` | `n` | Inject a failure (test builds only) |
| `APP_DIAG` | `n` | Health lines and stall reports (use `overlay-diag.conf`) |
| `APP_DIAG_PERIOD_S` | `10` | Health line period |
| `APP_NET_WORKQ_STACK_SIZE` | `3072` | Connectivity work queue stack |
| `APP_MODBUS_CLIENT_IDLE_TIMEOUT_S` | `60` | Drop a Modbus client that sends nothing for this long |

### Memory footprint (ESP32-WROOM-32, `esp32_devkitc/esp32/procpu`)

Measured with Zephyr 4.4.2 and SDK 1.0.1 (`FLASH` and `dram0_0_seg` from the
link map), with only the Wi-Fi credentials overlay unless noted. "Before" is
the tree before the freeze fix.

| App | Before: flash / DRAM | Release: flash / DRAM | + liveness | + `overlay-diag.conf` + liveness |
|---|---|---|---|---|
| `opcua-server` | 735,536 B / 124,088 B (63.1%) | 735,664 B / 124,288 B (63.2%) | 737,840 B / 124,656 B (63.4%) | 739,120 B / 124,176 B (63.2%) |
| `modbus-server` | 575,984 B / 100,784 B (51.3%) | 576,224 B / 100,984 B (51.4%) | 578,288 B / 101,352 B (51.6%) | 580,032 B / 100,872 B (51.3%) |
| `snmp-agent` | 564,880 B / 125,416 B (63.8%) | 564,992 B / 125,616 B (63.9%) | 566,960 B / 125,968 B (64.1%) | 568,544 B / 125,504 B (63.8%) |

Flash is out of 4,194,048 B (all builds use 13–18%).

Thread stacks sit in the no-init RAM section, which the DRAM figure above does
not include. The fix grows that section by 6,112 B:
- the `net_wq` stack (3,072 B);
- a larger system workqueue stack (+1,024 B);
- a larger log thread stack (+1,024 B);
- a larger socket-service stack (+992 B).

On the WROOM, `opcua-server` takes its libc `malloc` arena from the remaining
RAM. It reports `libc heap size 70 kB` at boot both before and after the fix (with liveness on), so the larger stacks don't reduce it.

QT Py ESP32-S3 release builds: `opcua-server` 684,500 B / 239,792 B (60.1%),
`modbus-server` 579,428 B / 196,488 B (49.2%), `snmp-agent` 578,068 B /
219,232 B (54.9%). All builds above compile with no warnings.

## Finding the device (mDNS / DNS-SD)

The firmware advertises over mDNS, so no IP is needed. On macOS (Bonjour):

```sh
ping tedge-opcua.local                 # resolves to the device's DHCP address
dns-sd -B _opcua-tcp._tcp              # lists the "tedge-opcua" OPC-UA service
dns-sd -B _modbus._tcp                 # lists the "tedge-modbus" Modbus service
dns-sd -B _snmp._udp                   # lists the "tedge-snmp<mac>" SNMP agent
```

Each firmware advertises its own DNS-SD service type (`_opcua-tcp`/`_modbus`
over TCP, `_snmp` over UDP) via `CONFIG_APP_DNSSD_*`, and answers to its unique
`<hostname>.local` name.

> **Discovery note.** The devices answer service-discovery queries correctly:
> a browse from a Linux host (for example Python `zeroconf`, or `avahi-browse`)
> lists every device with its address, port and TXT record, and
> `<hostname>.local` lookups work everywhere. On one macOS machine, `dns-sd -B`
> listed nothing for these service types while browsing `_ssh._tcp` worked;
> packet captures showed macOS never sent the query, so that is a client-side
> quirk rather than a device fault. Note also that Zephyr 4.4.2 does not answer
> direct SRV/TXT queries (only PTR), which is what `dns-sd -L` asks for.

Point an OPC-UA client at `opc.tcp://tedge-opcua.local:4840`. Quick check with
the bundled Python client:

```sh
~/flashenv/bin/python - <<'PY'
import asyncio
from asyncua import Client
async def main():
    async with Client("opc.tcp://tedge-opcua.local:4840", timeout=15) as c:
        dev = [x for x in await c.nodes.objects.get_children()
               if (await x.read_browse_name()).Name == "tedge-opcua-device"][0]
        for m in await dev.get_children():
            print((await m.read_browse_name()).Name, "=", await m.read_value())
asyncio.run(main())
PY
```

## Configuration options

Board-agnostic settings live in `Kconfig` / `prj.conf`:

| Kconfig | Default | Purpose |
|---------|---------|---------|
| `APP_DEVICE_NAME` | `tedge-opcua-device` | OPC-UA application/server name |
| `APP_OPCUA_PORT` | `4840` | OPC-UA `opc.tcp` port |
| `APP_SAMPLE_INTERVAL_MS` | `1000` | data-source sampling interval |
| `APP_WIFI_SSID` / `APP_WIFI_PSK` | *(empty)* | Wi-Fi credentials (via overlay) |

The mDNS hostname is `CONFIG_NET_HOSTNAME` (`tedge-opcua`), set per board.
Override any value at build time, e.g. `-- -DCONFIG_APP_OPCUA_PORT=4855`.

## Writable data points & subscriptions

All application nodes live in namespace `ns=1`. The Device object exposes two
**writable** control nodes alongside the read-only measurements:

| Node id | Name | Type | Access | Notes |
|---------|------|------|--------|-------|
| `ns=1;s=Setpoint` | Setpoint | Int32 | read/write | operator target; clamped to `APP_SETPOINT_MIN`..`APP_SETPOINT_MAX` (default −1000..1000) |
| `ns=1;s=Running` | Running | Boolean | read/write | whether the simulated process is running |

Writes are validated (out-of-range `Setpoint` is clamped; writing a read-only
measurement returns `BadNotWritable`). Values are held in RAM (not persisted).
Example:

```python
from asyncua import ua
await client.get_node("ns=1;s=Setpoint").write_value(ua.Variant(500, ua.VariantType.Int32))
await client.get_node("ns=1;s=Running").write_value(ua.Variant(True, ua.VariantType.Boolean))
```

**Subscriptions** (monitored items / change notifications) are **off by default**.
They require open62541's `REDUCED` namespace-zero, whose larger nodeset OOMs at
namespace init on the ESP32-WROOM's ~68 KB heap. So the committed amalgamation
uses the `minimal` profile (writes work, subscriptions off). To enable
subscriptions on a **higher-RAM board** (e.g. an ESP32-S2 with PSRAM), regenerate
with the `reduced` profile and rebuild:

```sh
scripts/regen-open62541.sh <open62541-src> 300 reduced   # NS0=REDUCED, subscriptions ON
```

Subscription resource caps are set from `APP_OPCUA_MAX_SUBSCRIPTIONS` /
`APP_OPCUA_MAX_MONITORED_ITEMS`.

## open62541 integration

open62541 is vendored as a single-file amalgamation in
`third_party/open62541/`, regenerated by
[`scripts/regen-open62541.sh`](scripts/regen-open62541.sh). It is built with a
minimal, read-only, POSIX-architecture profile (MINIMAL namespace-0; no
subscriptions/methods/discovery/history; single-threaded; 8 kB buffers) and
carries small Zephyr portability patches (IPv6 off; skip the interrupt/UDP/
Ethernet connection managers; neutralize `pipe()`; 8 kB shared RX buffer;
`getaddrinfo` NULL-host fallback to `0.0.0.0`; monotonic clock via
`k_uptime_get()`). To refresh it:

```sh
docker exec -w /ws/app -e WEST_TOPDIR=/ws zephyr-dev \
  bash scripts/regen-open62541.sh /ws/modules/lib/open62541 300
```

### RAM notes (WROOM-32 has no PSRAM)

open62541 shares the WROOM's ~300 KB SRAM with the Wi-Fi stack (~68 KB free
heap after Wi-Fi). It fits only after: MINIMAL namespace-0, an 8 KB shared RX
buffer, 8 KB per-connection buffers, and a small `CONFIG_HEAP_MEM_POOL_SIZE`
(open62541 uses the libc `malloc` arena, sized `-1` = all remaining RAM). The
ESP32-S2 (2 MB PSRAM) has far more headroom if these limits become tight.

## Repository layout (multi-protocol workspace)

Firmware is organised as a shared core plus per-protocol libraries and apps
(see `lib/common/README.md` for the protocol-frontend contract):

```
lib/common/            shared core: connectivity, display, data model, identity
                       + selectable simulations (sim_environment, sim_pump, sim_switch)
lib/opcua/             OPC-UA frontend (open62541) + address-space adapter
lib/modbus/            Modbus TCP frontend (Zephyr modbus subsystem, port 502)
lib/snmp/              SNMPv2c agent frontend (in-repo BER; UDP 161 + traps 162)
lib/frontend-template/ copy-me skeleton for a new protocol (CAN/...)
apps/opcua-server/     OPC-UA firmware: lib/common (env sim) + lib/opcua
apps/modbus-server/    Modbus TCP firmware: lib/common (pump sim) + lib/modbus
apps/snmp-agent/       SNMP agent firmware: lib/common (switch sim) + lib/snmp
  ├── boards/<board>.conf       per-app board overlays (RAM/Wi-Fi tuning)
  └── points.d/<proto>/*.toml   point library: the app's address map for a
                                collector (see "Point libraries" below)
```

Each firmware is built by targeting its app directory, e.g.
`west build -b <board> apps/modbus-server`. A new protocol becomes a new
`lib/<protocol>` + `apps/<protocol>` pair; nothing else needs to change.

### Simulations (per-firmware, selectable)

The shared data model is driven by a **simulation** chosen with a Kconfig
`choice` (in each app's `prj.conf`):

- `CONFIG_APP_SIM_ENVIRONMENT` (default) — temperature/humidity/pressure +
  `Setpoint`/`Running`. Used by `apps/opcua-server`.
- `CONFIG_APP_SIM_PUMP` — a control-driven pump/motor: measurements react to the
  `speed_setpoint`/`running`/`mode` controls via pump affinity laws (flow ∝ speed,
  pressure ∝ speed²), a motor-thermal lag, `run_hours` that accrue only while
  running, and an over-temp fault. Used by `apps/modbus-server`.
- `CONFIG_APP_SIM_SWITCH` — a managed switch/router: a fixed set of Ethernet
  interfaces (`CONFIG_APP_SIM_SWITCH_IF_COUNT`, ≤ 8) with admin/oper status,
  1 Gbit/s nominal speed and monotonic traffic counters; one port flaps its link
  every `CONFIG_APP_SIM_SWITCH_FLAP_PERIOD_STEPS` sampling steps (default 15,
  `0` = never) to drive link up/down events. Used by `apps/snmp-agent`.

Frontends are simulation-agnostic, so any simulation can back any protocol.

## Modbus TCP server firmware

Build and flash exactly like the others, targeting `apps/modbus-server` (Wi-Fi
board; no serial/RS-485 — this is Modbus **TCP** on port 502):

```sh
docker exec -w /ws/app -e ZEPHYR_SDK_INSTALL_DIR=$SDK zephyr-dev \
  west build -b esp32_devkitc/esp32/procpu apps/modbus-server --pristine \
  -- -DEXTRA_CONF_FILE=/ws/app/overlay-wifi-credentials.conf
# flash as for the WROOM/S3 above; advertises _modbus._tcp on port 502
```

**Register map** (unit id 1, zero-based; pump simulation):

| Object | Addr | Meaning |
|--------|------|---------|
| Input Reg (RO) | 0 / 1 / 2 / 3 / 4 | flow ×10 / pressure ×100 / motor_temp ×10 (signed) / rpm / vibration ×100 |
| Input Reg (RO) | 10–11 | run time, seconds — uint32, big-endian pair |
| Input Reg (RO) | 20–21 / 22–23 / 24–25 | flow / pressure / motor_temp as IEEE-754 float (BE pairs) |
| Holding Reg (RW) | 0 / 1 | speed_setpoint (0–100) / mode (0=off,1=auto,2=manual) |
| Coil (RW) | 0 | running |
| Discrete In (RO) | 0 / 1 / 2 | running mirror / fault (over-temp) / network connected |

**Client test recipe** (`pymodbus`; `pip install pymodbus`):

```python
from pymodbus.client import ModbusTcpClient
c = ModbusTcpClient("<device-ip>", port=502); c.connect()
c.write_coil(0, True, device_id=1)          # start
c.write_register(1, 2, device_id=1)         # mode = manual
c.write_register(0, 80, device_id=1)        # speed 80 %  (write 150 -> clamps to 100)
print(c.read_input_registers(0, count=5, device_id=1).registers)  # flow,pressure,temp,rpm,vib
print(c.read_discrete_inputs(0, count=3, device_id=1).bits)       # running,fault,net
```

## SNMP agent firmware

`apps/snmp-agent` presents the device as a managed **switch/router**: a
minimal **SNMPv2c** agent (UDP 161) answering GET/GETNEXT/GETBULK over the
system group + a MIB-II interfaces table, plus **traps** (UDP 162) on coldStart
and interface link up/down. It uses an in-repo BER/ASN.1 codec — no external
SNMP stack — and is UDP-only, so it also runs end-to-end on `native_sim`. On
hardware it advertises itself over mDNS as `_snmp._udp` and answers to
`<hostname>.local`.

```sh
docker exec -w /ws/app -e ZEPHYR_SDK_INSTALL_DIR=$SDK zephyr-dev \
  west build -b esp32_devkitc/esp32/procpu apps/snmp-agent --pristine \
  -- -DEXTRA_CONF_FILE=/ws/app/overlay-wifi-credentials.conf
# point traps at your manager, e.g. -DCONFIG_APP_SNMP_TRAP_MANAGER=\"192.168.68.10\"
```

**Discover it (mDNS / DNS-SD):**

```sh
dns-sd -B _snmp._udp local.                       # lists "tedge-snmp<mac>" instances
dns-sd -L tedge-snmp<mac> _snmp._udp local.        # -> <hostname>.local:161
snmpwalk -v2c -c public tedge-snmp<mac>.local 1.3.6.1.2.1   # poll by name, no IP
```

The hostname is unique (`tedge-snmp` + Wi-Fi MAC, from `CONFIG_NET_HOSTNAME_UNIQUE`),
so the base `tedge-snmp.local` does not resolve — use the full advertised name.

**Poll it** (net-snmp; `-v2c -c public` — the agent serves v2c only):

```sh
snmpget    -v2c -c public <device-ip> sysDescr.0 sysName.0 sysUpTime.0
snmpwalk   -v2c -c public -On <device-ip> 1.3.6.1.2.1        # system + ifTable
snmpbulkwalk -v2c -c public <device-ip> 1.3.6.1.2.1.2.2      # ifTable via GETBULK
```

**MIB view** (all read-only; SET is refused with `notWritable`):

| OID | Object | Type | Source |
|-----|--------|------|--------|
| `1.3.6.1.2.1.1.1.0` | sysDescr | OCTET STRING | firmware name/version/build |
| `1.3.6.1.2.1.1.3.0` | sysUpTime | TimeTicks | since agent start |
| `1.3.6.1.2.1.1.5.0` | sysName | OCTET STRING | device hostname |
| `1.3.6.1.2.1.2.1.0` | ifNumber | INTEGER | interface count |
| `…2.2.1.{1,2,3,4,5}.<n>` | ifIndex/ifDescr/ifType/ifMtu/ifSpeed | INTEGER/STRING/Gauge32 | switch sim |
| `…2.2.1.{7,8}.<n>` | ifAdminStatus/ifOperStatus | INTEGER (up=1,down=2) | switch sim |
| `…2.2.1.{10,11,16,17}.<n>` | ifIn/OutOctets, ifIn/OutUcastPkts | Counter32 | switch sim |
| `1.3.6.1.4.1.99999.1.{1,2,3}.0` | firmware name / version / build timestamp | OCTET STRING | `lib/common` identity |

The last three live on the private-enterprise arc `sysObjectID.0` names. They are
the same strings `sysDescr.0` packs into one sentence, given one object each so a
collector can report the running firmware without parsing prose:

```sh
snmpget -v2c -c public -On <device-ip> 1.3.6.1.4.1.99999.1.2.0   # -> "0.1.0"
snmpwalk -v2c -c public -On <device-ip> 1.3.6.1.4.1.99999        # name, version, build
```

**Receive traps** (net-snmp `snmptrapd`, on the configured manager host):

```sh
sudo snmptrapd -f -Lo -c /dev/null      # prints coldStart on boot, then
                                        # linkDown/linkUp with ifIndex as ports flap
```

**Trap volume.** One port toggles every `CONFIG_APP_SIM_SWITCH_FLAP_PERIOD_STEPS`
sampling steps — 15 s by default, so ~4 notifications a minute (~5,700 a day),
each also raising or clearing an alarm in a collector. That is what makes a short
demo interesting and a long-running device noisy, so raise it for anything left
running, or switch flapping off and exercise only the counters:

```sh
-DCONFIG_APP_SIM_SWITCH_FLAP_PERIOD_STEPS=900   # a flap every 15 min
-DCONFIG_APP_SIM_SWITCH_FLAP_PERIOD_STEPS=0     # links never flap
```

## Point libraries (for a tedge-dot collector)

Each firmware ships the point list a collector needs to read it, as a **point
library** — the address map of one device type in its own TOML file, with no
connection details in it. The firmware owns that map (the register layout, the
node ids, the OIDs), so the list lives next to the firmware and is versioned with
it, rather than being copy-pasted into every gateway that polls one of these
boards. The format is [tedge-dot](https://github.com/thin-edge/tedge-dot)'s
(OT-connector contract §3.4).

```
apps/opcua-server/points.d/opcua/zephyr-opcua.toml          ns=1 nodes: measurements + Setpoint/Running
apps/modbus-server/points.d/modbus/zephyr-modbus-pump.toml  unit 1 register map: pump sim + controls
apps/snmp-agent/points.d/snmp/zephyr-snmp-switch.toml       system group, ifTable rows, traps
```

A device instance then declares only where to reach the board, and names the
library it is an instance of:

```toml
[[device]]
name             = "tedge-snmp-device"
protocol_address = { host = "tedge-snmp<mac>.local", port = 161, version = "v2c", community = "public" }
points_from      = ["zephyr-snmp-switch"]
```

A bare name resolves as `<dir>/<protocol>/<name>.toml` along the collector's
search path, so from a checkout point that path at the app directory (an
installed package finds its own copies):

```sh
export TEDGE_DOT_POINT_LIBRARY_PATH=/path/to/apps/snmp-agent/points.d
tedge-dot read -c snmp.toml -p if4_oper_status
```

**What the SNMP library declares.** Meaning is attached to each point next to its
address, so the device shows up usefully with no per-deployment flow parameters:

| | Points | Declared as |
|---|---|---|
| Measurements | `uptime`, per-port `if<n>_{in,out}_{octets,ucast_pkts}` | plain points with a `unit` |
| Alarms | per-port `if<n>_oper_status` | `meta.alarm`, one alarm type per port, raised `when.equals = 2` |
| Events | `link_down_if`, `link_up_if`, `cold_start` | `meta.event` with `every = true` — traps are occurrences |
| Device state | `firmware_name`, `firmware_version`, `build_timestamp`, `sys_*` | `meta.measurement = false`; the first three also `meta.parameter.key = "firmware.*"` |

Link state is alarmed from the **polled** `ifOperStatus` column rather than from
the traps, so an alarm names the port it is about and clears on the next poll
that reads it up; the traps become events instead. The file documents the other
arrangement (alarm on the trap) in a commented block at its end.

Two things to keep in sync when the firmware changes: the ifTable rows cover
`CONFIG_APP_SIM_SWITCH_IF_COUNT = 5`, and polling a row the agent does not serve
yields a permanently `bad` sample rather than nothing — a build with a different
interface count needs rows added or switched off with `enabled = false`.

## Adding another Wi-Fi board

1. Add `apps/<app>/boards/<fully-qualified-board>.conf` (e.g.
   `esp32s2_saola.conf`) with that board's Wi-Fi driver + IP stack + mDNS +
   `CONFIG_NET_HOSTNAME` — copy an existing ESP32 conf. HWMv2 matches the
   *fully-qualified* filename (board + qualifiers, `/` → `_`).
2. Build with `-b <board> apps/<app>`; the core and OPC-UA logic need no changes.
