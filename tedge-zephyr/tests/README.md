# tedge-zephyr tests

## Kconfig dependency rules

`kconfig/check-kconfig.sh` configures `samples/minimal` once per case in
`kconfig/cases/*.conf` and checks the resulting values and Kconfig warnings.
Each case states what it expects in comments:

```
# expect: CONFIG_TEDGE_FIRMWARE_UPDATE=n
# expect-warning: TEDGE_FIRMWARE_UPDATE .*BOOTLOADER_MCUBOOT
```

This check is needed because Zephyr only warns when a dependency overrides an
assignment ("was assigned the value 'y' but got the value 'n'"). The build
carries on without the feature.

Run it from a Zephyr workspace:

```sh
tedge-zephyr/tests/kconfig/check-kconfig.sh [build-root] [board]
```
