# c8y-direct spike firmware (throwaway)

The firmware for `openspec/changes/c8y-direct-spikes`. It measures and proves
the direct-to-Cumulocity transport before any production code is written in
`tedge-zephyr/`. It stands in for a user application: `lib/common` provides
Wi-Fi, identity and the data model, and it includes `tedge-zephyr` the way any
application would.

Each spike is a Kconfig option (`SPIKE_TLS_MQTT`, `SPIKE_OTA`, `SPIKE_ENROLL`,
`SPIKE_REMOTE_ACCESS`), so its cost can be measured on its own. The code is
written to measure, not to keep. Findings go into the change's `design.md`
("Spike results").

Boards: ESP32-C6 (primary) and ESP32-S3-DevKitC-1. Build with sysbuild so that
MCUboot and `slot1` are real:

```sh
west build -b esp32c6_devkitc/esp32c6/hpcore apps/c8y-spike --sysbuild \
  -- -DEXTRA_CONF_FILE=/ws/app/overlay-wifi-credentials.conf
```
