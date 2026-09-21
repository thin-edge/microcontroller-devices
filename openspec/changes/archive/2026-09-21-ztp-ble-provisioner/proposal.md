## Why

BLE provisioning today hands the device a Wi-Fi SSID and PSK and nothing else,
because it speaks Improv Wi-Fi — a protocol whose entire command set is
`CMD_WIFI_SETTINGS` and `CMD_IDENTIFY` (`apps/wifi-provisioner/src/improv.c:136`).
Cumulocity onboarding is therefore still a build-time affair: the tenant host is
baked in through `CONFIG_TEDGE_C8Y_URL`, and the device mints its own one-time
password and waits for an operator to read a registration URL off the console
and register it by hand. An installer standing in front of a sealed enclosure
has no console.

`lab-ztp-provisioner` already solves the other half of this problem for Linux
devices: an operator's phone or laptop acts as a BLE *relay* between an
unconnected device and a ZTP server, and the server returns a signed
provisioning bundle carrying both Wi-Fi credentials and a freshly minted,
per-device Cumulocity enrollment token. Its `c8y.v2` module is already exactly
the four fields this repo needs — `url`, `tenant`, `external_id`,
`one_time_password` — and the token is sealed end-to-end so the relay never
sees it. Teaching the provisioning image to speak that protocol turns a
two-step, console-bound onboarding into one BLE session with no console at all.

