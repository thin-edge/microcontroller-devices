## Why

The status LED patterns (connected, not connected, provisioning, identify,
erase armed) are only visible on boards with a plain GPIO `led0`: of the BLE
provisioning boards, only the WROOM-32. The ESP32-C6 and ESP32-S3-DevKitC-1
carry a WS2812 addressable RGB LED that the firmware did not drive, so an
operator pressing the button had no feedback at all; during the button tests
of the `ble-wifi-provisioning` change the console was the only way to tell
whether a press registered.

## What Changes

- `lib/common/status_led.c` drives an addressable RGB LED (the board's
  `led-strip` alias) when the board has no `led0`, with the same patterns as
  the GPIO LED, and shows each state in its own colour: green connected, amber
  not connected, blue provisioning, white identify and button acknowledgement,
  red erase armed. About 1/8 brightness.
- The pattern engine runs on the system work queue instead of a kernel timer:
  an RGB LED update goes through a bus driver (I2S) that must not run in
  interrupt context. The GPIO LED behaves as before.
- `LED_STRIP` is enabled by default wherever the devicetree has a `led-strip`
  alias; board overlays only add the LED node.
- ESP32-C6 (WS2812 on GPIO8) and ESP32-S3-DevKitC-1 (GPIO48, board v1.0) get
  the LED node in all three applications and the Wi-Fi provisioner, which then
  also reports the Improv identify capability. The QT Py ESP32-S3 needs no
  board change: its upstream devicetree already has the NeoPixel (`led-strip`,
  WS2812 over SPI) and a GPIO hog for its power pin.

## Non-goals

- Brightness or colours as Kconfig options.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `wifi-connectivity`: "Connectivity status indicator (LED)" also covers an
  addressable RGB LED, with a colour per state.

## Impact

- `lib/common/status_led.{c,h}`, `lib/common/Kconfig`, two devicetree
  includes (`lib/common/dts/rgb-led-*.dtsi`), and the C6 and S3-DevKitC-1
  overlays of the three applications and the provisioner.
- Builds on the `ble-wifi-provisioning` change (the provisioning patterns);
  archive that one first.
