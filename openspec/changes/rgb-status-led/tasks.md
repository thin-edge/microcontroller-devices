## 1. Implementation

- [x] 1.1 RGB backend in `status_led.c` (the `led-strip` alias when no `led0`), colours per mode, pattern engine on the system work queue
- [x] 1.2 `LED_STRIP` on by default where the devicetree has a `led-strip` alias
- [x] 1.3 WS2812 nodes: ESP32-C6 GPIO8, ESP32-S3-DevKitC-1 GPIO48; included by the three applications and the provisioner

## 2. Verification

- [x] 2.1 ESP32-C6 (someone at the board): green when serving; white acknowledgement then blue double-blink on the triple press; white fast blink on identify (capability now 0x01); red flicker when the erase is armed, then blue; amber then green after provisioning
- [x] 2.2 ESP32-S3-DevKitC-1 (N16R8, board v1.0): green when serving, blue double-blink in the provisioner, identify capability 0x01
- [x] 2.3 WROOM-32 (GPIO `led0`) after the work-queue change: steady when serving
- [x] 2.5 QT Py ESP32-S3 (on the Pi, upstream NeoPixel node): green when serving, blue double-blink in the provisioner
- [x] 2.6 ESP32-CAM (diymore, on an ESP32-CAM-MB base; plain GPIO LED): `lib/common/dts/esp32cam-status-led.overlay` moves `led0` from GPIO2 (an SD line on the CAM) to the red LED on GPIO33, active low; steady when serving, double-blink in the provisioner
- [x] 2.4 Regression builds: all boards and `native_sim`

## 3. Documentation

- [x] 3.1 README: status LED section and the provisioning LED table