SCOPE.md:210 already names this as the intended delivery mechanism ("the BLE
Wi-Fi provisioner returns the registration URL as the Improv RPC result"); this
change supersedes that sketch with the ZTP relay flow, which additionally
removes the need for a per-fleet tenant URL baked into the image.

**Target boards:** the four BLE-capable targets — `esp32_devkitc/esp32/procpu`
(WROOM-32), `esp32s3_devkitc/esp32s3/procpu`,
`adafruit_qt_py_esp32s3/esp32s3/procpu`, `esp32c6_devkitc/esp32c6/hpcore`. The
Feather ESP32-S2 has no BLE radio and is out of scope, as are `native_sim` and
the Pico W.

**Industrial protocols:** all three source applications (OPC-UA, Modbus TCP,
SNMP) benefit equally and none of them change. The work is confined to the
provisioning image, `lib/common/`, and a small addition to the `tedge-zephyr`
public API.

**Phase:** 3 (Cumulocity onboarding), building on the Phase 2 provisioning
image and MCUboot layout.

## What Changes

- **A second provisioning protocol in `apps/wifi-provisioner`.** A new
  `ztp.c` implements the `lab-ztp-provisioner` GATT service
  (`6e400001-b5a3-f393-e0a9-e50e24dcca9e`, Nordic-UART-derived) as a BLE
  peripheral: `request` (write), `response` (notify), `status` (notify),
  `timesync` (write), with 2-byte big-endian length framing and a zero-length
  fragment as end-of-message.
- **Kconfig choice, one protocol per image.** `APP_PROV_PROTOCOL` selects
  `APP_PROV_IMPROV` (today's behaviour, default) or `APP_PROV_ZTP`. They are
  mutually exclusive by construction: two 128-bit service UUIDs cannot both fit
  in the 31-byte BLE primary advertising PDU alongside Flags and TX-Power, and
  the `prov` partition on the 4 MB boards is already 92% full with Improv
  alone.
- **A module dispatch table** maps bundle module types to appliers.
  `wifi.v2` writes through `wifi_credentials`; `c8y.v2` stores the Cumulocity
  fields for the application image. Unknown types are skipped, not fatal, as
  the wire format requires.
- **Onboarding data crosses the image boundary through settings.** The
  provisioner writes `prov/c8y/{url,tenant,external_id,otp}` into the shared
  `storage` partition; the application image reads them at boot and pushes them
  into the `tedge-zephyr` module. The provisioner never links `tedge-zephyr`.
- **New `tedge-zephyr` public API: `tedge_set_enroll_otp()`.** The module
  currently mints its own one-time password; it must be able to accept one
  issued by Cumulocity through the ZTP server instead. `ensure_otp()` already
  prefers a stored value (`tedge-zephyr/src/tedge_enroll.c:140`), so this is a
  setter over the existing `tedge/enroll/otp` key. `tedge_set_c8y_url()`
  already exists and gains its first caller.
- **BREAKING (behavioural, ZTP builds only):** with `APP_PROV_ZTP` selected the
  device no longer generates its own one-time password when the bundle supplied
  one. A device provisioned this way is registered in Cumulocity by the ZTP
  server before it ever boots the application, so the console registration URL
  is not printed.
- **Changes to `lab-ztp-provisioner`** (a companion PR in that repo — the user
  owns it and has agreed it may change to accommodate constrained devices):
  - **P-256 signatures alongside Ed25519.** Mbed TLS, and therefore Zephyr's
    PSA stack, does not implement Ed25519. The device already has a
    persistent P-256 key, ECDSA and SHA-256 compiled in for Cumulocity EST
    enrollment. The `SignedEnvelope.alg` field already exists to carry this;
    the server must accept `ecdsa-p256-sha256` on inbound envelopes.
  - **Text manifest over BLE.** `BuildTextManifest` already renders a bundle
    as sorted `key=value` lines with base64 payloads, precisely so a shell
    agent needs no JSON parser. Extending it to the BLE response path (a
    `response_format` hint on `EnrollRequest`) removes the need for a JSON
    parser on the MCU entirely.
  - **A bounded-response contract** so a device with a fixed RX buffer can
    advertise its limit and get a predictable failure rather than a truncated
    bundle.
- **Host tooling.** `scripts/ztp_provision.py` mirrors
  `scripts/improv_provision.py` for bench testing without the phone app.

## Capabilities

### New Capabilities

- `ztp-provisioning`: the ZTP relay provisioning protocol as implemented by the
  provisioning image — the build-time choice between provisioning protocols,
  the GATT service and framing, the enrollment envelope the device signs, the
  trust model for the returned bundle, module dispatch and the two appliers,
  the storage the provisioner owns, and the handoff of onboarding data to the
  application image.

### Modified Capabilities

- `wifi-provisioning`: the image implements exactly one protocol, chosen at
  build time, and the Improv requirement applies to Improv builds only. (Folded
  in from `ztp-provisioning` once `ble-wifi-provisioning` had been archived.)
- `tedge-c8y-onboarding`: the one-time password and tenant host may now be
  supplied from outside instead of generated on-device, and the client must
  honour an externally issued password. Adds `tedge_set_enroll_otp()` to the
  public API.

`boot-layout` is not modified: the `prov/c8y/` subtree this change adds to the
shared `storage` partition is ZTP-specific, and is specified in
`ztp-provisioning`.

## Impact

- **Code:**
  - new `apps/wifi-provisioner/src/ztp.c`, `ztp.h`, `ztp_manifest.c`
    (line parser), `ztp_crypto.c` (PSA sign / ECDH / AEAD), `ztp_apply.c`
    (dispatch table and the two appliers);
  - `apps/wifi-provisioner/Kconfig`: the `APP_PROV_PROTOCOL` choice and the ZTP
    options;
  - `lib/common/prov_handoff.c`: read `prov/c8y/*` in the application and feed
    the module;
  - `tedge-zephyr/src/tedge_enroll.c` + `include/tedge/tedge.h`:
    `tedge_set_enroll_otp()`;
  - new `scripts/ztp_provision.py`.
- **Public API:** one added function, `tedge_set_enroll_otp()`. No signature
  changes. Application glue stays in `apps/`/`lib/`, not in `tedge-zephyr/`.
- **Dependencies:** PSA crypto in the provisioning image — ECDSA P-256 with
  SHA-256, P-256 ECDH with HKDF-SHA256, ChaCha20-Poly1305, base64, and PSA
  ITS over the shared settings store. One curve only: no Ed25519, no
  Curve25519, no JSON parser.
- **External:** a companion change to `lab-ztp-provisioner` (P-256 suite,
  text responses over BLE, bounded responses) — **merged** as
  lab-ztp-provisioner#2 on 2026-09-21. The server side is ready; the Zephyr
  side is the remaining work. The same PR taught the Rust agent the P-256 suite
  and text responses, so a Linux box can stand in for the device when testing.

### Resource constraints

The binding constraint is the `prov` partition, ~1 MB on the 4 MB boards
(WROOM-32, C6), where the Improv image already sits at 92%. The ZTP image must
fit the same partition, which is why the protocols are a build-time choice
rather than both present. Dropping Improv's Wi-Fi-test path is not on the
table — ZTP needs the same "verify before storing" behaviour — so the budget
has to come from the protocol code itself being small: a line parser and three
PSA calls, against Improv's state machine plus the new crypto.

RAM in the application image must not move at all: the application gains only
four settings reads and one extra API call.

| Kconfig option | Purpose |
|---|---|
| `APP_PROV_PROTOCOL` (choice) | `APP_PROV_IMPROV` (default) or `APP_PROV_ZTP` |
| `APP_PROV_ZTP_PSA_KEY_ID` | PSA key ID of the device's ZTP identity key, distinct from `CONFIG_TEDGE_PSA_KEY_ID` |
| `APP_PROV_ZTP_SERVER_PUBKEY` | Pinned ZTP server public key; empty selects TOFU |
| `APP_PROV_ZTP_RX_BYTES` | Maximum bundle the device will accept |
| `APP_PROV_ZTP_WINDOW_S` | Provisioning window before falling back to the application |

Measured in the spike (design.md, *Spike results*): the crypto adds 68.8 KB on
the C6, putting `prov` at 96.8%, and 32.8 KB on the WROOM-32 (74.6%, dram0
91.1%). Both fit; the C6 has ~41.7 KB left for the protocol code once Improv
and the self-test are dropped.

## Non-goals

- **Replacing Improv.** It stays the default and keeps working; ZTP is an
  alternative for fleets that run a ZTP server.
- **Verifying the server's bundle signature in this change.** The first
  implementation uses the protocol's documented BLE TOFU mode
  (`DecodePayloadUnverified`); pinning `APP_PROV_ZTP_SERVER_PUBKEY` and
  verifying is a follow-up, and the Kconfig option is laid out for it here.
- **Modules beyond `wifi.v2` and `c8y.v2`.** The dispatch table exists so
  `files`, `hook` and `ssh` can be added later; none are implemented.
- **The acknowledgement leg.** The device does not send `Acknowledgement` back
  to the server.
- **Whole-bundle encryption** (`EncryptBundle`). Per-module sealing already
  protects the only secret that matters here.
- **Provisioning over the SoftAP/captive-portal path**, serial, or USB.
- **BLE pairing or bonding.** As with Improv, the link is unauthenticated; the
  Cumulocity token is protected by end-to-end sealing rather than by the link.
- **Bluetooth in an application image**, on the ESP32-S2, the Pico W, or
  `native_sim`.
