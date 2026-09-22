# Board settings for tedge-zephyr builds

One file per device: what a `tedge-zephyr` build needs from *this board*,
whatever the application — where the mbedTLS heap lives and how big it is,
PSRAM, TLS context and connection counts, network buffer depth. The feature
set comes from a profile in `tedge-zephyr/profiles/`; put the board file
**after** it so its sizes win:

```sh
west build --sysbuild -b esp32c6_devkitc/esp32c6/hpcore apps/modbus-server -- \
  "-DEXTRA_CONF_FILE=/ws/app/tedge-zephyr/profiles/full.conf;/ws/app/lib/common/tedge-boards/esp32c6-devkitc.conf"
```

| File | Board target | Notes |
|---|---|---|
| `esp32c6-devkitc.conf` | `esp32c6_devkitc/esp32c6/hpcore` | no PSRAM; heap in internal RAM |
| `esp32s3-devkitc.conf` | `esp32s3_devkitc/esp32s3/procpu` | N16R8; heap in 8 MB octal PSRAM |
| `qtpy-esp32s3.conf` | `adafruit_qt_py_esp32s3/esp32s3/procpu` | N4R2; heap in 2 MB quad PSRAM |
| `esp32-devkitc.conf` | `esp32_devkitc/esp32/procpu` | WROOM-32, no PSRAM; 8 KB records |
| `esp32-cam.conf` | `esp32_devkitc/esp32/procpu` | ESP32-CAM; heap in 4 MB quad PSRAM |

`extras/` holds feature additions for a board whose `tedge-ota` image carries
more than the profile (see `release/devices.yml`). What each board has been
measured to run is in DEVICES.md.
