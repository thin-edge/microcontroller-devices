## Why

Wi-Fi credentials are currently compiled into the image (`CONFIG_APP_WIFI_SSID` /
`CONFIG_APP_WIFI_PSK` from a git-ignored overlay), so every network needs its own
build and every image carries a plaintext password. That breaks the
"ready-to-use, easy to flash" goal: one release image cannot be flashed onto a
fleet of devices and then pointed at whatever network each one is installed on.
Every BLE-capable board we ship (ESP32-WROOM-32, both ESP32-S3 boards, ESP32-C6)
has a BLE radio that sits unused. Using it to hand the device its credentials at
install time lets one image serve every network.

A first implementation put BLE provisioning inside each application image. It
worked end to end on the ESP32-C6, but compiling Bluetooth into an image costs
RAM in every boot, including the ones that never use it: 67–107 KB of heap on
the S3 and C6, and on the WROOM-32 a 55 KB link-time controller reservation
that stops the OPC-UA and SNMP images from linking at all (see design.md,
"Measured cost of in-app provisioning"). OTA firmware update is also a
must-have that is coming next, and it needs a place in the flash layout. So
this change moves provisioning into its **own image in its own flash
partition**, booted by MCUboot only when needed, and fixes a partition layout
that leaves A/B slots free for OTA.

## What Changes

- **Bootloader and partition layout.** Boards switch from Espressif "simple
  boot" (one image at offset 0) to **MCUboot**, with three image partitions:
  - `slot0` / `slot1`: the application's A/B slots. `slot1` is **reserved for
    OTA** (the follow-up change); in this change it is only laid out and left
    empty. MCUboot's normal swap and revert logic applies to this pair.
  - `prov`: a dedicated partition for the **provisioning image**. OTA never
    writes it.
  - `storage`: the existing settings/NVS partition, now **shared** by the
    application and the provisioning image. It holds the Wi-Fi credentials and
    later other secrets.
- **A separate provisioning image** (`apps/wifi-provisioner`) holds everything
  Bluetooth: the Improv Wi-Fi BLE service, credential testing, the provisioning
  window and the optional authorization. It is protocol-agnostic, so one
  provisioner per board serves all three applications.
- **Application images carry no Bluetooth.** They keep only the credential
  store (read), the `sw0` button gesture, and a way to ask the bootloader for
  the provisioning image. The WROOM-32 is back in scope for all three apps, and
  the S3 and C6 get their heap back.
- **Boot selection.** A small persistent **boot request** tells MCUboot to boot
  the provisioning image instead of the application. MCUboot hooks read it and
  launch the `prov` partition. The request is set by:
  - the application, when it finds no Wi-Fi credentials, or
  - the application, after the button pattern (three short presses within 2 s),
    or after the 10 s erase hold (which also erases the stored credentials).
  The provisioning image clears the request, then reboots into the application,
  when it has stored verified credentials, or when its window expires on a
  device that still has credentials.
- **Unchanged from the first iteration:** the Improv Wi-Fi protocol, testing
  credentials before storing them, credential precedence (stored, then
  compile-time), no automatic fallback into provisioning, the button gestures,
  the LED patterns, and the plaintext-link caveat.
- **Image signing.** MCUboot requires signed images. This change uses a
  development key and documents that it is not for production; key management
  belongs to the OTA change.
- **Flashing changes** from one `write-flash 0x0` to a bootloader plus two
  signed images, built with sysbuild. The README and a helper script cover it.

Target boards: `esp32_devkitc/esp32/procpu` (WROOM-32),
`adafruit_qt_py_esp32s3/esp32s3/procpu`, `esp32s3_devkitc/esp32s3/procpu`,
`esp32c6_devkitc/esp32c6/hpcore`. The Feather ESP32-S2 TFT has **no BLE radio**
and stays on compile-time credentials and simple boot. `native_sim` and the
Pico W are out of scope.

Protocols affected: all three apps (OPC-UA, Modbus TCP, SNMP), through
`lib/common/`. No frontend behaviour changes.

**Phase:** 1/2 enabling work (deployability of the source firmware). Nothing
here touches thin-edge.io or Cumulocity (Phase 3).

### Resource constraints

- **RAM:** the application image must not grow by more than the credential
  store and button code (a few KB). Bluetooth RAM is paid only by the
  provisioning image, which runs nothing else.
- **Flash (4 MB boards: WROOM-32 and C6):** the bootloader, two app slots, the
  provisioning partition, storage and scratch must fit in 4 MB. App slots
  shrink from 1792 KB to about 1280 KB each (C6 OPC-UA is 829 KB today,
  WROOM OPC-UA 782 KB), and the provisioning partition gets about 1 MB. The
  provisioning image has to fit that; the spike measures it. The 8 MB and
  16 MB S3 boards have ample room.
- **Boot time:** MCUboot adds a signature check on every boot; the spike
  measures it.

## Non-goals

- **OTA itself.** This change lays out `slot1` and keeps MCUboot's swap/revert
  path working, but downloading, signing policy and triggering updates are the
  follow-up OTA change.
- A custom mobile app or a bespoke GATT protocol. We use Improv's.
- Provisioning over any other transport (SoftAP + captive portal, serial
  Improv, USB).
- Encrypting credentials in flash, BLE pairing or bonding, and production key
  management.
- Configuring anything other than Wi-Fi SSID/password over BLE.
- Updating the provisioning image in the field (it is flashed at manufacture
  or over serial).
- Bluetooth on the ESP32-S2, the Pico W, or `native_sim`.
- Phase 3 thin-edge.io / Cumulocity onboarding.

## Capabilities

### New Capabilities

- `wifi-provisioning`: the provisioning image and its BLE provisioning mode
  over Improv Wi-Fi. Covers when the device boots into it and leaves it,
  testing credentials before storing them, the bounded window, optional
  authorization, persisting and erasing credentials, precedence, and the rule
  that Bluetooth never runs in an application image.
- `boot-layout`: MCUboot, the flash partition layout (A/B application slots
  reserved for OTA, the provisioning partition, shared storage), the boot
  request that selects the provisioning image, image signing, and flashing.

### Modified Capabilities

- `wifi-connectivity`: "Wi-Fi station bring-up" takes credentials from the
  persisted store, falling back to compile-time values; with none, the
  application requests the provisioning image instead of staying offline. The
  "Connectivity status indicator (LED)" requirement gains the provisioning
  patterns.

## Impact

- **Code:**
  - new `apps/wifi-provisioner/` (reuses the Improv service and state machine
    already written as `lib/common/provisioning.c`);
  - `lib/common/net.c`: credential resolution; the provisioning-mode hooks move
    to the provisioner;
  - `lib/common/button_gesture.c` and a boot-request helper, used by the apps;
  - MCUboot hook sources (boot selection and the flash-area redirect), built
    into the bootloader via sysbuild.
- **Devicetree:** a shared partition-layout overlay per flash size (4 MB, 8 MB,
  16 MB), included by each board's `.overlay`.
- **Config files:** per-app `sysbuild.conf` enabling MCUboot; the provisioner's
  own `prj.conf` carries the Bluetooth options now in
  `overlay-ble-provisioning.conf`, which goes away.
- **Dependencies:** MCUboot (added to the west allowlist), Zephyr Bluetooth and
  the `hci_esp32` driver (provisioner only), `wifi_credentials`, settings and
  NVS.
- **Flashing and docs:** new flash offsets and a flash helper script; README
  flash sections for every BLE board; the provisioning section and support
  matrix.
- **Security posture:** images contain no Wi-Fi secret. Credentials sit
  unencrypted in `storage`. Images are signed with a development key only.
