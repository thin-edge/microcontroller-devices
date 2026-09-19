## Context

Today `lib/common/net.c` `wifi_connect()` fills `wifi_connect_req_params` straight
from `CONFIG_APP_WIFI_SSID` / `CONFIG_APP_WIFI_PSK`. If the SSID is empty it logs
an error and the device stays offline for good. All three apps share the same
boot sequence in `main()`: `app_net_init()`, then
`app_net_wait_connected(60 s)`, then start the frontend. Connectivity recovery
(the reachability watchdog, robust reconnect and the last-resort reboot after
`APP_NET_REBOOT_TIMEOUT_S`) runs on a dedicated connectivity work queue. The
liveness watchdog (`APP_LIVENESS`, on by default for Wi-Fi) resets the device
when a watched context stops making progress.

What the targets have in common, confirmed against the Zephyr 4.4.2 tree:

- Every BLE-capable target (WROOM-32, QT Py S3, S3-DevKitC, C6-DevKitC) has a
  `storage_partition` from the Espressif `partitions_*` dtsi, and an `sw0`
  button alias. On every one of them `sw0` is the SoC's BOOT strapping pin
  (GPIO0, or GPIO9 on the C6).
- The BLE controller is driven by `drivers/bluetooth/hci/hci_esp32.c`, with
  Wi-Fi/BLE software coexistence available (`ESP32_SW_COEXIST_ENABLE`).
- Zephyr provides `subsys/net/lib/wifi_credentials`, a credential store with a
  settings backend.
- No board ships with Bluetooth, settings or NVS enabled today.
- Every image boots with Espressif **simple boot**: one image at flash offset
  0 (0x1000 on the classic ESP32), no second-stage bootloader, so there is
  nothing that could choose between two images. The Espressif partition
  dtsi already defines `slot0`/`slot1` (1792 KB each on 4 MB parts), a
  `scratch_partition` and `storage_partition`, i.e. an MCUboot-shaped layout
  that simple boot ignores.
