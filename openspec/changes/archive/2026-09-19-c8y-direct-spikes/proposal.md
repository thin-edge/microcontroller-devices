## Why

Phase 3 goal: **tedge-zephyr**, a thin-edge.io device-management client that
ships as a reusable Zephyr module, so that **any user's Zephyr application** can
include it. It should provide firmware update, remote access, telemetry, log
retrieval and configuration management. The OPC-UA, Modbus and SNMP apps in this
repo stand in for such user applications: they run their own workload, and the
client runs alongside it. The client will get two transports:

- **direct to Cumulocity**, which nothing supports today, so it comes first;
- **via a thin-edge.io gateway** as a child device. `rpi-pico-client` and
  `freertos-esp32-client` already prove that contract.

Before we commit to a design for the direct transport, a few unknowns could
change its shape:

- Can a 4 MB ESP32-C6 afford TLS to the Cumulocity **MQTT Service** (port
  9883)? Do its SmartREST and JWT paths behave as we expect?
- Does OTA into the already laid-out `slot1` work end to end?
- Can the device enroll itself with the **Cumulocity CA** (device-generated
  one-time password, on-device key, EST `simpleenroll`)?
- Can the device act as a **remote-access gateway**, tunnelling a Cumulocity
  Cloud Remote Access session to another host on its LAN (for example SSH to
  a machine next to it)? Users are expected to value this feature most, and
  the gateway transport can't offer it for child devices.

This change answers those questions with measurements before any production
code is written. It also sets the rule every later phase follows: **each
device-management feature is selected at build time**, so a board with little
RAM or flash carries only the features it can afford.

## What Changes

- **Spike A: TLS and MQTT to Cumulocity.** An ESP32-C6 opens MQTTS to the
  MQTT Service on 9883 (and, for comparison, to Core MQTT on 8883). It
  subscribes to the SmartREST topics, publishes an inventory message, and
  publishes one free-form telemetry message. It fetches a JWT via `s/uat`. We
  measure heap (steady and peak), flash, handshake time, the TLS record size
  the server needs, and reconnect behaviour after a Wi-Fi drop. Time comes
  from SNTP before certificates are validated.
- **Spike B: OTA into `slot1`, without the cloud.** The device streams a
  signed image over HTTP, then HTTPS, into `slot1`. It requests a test boot,
  and MCUboot swaps. The image confirms itself, or it is left unconfirmed and
  MCUboot must revert it. We check that `prov`, `storage` and `bootreq` are
  untouched, measure swap time, and record how HTTP redirects behave.
- **Spike C: Cumulocity CA enrollment on the device.**
  - The device generates a P-256 key in PSA secure storage, a one-time
    password and a pre-filled registration URL.
  - It builds a PKCS#10 CSR (CN = external ID) and polls
    `/.well-known/est/simpleenroll` with Basic `<id>:<otp>` until it gets a
    certificate.
  - It then connects to 9883 with mTLS and tries `simplereenroll`.
  - The bootstrap-user basic-auth path (`s/ucr` → `70,…`) is exercised as the
    fallback.
- **Spike D: module skeleton, feature selection and footprint.**
  - Create `tedge-zephyr/`: a self-contained Zephyr module in its own
    top-level directory (`zephyr/module.yml`, `Kconfig`, `CMakeLists.txt`,
    `include/tedge/`, `samples/`, `README.md`). It is laid out to move into
    its own repository and west project later without changing its contents.
  - It holds the `CONFIG_TEDGE_*` Kconfig menu: umbrella, transport choice,
    auth choice and one option per feature. It also holds the outline of the
    `tedge_*` public API. There is no feature logic yet.
  - It depends only on Zephyr (plus MCUboot for firmware update) and never on
    `lib/` or `apps/` code. A bare `samples/minimal` build and a CI check
    enforce that.
  - Spikes A–C fill in a table of flash, static RAM and peak heap for each
    feature and board. That table sets the default feature profile for each
    board and app.
- **Spike F: remote access through the MCU to a LAN host.**
  - `c8y_RemoteAccessConnect` arrives over the Spike A connection.
  - The device checks the target against a policy, opens TCP to a Raspberry
    Pi's SSH port on the same LAN, and bridges it to Cumulocity's device-side
    WebSocket (WSS).
  - Measured: interactive latency, `scp` throughput, heap per session, and
    clean teardown when either side drops.
  - A second run targets the device's own loopback telnet shell.
- **Spike E (optional): SoftAP provisioner feasibility.** Measure a SoftAP,
  captive-portal and HTTP-form provisioner against the C6's 1024 KB `prov`
  partition, and check whether Zephyr's ESP32 driver can run AP and station
  at the same time. This informs the BLE-versus-captive-portal decision for
  onboarding.
- Findings are recorded in `design.md` ("Spike results"). Each ends in a
  go/no-go that feeds the proposal for the next change (`c8y-direct-core`).

Spike code lives in a dedicated `apps/c8y-spike/` and is **not** merged into
the protocol apps or the module. The lasting outputs are:

