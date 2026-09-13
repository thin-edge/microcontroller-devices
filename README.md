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
| Feather ESP32-S2 TFT | `esp32s2_saola` (representative) | co-primary | config authored, not yet built |
| Raspberry Pi Pico W | `rpi_pico/rp2040/w` | stretch | config authored, not yet built |
| Host simulation | `native_sim/native/64` | dev / CI | builds & runs (see NSOS note) |

All hardware targets are Wi-Fi-only (station mode).

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
  west build -b native_sim/native/64 . --pristine
docker exec -w /ws/app zephyr-dev ./build/zephyr/zephyr.exe   # boots, starts server
```

## Build & flash the ESP32-WROOM-32 (from macOS)

1. Provide Wi-Fi credentials (see below), then cross-compile in the container:

   ```sh
   docker exec -w /ws/app -e ZEPHYR_SDK_INSTALL_DIR=$SDK zephyr-dev \
     west build -b esp32_devkitc/esp32/procpu . --pristine \
     -- -DEXTRA_CONF_FILE=overlay-wifi-credentials.conf
   ```

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

## Wi-Fi credentials (never commit secrets)

```sh
cp overlay-wifi-credentials.conf.example overlay-wifi-credentials.conf
# edit → CONFIG_APP_WIFI_SSID / CONFIG_APP_WIFI_PSK  (2.4 GHz network)
```

`overlay-wifi-credentials.conf` is git-ignored. Pass it with
`-- -DEXTRA_CONF_FILE=overlay-wifi-credentials.conf` on the hardware build.

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

## Adding another Wi-Fi board

1. Add `boards/<fully-qualified-board>.conf` (e.g.
   `esp32s2_saola.conf`) with that board's Wi-Fi driver + IP stack + mDNS +
   `CONFIG_NET_HOSTNAME` — copy an existing ESP32 conf. HWMv2 matches the
   *fully-qualified* filename (board + qualifiers, `/` → `_`).
2. Build with `-b <board>`; the core app and OPC-UA logic need no changes.