- MCUboot v2.4.0 (Zephyr 4.4.2's pinned revision) runs on the Espressif SoCs.
  Its Espressif launcher (`do_boot()` → `start_cpu0_image()` →
  `esp_app_image_load()`) resolves the image's flash area through
  `flash_area_id_from_multi_image_slot()`, which `BOOT_FLASH_AREA_HOOKS` can
  override, and `BOOT_GO_HOOKS` lets project code replace the slot decision.

The WROOM-32 running OPC-UA is the tightest RAM budget in the fleet and has a
history of network stalls under load (see the esp32-network-freeze
investigation). Whatever this change adds must not put extra load on its data
path while it is serving.

**Iteration 1** built provisioning into each application image (behind an
overlay) and verified it end to end on the C6. It proved the protocol, the
credential test and the button gestures, and it also proved the cost:
Bluetooth compiled into an image takes its RAM in every boot, and on the WROOM
that is more than the image can spare ("Measured cost of in-app provisioning"
below). OTA is a must-have follow-up that needs A/B slots. This design keeps
everything iteration 1 got right and changes where it runs.

## Goals / Non-Goals

**Goals:**

- One image per board/app can be put on any WPA2-Personal or open network
  without a rebuild.
- Existing clients work unchanged (the Improv web page, Home Assistant), so we
  do not write or maintain an app.
- Application images carry **no Bluetooth code or RAM** at all; provisioning
  lives in its own image, so it costs the protocol frontends nothing and the
  WROOM-32 is supported.
- A flash layout that OTA can use as-is: A/B application slots with MCUboot's
  swap and revert, and a provisioning image that OTA never touches.
- Recoverable in the field with nothing but the board's button.

**Non-Goals:**

- Encrypted provisioning transport, BLE pairing or bonding, and flash
  encryption.
- Enterprise (EAP) networks, several stored networks, and configuring anything
  other than SSID/password.
- SoftAP, serial or USB provisioning.
- OTA download, update policy and production signing keys (the OTA change).
- Boards without BLE (ESP32-S2), the Pico W, and `native_sim`.

## Decisions

### D1: Improv Wi-Fi over BLE as the protocol

We implement the Improv Wi-Fi BLE service (service
`00467768-6228-2272-4663-277478268000`) as a GATT peripheral:

| Characteristic | UUID suffix | Props | Use |
|---|---|---|---|
| Current state | `…8001` | read, notify | 0x01 authorization required, 0x02 authorized, 0x03 provisioning, 0x04 provisioned |
| Error state | `…8002` | read, notify | 0x00 none, 0x01 invalid RPC, 0x02 unknown RPC, 0x03 unable to connect, 0x04 not authorized, 0xFF unknown |
| RPC command | `…8003` | write | `0x01` send Wi-Fi settings, `0x02` identify |
| RPC result | `…8004` | read, notify | the redirect URL list after a successful provisioning |
| Capabilities | `…8005` | read | bit 0 is set when an identify LED exists |

Advertising carries the service UUID and Improv service data (UUID `0x4677`,
with the state and capability bytes), so clients can filter for it. RPC frames
are `[cmd][len][payload][checksum]`, where the checksum is the byte sum mod 256.
The device reassembles a frame across several writes when it exceeds one ATT
write, using `len` to know when the frame is complete. It rejects a bad checksum
or an over-long SSID (>32 bytes) or password (>64 bytes) with error 0x01.

- *Alternative: Espressif unified provisioning (protocomm, protobuf,
  Security1 with proof-of-possession).* It encrypts the credentials, which is
  its real advantage, and the ESP BLE Provisioning apps speak it. But it is an
  ESP-IDF component with no Zephyr port. Porting it pulls in protocomm, nanopb
  and X25519/AES-CTR session code. That is far more code and flash for a Phase
  1/2 convenience feature. We note it as the upgrade path if encrypted
  provisioning becomes a requirement.
- *Alternative: a bespoke GATT service.* Smallest possible code, but nothing
  could talk to it without a client we would have to write and maintain.
- *Alternative: SoftAP with a captive portal.* Needs AP mode, a DHCP server
  and an HTTP server. That costs more RAM than BLE on the WROOM, and it takes
  the Wi-Fi radio away from the station test it has to run.

### D2: Provisioning is a separate image in its own partition

The device runs one of two images, never both:

- **The application** (OPC-UA, Modbus or SNMP) from `slot0`. Built without
  `CONFIG_BT`. It reads credentials, serves, and watches the button.
- **The provisioner** (`apps/wifi-provisioner`) from the `prov` partition. It
  has Bluetooth, Wi-Fi (to test credentials) and the credential store, and no
  protocol frontend. One provisioner per board serves every application; it
  names itself after the device's unique hostname and reports the service URL
  of the application it will hand back to, which it learns from a small
  record the application writes into `storage` (D9).

Every switch between them is a reboot through MCUboot (D9). The Improv service,
state machine and credential test written in iteration 1
(`lib/common/provisioning.c`) move into the provisioner essentially
unchanged; the provisioning-mode branches in `net.c` (suppressed recovery,
`app_net_try_credentials()`) are only compiled into the provisioner.

- *Alternative: provisioning inside the application (iteration 1).* Verified
  working, but costs 67–107 KB of heap on the S3/C6 and cannot link on the
  WROOM. Rejected.
- *Alternative: MCUboot firmware-loader mode* (one app slot plus a loader that
  does provisioning and OTA). Leaner still, but no A/B rollback for OTA.
  Rejected by the product owner: OTA with rollback is required.
- *Alternative: provisioning image in `slot1`, reached with a test-swap that
  MCUboot reverts.* Needs no hooks, but occupies the OTA slot. Rejected.

### D3: How the device enters provisioning mode

The mode is decided at boot, in this order:

1. **MCUboot** checks the boot request (D9). If it asks for provisioning, it
   launches the provisioner.
2. Otherwise it boots the application normally (A/B logic included). If the
   **application** finds no credentials (see D4), it sets the boot request and
   reboots, so a fresh device reaches the provisioner after one extra boot.
3. Otherwise the application serves.

At runtime in station mode, a small `sw0` monitor (a debounced GPIO
interrupt plus a k_work timer on the connectivity queue) recognizes two
gestures:

- **Provisioning pattern:** exactly `APP_WIFI_PROV_PRESS_COUNT` (default 3)
  short presses, each shorter than 1 s, all completed within
  `APP_WIFI_PROV_PRESS_WINDOW_MS` (default 2000 ms), with no further press for
  about 700 ms afterwards. The application then sets the boot request and
  reboots into the provisioner. Stored credentials are kept.
- **Erase:** one continuous hold of at least `APP_WIFI_PROV_ERASE_HOLD_S`
  (default 10 s). On release the application deletes all stored credentials,
  sets the boot request, and reboots. The LED switches to a fast flicker once the hold
  passes the threshold, so the operator knows the erase is armed before
  letting go.

Anything else does nothing: a single press, two presses, too many presses,
presses spread beyond the window, or a hold shorter than the erase threshold.
A pattern is hard to trigger by accident (a knock, someone leaning on the
board, a bouncy contact), and it is easy to describe in a README. When the
pattern is recognized the LED fast-blinks for about 1 s before the reboot, so
the operator knows it registered.

**No automatic fallback.** Stored credentials that stop working, for example
on a relocated device or after an access point password change, never put the
device into provisioning mode by themselves. The device keeps reconnecting and
doing its last-resort reboots, exactly as today. Provisioning mode is entered
only through the button pattern, or when no credentials resolve at all. That
way the BLE link, which is not encrypted, is never opened without someone
physically at the device, or on a device that has never been configured.

- *Alternative: hold the button through reset.* This is impossible on these
  boards: `sw0` is the BOOT strapping pin, so holding it through reset enters
  the ROM download mode.
- *Alternative: a press window of a few seconds at every boot.* This adds that
  delay to every boot and watchdog reset, and it still races the strapping pin
  on a power-on reset. Rejected.
- *Alternative: a single long-press (e.g. 3 s) to enter provisioning mode.*
  Simpler, but too easy to trigger by accident, and too close to the erase
  hold. A multi-press pattern is an unmistakably different gesture from the
  erase hold. Rejected.
- *Alternative: fall back to provisioning after K failed boots.* This helps a
  relocated device, but it would advertise the unencrypted provisioning service
  with nobody present, on a device an attacker could then move onto their own
  network. Rejected.

### D4: Credential storage and precedence, using upstream `wifi_credentials`

We use Zephyr's `wifi_credentials` library with
`WIFI_CREDENTIALS_BACKEND_SETTINGS`, on `SETTINGS_NVS` over `storage_partition`,
with `WIFI_CREDENTIALS_MAX_ENTRIES=1`. We turn off
`WIFI_CREDENTIALS_CONNECT_STORED` and `WIFI_CREDENTIALS_SHELL`. `net.c` keeps
ownership of connecting and reconnecting, because its robust reconnect logic is
the verified behaviour, and we do not want a second connect path competing with
it.

`net.c` gains `app_wifi_creds_resolve(struct app_wifi_creds *out)`, which
returns the credentials from the first source that has any:

1. The stored entry (the first and only SSID from
   `wifi_credentials_for_each_ssid`, then `…_get_by_ssid_personal`).
2. Compile-time `CONFIG_APP_WIFI_SSID` / `PSK`, if the SSID is non-empty.
3. None.

`wifi_connect()` becomes `wifi_connect(const struct app_wifi_creds *)`. The
application passes it the resolved credentials, and the provisioner passes it
the candidate. Both images link the same `wifi_credentials` settings backend
against the same `storage_partition`, so the format is shared by construction;
the application only reads (and, for the erase gesture, deletes). The security type is PSK when the password is non-empty and NONE
otherwise, exactly as today. Saving new credentials replaces the old entry:
delete all, then `wifi_credentials_set_personal`. Stored credentials beat
compile-time ones, so a device built with a baked-in default can still be moved
to another network in the field.

- *Alternative: our own settings keys (`wifi/ssid`, `wifi/psk`).* Only
  marginally smaller, and it duplicates an upstream module. Upstream also gives
  us a PSA backend later if encrypted storage becomes a goal.
- *Alternative: raw NVS.* This loses the settings abstraction and gains
  nothing.

Writing an application image into `slot0` (or, later, an OTA image into
`slot1`) does not touch `storage_partition`, so credentials survive firmware
updates. `esptool erase-flash` clears them.

### D5: Provisioning mode state machine

```
            ┌───────────────── window expired, creds exist ─────────────┐
 boot ──► READY (advertising) ──RPC 0x01──► TESTING ──IPv4 ok──► PROVISIONED ──► reboot
            ▲   │ (auth req'd: AUTH_REQUIRED ──short press──► READY/authorized for 60 s)
            │   │                              │
            │   └─ window expired, no creds ──► IDLE (not advertising; short press ► READY)
            └──────────── connect failed / timeout (error 0x03) ─────┘
```

- **Leaving:** every exit clears the boot request first, then reboots, so
  MCUboot boots the application.
- **Window:** advertising lasts `APP_WIFI_PROV_WINDOW_S` (default 900 s). When
  it expires and credentials resolve (the device entered by the button
  pattern), the provisioner clears the request and reboots back into the
  application on its old network. When nothing
  resolves, it stops advertising and goes **IDLE**. A short press of `sw0`
  restarts the window. Nobody is ever locked out, and an unattended fresh
  device does not advertise forever.
- **Authorization** (`APP_WIFI_PROV_REQUIRE_AUTH`, default `n`): when enabled,
  the device starts in state 0x01. RPC 0x01 is refused with error 0x04 until
  someone presses `sw0` on the device, which authorizes it for 60 s (the Improv
  convention). This proves physical presence, which matters because the link
  is not encrypted.
- **Testing:** the device issues the station connect with the candidate
  credentials and waits for an IPv4 address for up to
  `APP_WIFI_PROV_CONNECT_TIMEOUT_S` (default 30 s). On failure it disconnects,
  notifies error 0x03, and returns to READY. The old stored credentials are
  untouched.
- **Provisioned:** the device saves the credentials, notifies state 0x04, and
  sets RPC result to `<scheme>://<hostname>.local:<APP_DNSSD_PORT>`. The scheme
  follows `APP_DNSSD_SERVICE_TYPE`: `opc.tcp` for OPC-UA, and `modbus` or
  `snmp` for the others. It then waits until the client disconnects, or up to
  5 s, so the result is delivered, and reboots.
- **Identify** (RPC 0x02): the device fast-blinks `led0` for 10 s.
  Capabilities bit 0 is set only when `led0` exists.
- **No serving-side recovery in the provisioner:** it has no connectivity
  watchdog reconnects and no last-resort reboot (waiting for credentials is
  "offline" by design). It keeps the liveness watchdog, so a wedged BLE or
  Wi-Fi driver still resets the device; after such a reset the boot request is
  still set, so the device comes back into the provisioner.
- **One central at a time:** `BT_MAX_CONN=1`. Advertising resumes when a
  client disconnects without finishing.
- **Name and URL:** the BLE device name is the unique hostname the application
  will use, and the RPC result URL is the application's service URL. Both come
  from the application's identity record in `storage` (D9), because the
  provisioner is shared by all apps and cannot know them at build time. With no
  record yet (the application never ran), it falls back to a generic
  `tedge-prov<mac>` name and returns no URL.

### D6: Status LED patterns

`status_led` gains a mode API:

| Mode | Pattern | When |
|---|---|---|
| `CONNECTED` | steady on | station mode, IPv4 assigned (as today) |
| `DISCONNECTED` | 250 ms on / 250 ms off | station mode, not connected (unchanged) |
| `PROVISIONING` | **two short blinks, then a pause**: 100 ms on, 150 ms off, 100 ms on, ~1.5 s off, repeating | provisioning mode, advertising (READY, AUTH_REQUIRED, TESTING) |
| `IDENTIFY` | fast 5 Hz blink | identify RPC, and the ~1 s acknowledgement of the button pattern |
| `ERASE_ARMED` | very fast flicker (~10 Hz) | the erase hold has passed its threshold |
| `OFF` | off | provisioning IDLE (window expired) |

The provisioning pattern is built so it cannot be mistaken for the other two
states an operator sees most often. It is mostly off, so it never looks like
steady on. It has a rhythm, a pair of blinks and then a gap, which the even
even 250 ms "not connected" blink does not have. The timings are constants in
`status_led.c`, not Kconfig options, so every device in the field looks the
same. `status_led_set_connected(bool)` stays as a
wrapper around the mode API, so existing callers do not change. Boards without
`led0` stay a no-op.

### D7: Packaging

- **Application images** (all three apps) gain `wifi_credentials`, settings
  and NVS, the `sw0` gesture and the boot-request helper, all small. They do
  not gain Bluetooth. With MCUboot enabled for a board, stored credentials take
  precedence over compile-time ones as before.
- **The provisioner** (`apps/wifi-provisioner/`) is its own Zephyr application:
  `prj.conf` carries the Bluetooth, coexistence and credential options that
  iteration 1 put in `overlay-ble-provisioning.conf` (which is removed), plus
  `lib/common` for Wi-Fi bring-up, identity and the status LED.
- **Sysbuild** builds all three images in one `west build --sysbuild`: MCUboot
  (with the hooks, `lib/mcuboot-hooks/`), the signed application, and the
  signed provisioner as an extra image (`sysbuild/provisioning.cmake`, included
  from each app's `sysbuild.cmake`). The provisioner is the same image whichever
  app it is built with, so any build's copy can go into `prov`. *Changed from
  the first draft, which built the provisioner separately: one build keeps the
  three images on the same layout and key by construction.*
- **Configuration follows the layout.** The shared layout
  (`lib/common/dts/layout-*.dtsi`) is included by the app board overlays, so a
  plain build of those boards has a `bootreq_partition` too. The credential
  store (`APP_WIFI_CRED_STORE`) defaults on wherever that partition exists; the
  hand-off (`APP_PROV_HANDOFF`) only in an MCUboot build that also has `prov`.
  A plain build therefore reads stored credentials but never hands off.
- The ESP32-S2 Feather stays on simple boot with compile-time credentials.

### D8: Flash partition layout

A shared devicetree overlay per flash size replaces the upstream `partitions`
node for the BLE boards. The 4 MB layout (WROOM-32 at a 0x1000 bootloader
offset, C6 at 0x0) is the tight one:

| Partition | Size | Use |
|---|---|---|
| `boot_partition` | 64 KB | MCUboot |
| `sys_partition` | 64 KB | unchanged from upstream |
| `slot0_partition` | ~1280 KB | application, primary |
| `slot1_partition` | ~1280 KB | application, secondary (reserved for OTA) |
| `prov_partition` | ~1024 KB | provisioner |
| `storage_partition` | 128–192 KB | settings/NVS: credentials, identity record |
| `bootreq_partition` | 4 KB | boot request (D9) |
| `scratch_partition` | 124 KB | MCUboot swap scratch |
| `coredump_partition` | 4 KB | unchanged |

The C6 layout drops the upstream LP-core slots (unused). The implemented
layouts (`lib/common/dts/layout-esp32c6-4M.dtsi`, `layout-esp32s3-16M.dtsi`):

| Partition | ESP32-C6, 4 MB | ESP32-S3-DevKitC-1, 16 MB |
|---|---|---|
| `boot_partition` | 0x000000, 64 KB | 0x000000, 64 KB |
| `sys_partition` | 0x010000, 64 KB | 0x010000, 64 KB |
| `slot0`/`slot1` | 0x020000/0x160000, 1280 KB | 0x020000/0x320000, 3072 KB |
| `prov_partition` | 0x2a0000, 1024 KB | 0x620000, 2048 KB |
| `bootreq_partition` | 0x3a0000, 4 KB | 0x820000, 4 KB |
| `storage_partition` | 0x3b0000, 192 KB (upstream offset) | 0x830000, 192 KB |
| `scratch_partition` | 0x3e0000, 124 KB | 0x860000, 128 KB |
| `coredump_partition` | 0x3ff000, 4 KB | 0x880000, 4 KB |

The S3 layout replaces the upstream AMP layout (APP-CPU and LP-core slots
unused). The QT Py S3 and the WROOM-32 have no layout yet.

- *Alternative: keep 1792 KB app slots and shrink only `prov`.* Does not fit
  in 4 MB with a provisioner of any realistic size.

### D9: Boot request and MCUboot hooks

The **boot request** is one record in its own 4 KB flash partition:
`{magic, target, reason, reserved}` (four little-endian words,
`lib/common/boot_request.h`, shared with the hooks) where
`target = PROVISIONER` and `reason` says whether the application had no
credentials or the operator asked (the button pattern). The provisioner uses
the reason at the end of its window: an operator request returns to the
application, which may be running on compile-time credentials that the
provisioner cannot see. Erased flash (`0xFF`) means
"no request". A raw flash record, not a settings key, because MCUboot must read
it without linking settings or NVS, and not retained RAM, because a device
that loses power mid-provisioning should come back into the provisioner.

MCUboot is built with `BOOT_GO_HOOKS`, `BOOT_FLASH_AREA_HOOKS`,
`MCUBOOT_ACTION_HOOKS` and `BOOT_VALIDATE_SLOT0` (the Espressif SoC defaults
turn slot0 validation off; the boot-layout spec requires it), and project hook
sources:

- `boot_go_hook()`: if the request is set, validate the image in `prov`
  (header magic and signature, with the same key as the application) and fill
  the boot response with it; otherwise return `FIH_BOOT_HOOK_REGULAR` so the
  normal A/B path runs, swap and revert included. If `prov` is invalid, log it
  and fall back to the application.
- `flash_area_id_from_multi_image_slot_hook()`: while a provisioner launch is
  in progress, map the slot the Espressif launcher asks for to `prov`. The
  launcher only distinguishes primary and secondary, so without this redirect
  it would load `slot1`. (The launcher's own log line therefore reads
  "Loading image 0 - slot 1" when it loads `prov`.)
- `mcuboot_status_change()`: MCUboot's own log is compiled out on Espressif,
  so a refused application would boot to silence; this prints a line when no
  application is bootable. The hooks print with `esp_rom_printf`.

The application also writes a small **identity record** into `storage`
(unique hostname, DNS-SD service type and port; settings key `prov/ident`,
`lib/common/prov_identity.c`) once it is online, and only when it changed, so
the provisioner can advertise the right name and URL (D5).

- *Alternative: MCUboot multi-image with the provisioner as image 1.*
  Multi-image means images that run together, and the Espressif `do_boot()`
  hard-codes image 0. Rejected.
- *Alternative: fork MCUboot's Espressif launcher.* The hooks cover it without
  a fork; revisit only if the spike shows they do not.

### D10: Signing and flashing

Images are signed by imgtool with MCUboot's development key during the build.
The README states plainly that this key is public and not for production; the
OTA change owns key management. `scripts/flash.sh <build-dir>` flashes the
bootloader, the application (`slot0`) and the provisioner (`prov`) at the
offsets read from the build's devicetree, and can erase `storage` and
`bootreq` (`--erase-storage`), erase the chip first (`--erase-all`), or write
only the application (`--app-only`).

## Spike and implementation results (task groups 1–7)

Measured on Zephyr 4.4.2 / MCUboot v2.4.0, 2026-09-18, on the ESP32-C6
(WROOM-1-N4) and the ESP32-S3-DevKitC-1 (N16R8).

**The hooks are enough (D9).** No `do_boot()` patch is carried. On both
boards MCUboot launches the provisioner from `prov` when the request is set,
boots the application when it is clear, and falls back to the application
with a console line when `prov` is erased ("no provisioner image") or its
signature fails ("provisioner image invalid").

**Swap and revert are unaffected.** On the C6 an image signed as 0.1.1 with
`--pad` written to `slot1` was swapped into `slot0` on the next boot and, not
being confirmed, reverted on the one after (headers read back: 0.1.1/0.1.0,
then 0.1.0/0.1.1). `prov` and `storage` were byte-identical before and after.

**MCUboot cost.** MCUboot is ~46 KB (of its 64 KB partition) on both boards.
Validating `slot0` on every boot is software ECDSA-P256 plus SHA-256 over the
image: on the S3, reset to application start is ~150 ms without it and
~700 ms with it (Modbus, 582 KB). Acceptable for these devices; hardware
SHA in MCUboot is a possible later improvement.

**Sizes.**

| Board | Image | Signed size | Partition | libc heap (before this change) |
|---|---|---|---|---|
| ESP32-C6 | Modbus | 732 KB | 1280 KB | 298,528 B (291,664) |
| ESP32-C6 | OPC-UA | 839 KB | 1280 KB | 256,384 B (249,568) |
| ESP32-C6 | SNMP | 731 KB | 1280 KB | 268,224 B (261,408) |
| ESP32-C6 | provisioner | 943 KB | 1024 KB (92 %) | 216 KB |
| ESP32-S3-DevKitC-1 | Modbus | 582 KB | 3072 KB | 200,588 B (197,852) |
| ESP32-S3-DevKitC-1 | SNMP | 581 KB | 3072 KB | 170,372 B (167,636) |
| ESP32-S3-DevKitC-1 | provisioner | 662 KB | 2048 KB | 159 KB |

Every application has at least its pre-change heap (iteration 1 left the C6
Modbus build 185,008 B). The MCUboot builds even have ~7 KB (C6) and ~3 KB
(S3) more; where that comes from was not investigated. The C6 provisioner is the
tight one at 92 % of `prov`.

**Found during implementation.**

- *BLE link drops during the credential test.* On the C6 one attempt in three
  lost the link to a macOS central with a supervision timeout (0x08) while the
  station was associating: the credentials were stored and the device moved
  on, but the client never saw the result. The provisioner now asks for a 5 s
  supervision timeout (interval 30–60 ms, within Apple's accessory limits)
  after a client connects; 4 of 4 re-provisioning runs then returned the URL,
  and the S3 log shows the central accepting the parameters.
- *C6 hung after leaving the provisioner.* `sys_reboot()` on Espressif is a
  CPU reset (`rst:0xc SW_CPU`). Taken from the provisioner, with Bluetooth and
  Wi-Fi running, it left the C6 stuck in MCUboot's start-up (after
  `flash_init`, before MCUboot's `main()`) until the next power cycle or USB
  reset: provisioning succeeded but the device never came back. Reboots that
  hand over between images now use a full digital-system reset
  (`boot_request_reboot()`, `esp_rom_software_reset_system()`,
  `rst:0x3`), as does the liveness watchdog in images with Bluetooth; 3 of 3
  provision-and-reboot runs then came back serving in 4–8 s.
- *Deferred logging and reboots.* Log lines written just before a reboot were
  lost (the log thread wakes every second). Every reboot path now calls
  `log_flush()` first.

**Classic ESP32 and QT Py S3 (2026-09-19, boards on a Raspberry Pi 5,
provisioned with the Pi's BlueZ adapter).** Layouts `layout-esp32-4M.dtsi`
(MCUboot at 0x1000) and `layout-esp32s3-4M.dtsi` (same offsets as the C6).
MCUboot, the hooks and the hand-off work on the classic ESP32 unchanged. One
real problem: Zephyr disables `ESP32_REGION_1_NOINIT` for MCUboot builds, so
`.noinit` (~68 KB of stacks and net buffers) moved into DRAM region 0 and the
WROOM OPC-UA app booted with a 3 KB libc heap (71,696 B under simple boot),
and the provisioner overflowed `dram0` by 44 KB. `.noinit` is not loaded, so
MCUboot is done with SRAM1 by the time the application touches it; setting
`CONFIG_ESP32_REGION_1_NOINIT=y` in the WROOM board confs restores the app
heap (73,488 B) and lets the provisioner link with the default Wi-Fi and
Bluetooth heap shares (dram0 91 %). Verified: two WROOMs (OPC-UA) and an
ESP32-D0WD-V3 board (SNMP) provisioned and read on the data path; the QT Py
S3 (Modbus) provisioned, then re-provisioned twice, back serving in 4–6 s.
The first QT Py provisioning after a fresh flash was not serving within 40 s
(pingable); not reproduced since, cause unknown.

**WROOM OPC-UA churn (task 7.7): passes.** 300 connect → session → browse →
disconnect cycles (asyncua, 8 s timeout, from a Mac over Wi-Fi) per board, with
each board's console captured on the Pi for the whole run:

| | WROOM `30aea4e87ee0` | WROOM `3c71bf10c2e4` |
|---|---|---|
| Client OK | 285 / 300 | 300 / 300 |
| Connections seen by the device | 288 | 303 |
| Net-buffer allocation failures | 0 | 0 |
| Listen-socket closures | 0 | 0 |
| Liveness stalls | 0 | 0 |
| Free heap, start → end | 22,844 → 22,536 B | 22,844 → 22,808 B |
| RSSI (mostly) | −75/−76 dBm | −70 dBm |

The first board's 15 timeouts came with no device-side error, and about 12 of
those connections never reached it. The same firmware on the board with the
better signal had none, so these are Wi-Fi-link losses, not the MCUboot build.
(A first run on the first board, with only a third of it on the console, had
23 timeouts at a similar RSSI.)

## Risks / Trade-offs

- **The MCUboot hooks may not be enough to launch a third partition on the
  Espressif port** (D9). → Task 1 is a spike on the C6 that proves it with a
  trivial image before anything else is built. If the hooks fall short, the
  fallback is a small patch to the Espressif `do_boot()` carried in this repo.
- **The provisioner may not fit ~1 MB on the 4 MB boards.** → The spike
  measures it. It needs Wi-Fi, IPv4/DHCP and BLE, but no protocol stack, no
  mDNS and minimal logging.
- **MCUboot on the WROOM-32** (classic ESP32, bootloader at 0x1000) is less
  travelled than on the C6/S3. → The spike repeats the boot flow on a WROOM
  before it is claimed.
- **Flashing gets harder** (three images, signed). → One helper script, and
  the README keeps a single copy-paste command per board.
- **Boot-time cost** of MCUboot's signature check on every boot. → Measured in
  the spike; acceptable if it is a few hundred ms.
- **Plaintext credentials over the BLE link.** Anyone in radio range during an
  active provisioning session can sniff the password. → The window is bounded,
  the mode is entered only when there are no credentials or after the
  deliberate button pattern (never automatically), the optional physical-presence authorization is available, the
  README documents the caveat, and D1 names encrypted unified provisioning as
  the upgrade path.
- **A hostile client provisions an unconfigured device onto its own network.**
  → The same mitigations apply, and the operator can always re-provision with
  the button pattern. The impact is limited: the device serves demo/simulated data
  to whoever is on that network.
- **Credentials sit unencrypted in flash** and can be read with esptool by
  anyone with physical access. → This is documented. It is no worse than
  today, where they sit in the image. Flash encryption is a non-goal.
- **Wi-Fi/BLE coexistence during the credential test** can slow or fail the
  association, which would look like wrong credentials. → The 30 s connect
  timeout is generous, the failure is reported as error 0x03 and can be
  retried, and coexistence is enabled. We verify on all four boards.
- **Settings/NVS writes are flash erase and write operations,** which on ESP32
  can stall the other core and the Wi-Fi blob. → Credential writes happen only
  in the provisioner or on the erase gesture. The application's identity
  record is written only when it changes, not on every boot.
- **Moving a device to a new network requires someone at the device** to
  press the button pattern (stored credentials that stop working cause
  reconnects and last-resort reboots, not provisioning). → This is deliberate
  (see D3). The README documents the pattern, and the LED confirms that it
  registered.
- **Boards without `sw0`** cannot be re-provisioned in the field. They can only
  be provisioned while they have no credentials, or after
  `esptool erase-region` of `storage_partition`. → All four target boards have
  `sw0`. This case is documented for future ports.

## Measured cost of in-app provisioning (iteration 1)

Built with `scripts/measure_prov.sh` on Zephyr 4.4.2. Heap is the libc arena
left for the frontend (`_libc_heap_size`, printed at boot as "libc heap
size"); image is `zephyr.bin`.

| Board | App | Image base → prov (B) | Heap base → prov (B) |
|---|---|---|---|
| WROOM-32 | OPC-UA | 782,336 → link fails | 71,696 → `dram0_0_seg` over by 19,680 |
| WROOM-32 | Modbus | 585,728 → 847,872 | 95,000 → 3,624 |
| WROOM-32 | SNMP | 585,728 → link fails | 62,896 → `dram0_0_seg` over by 28,480 |
| QT Py S3 | OPC-UA | 751,076 → 840,756 | 155,932 → 88,764 |
| QT Py S3 | Modbus | 580,468 → 735,700 | 197,988 → 130,828 |
| QT Py S3 | SNMP | 579,156 → 734,372 | 167,756 → 100,548 |
| S3-DevKitC-1 | OPC-UA | 750,916 → 840,580 | 155,812 → 88,660 |
| S3-DevKitC-1 | Modbus | 580,308 → 735,540 | 197,852 → 130,724 |
| S3-DevKitC-1 | SNMP | 578,980 → 734,212 | 167,636 → 100,412 |
| C6 | OPC-UA | 828,848 → 1,175,072 | 249,568 → 142,944 |
| C6 | Modbus | 723,632 → 1,004,480 | 291,664 → 185,008 |
| C6 | SNMP | 710,000 → 990,896 | 261,408 → 154,688 |

Provisioning costs ~67 KB of heap on the S3, ~107 KB on the C6 and ~91 KB on
the WROOM, and 90–350 KB of flash (every image still sits well inside its
slot; the largest, C6 OPC-UA, uses 64% of 1792 KB).

**Where the WROOM RAM goes.** A symbol diff of the WROOM Modbus images shows
~30 KB of new BLE host pools and thread stacks (HCI RX pool 5.6 KB, controller
stack 4 KB, ATT and ACL TX pools ~5 KB, host thread stacks ~3.5 KB, ...) and
~3 KB of controller BSS. On top of that the ESP32 linker script moves the start
of `dram0_0_seg` up by `CONFIG_ESP32_BT_RESERVE_DRAM` (0xdb5c, 56,156 B) for
the controller whenever `CONFIG_BT` is set. That reservation is fixed at link
time: Zephyr has no equivalent of ESP-IDF's
`esp_bt_controller_mem_release()`, so it cannot be handed back to the heap in a
station-mode boot that never enables Bluetooth. Shrinking the host buffers
(65-byte MTU, 3 ACL TX buffers, 6 event buffers) recovered 256 B.

**Consequence.** In-app provisioning is not viable on the WROOM-32 and is
expensive everywhere else. This is why D2 moves Bluetooth into its own image:
the application images go back to their "base" column, and only the
provisioner, which runs nothing else, pays the Bluetooth cost.

**No LED on the supported boards.** Only the WROOM has a plain-GPIO `led0`.
The C6 clone, S3-DevKitC-1 and QT Py S3 carry addressable RGB LEDs that
`status_led.c` does not drive, so the provisioning pattern (D6) is implemented
but, today, visible on no board that can provision. Driving those LEDs is a
follow-up (see Open Questions).

## Migration Plan

- **Existing devices** run simple boot and must be reflashed once over serial
  (bootloader, application, provisioner). There is no in-field path from
  simple boot to MCUboot; that is acceptable at this stage.
- The ESP32-S2 and `native_sim` builds do not change.
- **Rollback:** build the application without sysbuild/MCUboot and flash it at
  offset 0 as today. Credentials left in `storage` are ignored by an image
  without the credential store.

## Open Questions

- ~~Can the classic-BT controller memory be released on the original ESP32?~~
  No: the 55 KB reserve is link-time. Answered by iteration 1, and the reason
  for the separate provisioner.
- Should MCUboot also fall back to the provisioner when `slot0` holds no
  valid application (e.g. a board flashed with only bootloader + provisioner
  at the factory)? Cheap to add in `boot_go_hook()`; decide with the OTA
  change.
- The provisioning LED pattern is only useful with a board LED. Should
  `status_led.c` learn to drive the addressable RGB LEDs (WS2812 via
  `led_strip`) that the C6, S3-DevKitC-1 and QT Py S3 carry? A pixel could also
  use colour (e.g. blue for provisioning) on top of the blink pattern.
- Should Improv-over-serial (ESP Web Tools) be offered for boards without BLE,
  such as the S2? We leave this to a separate change.