- the `tedge-zephyr/` skeleton: Kconfig, the public header outline, and a
  minimal sample;
- the measurement tooling.

**Target boards:** ESP32-C6 (`esp32c6_devkitc/esp32c6/hpcore`) is primary.
ESP32-S3-DevKitC-1 (`esp32s3_devkitc/esp32s3/procpu`) repeats Spike A. The
ESP32-WROOM-32 (`esp32_devkitc/esp32/procpu`) is build-only for the footprint
table. Spike F needs a second LAN host with SSH (a Raspberry Pi). **Protocols:** none change. The spike app has no industrial-protocol
frontend, and the footprint table covers the feature cost on top of the OPC-UA,
Modbus and SNMP apps.

**Phase:** 3. This change explicitly starts the Phase 3 thin-edge.io /
Cumulocity work, now that the Phase 1/2 protocol firmware, provisioning and
MCUboot layout are in place.

### Resource constraints

- **RAM:** each TLS session is expected to cost roughly 35–50 KB, plus a
  handshake peak. The direct transport needs up to three sessions: MQTT
  always, HTTPS during a transfer, and WSS during remote access. Free heap
  today is about 250–300 KB on the C6, about 170–200 KB on the S3, and only
  about 70 KB on the WROOM running OPC-UA. The spikes must turn these
  estimates into measurements.
- **Remote access:** each tunnel holds one TLS session for as long as it is
  open, plus two bridge buffers. On the C6 and S3 the target is MQTT plus one
  tunnel at the same time. Spike F confirms it.
- **Flash:** application slots are 1280 KB on 4 MB boards. The largest app
  today is C6 OPC-UA at 839 KB, which leaves about 440 KB for the client, TLS
  and certificate handling. The C6 provisioner is already at 943 of 1024 KB,
  so **no TLS or crypto may be added to the provisioner**. At most it can
  generate and store a one-time password.
- **mbedTLS defaults:** `MBEDTLS_SSL_MAX_CONTENT_LEN` defaults to 1500 in
  Zephyr. Whether Cumulocity honours max-fragment-length decides whether 16 KB
  input buffers are unavoidable.

## Non-goals

- Production device-management code: operation handling, telemetry mapping,
  firmware-update policy, log upload, remote access, configuration. Spike F
  proves the tunnel; the production version, with its policy and session
  handling, is a follow-up change. Those are
  the follow-up changes.
- The thin-edge.io gateway (child-device) transport. The feature skeleton only
  reserves its Kconfig choice.
- Changing the provisioner. Spike C prints the one-time password and URL on
  the console. Delivering them through Improv is the next change.
- Production signing keys, flash encryption, secure boot.
- Making the WROOM run the direct transport. It is only measured.
- Any change to the OPC-UA, Modbus or SNMP behaviour.
- Moving `tedge-zephyr` into its own repository. It stays here until the
  core change has settled its API; this change only keeps it ready to move.
- A stable public API. The spike outlines `include/tedge/` and nothing more.

## Capabilities

### New Capabilities

- `tedge-client-module`: tedge-zephyr as a reusable Zephyr module for any
  application. It depends only on Zephyr and MCUboot. It has a namespaced
  public API (`tedge_*`, `CONFIG_TEDGE_*`) with telemetry push, operation and
  log/config registration, and restart and firmware-confirm hooks. It owns its
  resources within set bounds (its own thread and heap, namespaced settings and
  TLS credentials). The application owns connectivity. There is a minimal
  sample, and the module is kept ready to extract into its own repository.
- `device-management-features`: build-time selection of device-management
  features and transports, and the remote-access target policy for
  forwarding to LAN hosts. Each feature is an independent Kconfig option. A
  disabled feature costs nothing. The capabilities a device advertises match
  its build. Invalid combinations fail at configure time. The footprint of
  each feature is measured and documented, and boards get default profiles
  that fit them.

### Modified Capabilities

(none. The spikes observe the existing `boot-layout` and `wifi-connectivity`
behaviour; they do not change it.)

## Impact

- **Code:**
  - new `tedge-zephyr/`: the module skeleton and `samples/minimal`. Apps
    consume it through `ZEPHYR_EXTRA_MODULES` for now; later it becomes a
    west project in `west.yml`;
  - new `apps/c8y-spike/` (throwaway);
  - optionally `apps/prov-softap-spike/` (Spike E).
- **Dependencies:** Zephyr `MQTT_LIB` + `MQTT_LIB_TLS`, `HTTP_CLIENT`, `SNTP`,
  `SECURE_STORAGE` (PSA ITS), mbedTLS with `X509_CSR_WRITE_C`,
  `dfu/flash_img` and `dfu/mcuboot`. No new west projects.
- **External:** a Cumulocity test tenant with the `mqtt-service.smartrest`
  feature enabled, the Cumulocity CA enabled, and a bootstrap user for the
  fallback path.
- **Docs:** `SCOPE.md` Phase 3 roadmap. The spike results go in this change's
  `design.md`.
