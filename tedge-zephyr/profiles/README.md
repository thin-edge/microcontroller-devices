# Feature profiles

Kconfig overlays that select a coherent feature set, for example:

```sh
west build -b <board> <app> -- -DEXTRA_CONF_FILE=/path/to/tedge-zephyr/profiles/minimal.conf
```

- `minimal.conf`: connection, inventory, health, telemetry and restart over one
  TLS session.
- `full.conf`: every feature, for boards with room (ESP32-C6, ESP32-S3).

Both are drafts. Until the features are implemented in the module, their
options are only selectable with `CONFIG_TEDGE_EXPERIMENTAL_FEATURES=y`. The
measured costs are in each file's header.

A board's or application's default profile belongs to the application (its
`boards/*.conf`), not to this module.
