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

## Finding the device (mDNS / DNS-SD)

The firmware advertises over mDNS, so no IP is needed. On macOS (Bonjour):

```sh
ping tedge-opcua.local                 # resolves to the device's DHCP address
dns-sd -B _opcua-tcp._tcp              # lists the "tedge-opcua" OPC-UA service
```

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
                       + selectable simulations (sim_environment, sim_pump)
lib/opcua/             OPC-UA frontend (open62541) + address-space adapter
lib/modbus/            Modbus TCP frontend (Zephyr modbus subsystem, port 502)
lib/frontend-template/ copy-me skeleton for a new protocol (SNMP/CAN/...)
apps/opcua-server/     OPC-UA firmware: lib/common (env sim) + lib/opcua
apps/modbus-server/    Modbus TCP firmware: lib/common (pump sim) + lib/modbus
  └── boards/<board>.conf   per-app board overlays (RAM/Wi-Fi tuning)
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

## Adding another Wi-Fi board

1. Add `apps/<app>/boards/<fully-qualified-board>.conf` (e.g.
   `esp32s2_saola.conf`) with that board's Wi-Fi driver + IP stack + mDNS +
   `CONFIG_NET_HOSTNAME` — copy an existing ESP32 conf. HWMv2 matches the
   *fully-qualified* filename (board + qualifiers, `/` → `_`).
2. Build with `-b <board> apps/<app>`; the core and OPC-UA logic need no changes.
