# Feature profiles

Kconfig overlays that select a coherent feature set, for example:

```sh
west build -b <board> <app> -- -DEXTRA_CONF_FILE=/path/to/tedge-zephyr/profiles/minimal.conf
```

The `minimal` and `full` profiles are added once the spikes have measured what
each feature costs. A board or application's default profile belongs to the
application (its `boards/*.conf`), not to this module.
