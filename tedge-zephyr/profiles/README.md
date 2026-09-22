# Feature profiles

Kconfig overlays that select a coherent feature set, for example:

```sh
west build -b <board> <app> -- -DEXTRA_CONF_FILE=/path/to/tedge-zephyr/profiles/minimal.conf
```

- `minimal.conf`: connection, inventory, health, telemetry and restart over one
  TLS session.
- `full.conf`: every feature, for boards with room (ESP32-C6, ESP32-S3; on the
  S3 with the mbedTLS heap in PSRAM).
- `ota.conf`: `minimal.conf` plus firmware update — the smallest image that can
  still be updated over the air. Two TLS sessions (MQTT and the download).
- `remote-access-enabler.conf`: connection, health, restart and one
  remote-access tunnel, for a device that only gives the cloud access to LAN
  hosts. Fits an ESP32-WROOM-32 with nothing else on it.

The measured costs are in each file's header.

`full.conf` asks for every implemented feature. It expects a sysbuild image with
MCUboot and a shell; on a build without them, Kconfig warns that firmware
update and the shell command were turned back off, which is the warning doing
its job.

A board's or application's default profile belongs to the application (its
`boards/*.conf`), not to this module.
