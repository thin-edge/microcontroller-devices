# Device agent on ESP-IDF (tedge-agent-idf)

The thin-edge.io client, [tiny-edge](https://github.com/thin-edge/tiny-edge),
on ESP-IDF and FreeRTOS, with no protocol server. It is the ESP-IDF
counterpart of `apps/tedge-agent` (Zephyr): the device is managed from
Cumulocity and reports its own vitals.

| What | How |
|---|---|
| Set-up | Over USB with `tedge-onboard`: Wi-Fi, the tenant, and registration. Nothing is built into the image |
| Device management | Restart, firmware update with rollback, parameters, log upload, remote access, and the cloud shell (`free`, `version`, `uptime`) |
| Telemetry | The `device` measurement every 60 s: `uptime`, `heap_free`, and `rssi`. The tiny-edge measurements Smart Function stores it as `c8y_Device` |
| Firmware name | `tedge-agent-idf-<chip>`, for example `tedge-agent-idf-esp32c6`, so an update offered under it only holds images for that chip |
| Device type | `thin-edge.io-esp-idf-agent` |
| Version | `VERSION`, the same file and format as the Zephyr applications, so a release bumps it with them |

Built and run on the ESP32-C6, and released for it as the `tedge-onboard`
variant: `tedge-agent-idf-tedge-onboard-esp32c6-devkitc-<version>` with
`.factory.bin`, `.app.bin`, and `.zip`, like the Zephyr images. The release
build (`scripts/release/build.sh`) names the firmware after the variant and
the board instead of the chip. CI also builds it for the ESP32-S3 (workflow
`esp-idf`); the S3 image joins the releases once it has run on that board.

## Build

ESP-IDF 6.1, for example in the `espressif/idf:v6.1` container:

```sh
cd apps/tedge-agent-idf
idf.py set-target esp32c6
idf.py build
```

The build fetches tiny-edge at the tag in `CMakeLists.txt`
(`TINY_EDGE_VERSION`). To try a change to the client, point it at a local
checkout:

```sh
idf.py -D TINY_EDGE_DIR=/path/to/tiny-edge build
```

tiny-edge is fetched as a whole repository, not through the component
manager: its ESP-IDF component builds the portable core from beside it, and
the component manager's `git` + `path` dependency copies only the
component's own directory.

## Flash and set up

Flash the whole image the first time:

```sh
idf.py flash
# or, from the CI artifact or a copied image:
esptool --chip esp32c6 write-flash 0x0 tedge-agent-idf-esp32c6-<version>.factory.bin
```

The device starts without a Wi-Fi network and waits. Leave it plugged in and
run [`tedge-onboard`](https://github.com/thin-edge/tiny-edge/tree/main/tools/onboard)
on the same computer. The tool finds the board, gives it the computer's
Wi-Fi network, registers it in your Cumulocity tenant (through go-c8y-cli),
and passes it the tenant and a one-time password. The device then enrolls
for its certificate and connects. The network and the certificate stay on
the device, so later boots connect at once.

For a firmware update, upload `<stem>.app.bin` to the tenant's firmware
repository under the firmware name above. An image that does not connect
within its confirmation deadline is rolled back by the bootloader.

## Tenant

The device talks to the Cumulocity MQTT Service. The tenant needs the
tiny-edge Smart Functions and DTM definitions:
[tiny-edge `cumulocity/README.md`](https://github.com/thin-edge/tiny-edge/blob/main/cumulocity/README.md).

## Differences from the Zephyr tedge-agent

- No provisioner image and no MCUboot: ESP-IDF's bootloader runs the OTA
  slots and the rollback, and set-up is `tedge-onboard` over USB instead of
  Improv BLE or lab-ztp-provisioner.
- No status LED or identify pattern yet.
- tiny-edge 0.5.0, so the client's parameter sets are `tiny_edge` and
  `tiny_edge_remote_access`, not `zephyr_tedge`.
