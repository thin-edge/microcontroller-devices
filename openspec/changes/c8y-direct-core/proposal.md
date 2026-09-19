## Why

`c8y-direct-spikes` answered go: a Zephyr device can enroll with the
Cumulocity CA, hold an mTLS session to the MQTT Service on 9883, and handle
SmartREST operations, on the ESP32-C6 and on the ESP32-S3 with PSRAM. All of
that lives in a throwaway app (`apps/c8y-spike`). This change builds the first
real part of **tedge-zephyr** (Phase 3, roadmap step P1): the module, its
public API, onboarding and the connection that every later feature (remote
access, firmware update, telemetry) runs on.

## What Changes

- **The `tedge` module becomes real.** `tedge-zephyr/` gains a public
  `tedge_*` API (**BREAKING** for the unstable outline in
  `include/tedge/tedge.h`, which nothing uses yet): init and start, a
  connection-state callback, identity (external ID, default `tedge-<MAC>`),
  and the hooks the host application needs (restart veto, platform reset).
  The module owns its thread, a bounded mbedTLS heap, its settings subtree
  (`tedge/`) and its TLS credential tags. It still depends on nothing in
  `lib/` or `apps/`.
- **Onboarding through the Cumulocity CA** (primary): P-256 key generated in
  PSA secure storage, one-time password and registration URL, CSR signed
  inside PSA, `simpleenroll` polling, PKCS#7 unwrap, certificate stored in
  settings. The registration URL is exposed through the API so the host app
  can show it (log, shell, BLE provisioner hand-off).
- **Onboarding through the bootstrap user** (fallback): `s/ucr` → `70`
  credentials, over Core MQTT (8883). The client sends `100` and waits for
  the device to exist before subscribing (spike problem P8).
- **Connection:** MQTTS to the MQTT Service (9883) with the enrolled
  certificate, or Core MQTT (8883) with basic auth. One filter per
  SUBSCRIBE. SNTP before certificate validation. JWT via `s/uat` for later
  HTTPS/WSS features. Reconnect with back-off after any drop, returning TLS
  heap and TCP contexts to baseline (P1, never measured in the spikes). A
  warning when another client keeps taking over the same client ID.
- **Device state:** `100`/`114`/`117` (inventory, supported operations built
  from the enabled features, required interval), availability, and restart
  (`510` → `501` → reboot → `503`) through a **full-system reset hook** (a
  CPU reset hangs MCUboot on the C6, P2). State published on free-form `te/`
  topics per D10, with one reference Smart Function (twin → inventory).
- **Integration:** `samples/minimal` and one protocol app (Modbus on the C6)
  use the module, with the glue in `apps/<app>/src/`. `apps/c8y-spike`
  stays as the reference for the features that come later.
- **Kconfig** (all under `CONFIG_TEDGE`): the existing transport, endpoint
  and auth choices, `TEDGE_C8Y_URL`, `TEDGE_DEVICE_ID_PREFIX`,
  `TEDGE_HEAP_SIZE` and `TEDGE_THREAD_STACK_SIZE` become functional;
  `TEDGE_HEALTH` and `TEDGE_RESTART` stop being experimental. New:
  `TEDGE_RECONNECT_BACKOFF_MAX_S` and `TEDGE_ENROLL_POLL_S`. The TLS record
  size stays Zephyr's global `MBEDTLS_SSL_MAX_CONTENT_LEN`: the profiles set
  16 KB, and 8 KB is a documented opt-in for the MQTT Service (P3).

## Non-goals

- Remote access, firmware update, telemetry, logs, configuration and
  certificate renewal (roadmap P2–P7). The spike code for them stays in
  `apps/c8y-spike`.
- The thin-edge.io gateway transport (P8).
- A production SoftAP/captive-portal provisioner (Spike E showed it works;
  it needs a success page, authorization and an Android check first).
- Protecting the device key beyond what the spikes showed (P9): documented as
  a limitation, not solved here.
- The same full-system reset fix in `lib/common` (`liveness.c`, `net.c`): a
  separate small change, because it touches the existing apps.
- The WROOM-32 as a direct-transport target: its only fitting role is the
  remote-access enabler, which needs P2 first.

## Resource constraints

Measured in the spikes on the ESP32-C6, as upper bounds for this change
(spike overheads such as the shell and oversized buffers are not carried):

| Item | Cost |
|---|---|
| TLS + MQTT + SNTP + DNS (text) | +134 KB (C6), +118 KB (S3) |
| Enrollment (text) | +17.7 KB |
| TLS heap, one session, 16 KB records | 51.8 KB peak / 34.8 KB connected (8 KB records: 35.4 / 18.5 KB) |
| Client thread stack | peak 3.3 KB including enrollment |
| TCP contexts | +4 over the app's own (`NET_MAX_CONN` 10 in the spike), since a closed TLS connection holds one until TCP finishes |
| Target | a 64 KB TLS heap (one session plus the enrollment HTTPS request), under 160 KB text; checked per board with `scripts/measure_tedge.sh` |

Boards: ESP32-C6 (primary, 4 MB), ESP32-S3-DevKitC-1 (with the mbedTLS heap
in PSRAM). Next to OPC-UA on the C6 the client must fit the ~256 KB budget
the app leaves.

## Capabilities

### New Capabilities

- `tedge-c8y-onboarding`: CA enrollment and the bootstrap fallback, the
  identity and credential storage, the registration URL for the host app.
- `tedge-c8y-connection`: the MQTT session to 9883/8883, subscription
  rules, time, JWT, reconnect and back-off, client-ID takeover detection.
- `tedge-device-state`: inventory, supported operations, availability,
  restart through the reset hook, state on `te/` topics and the reference
  Smart Function.

### Modified Capabilities

These two are introduced by `c8y-direct-spikes`, which must be archived
first:

- `tedge-client-module`: the public API moves from outline to stable-for-P1,
  and the platform-reset and restart-veto hooks become requirements.
- `device-management-features`: health and restart are no longer
  experimental, and the per-board default profiles (C6 `full`, S3 `full`
  with PSRAM, WROOM `remote-access-enabler`) become requirements.

## Impact

- `tedge-zephyr/`: new `src/` (connection, onboarding, state), a public
  header, Kconfig, `samples/minimal`, tests (Kconfig cases, a `native_sim`
  unit test for the SmartREST parser and the PKCS#7 unwrap).
- `apps/modbus-server`: opts in on the C6, glue in `apps/modbus-server/src/`.
- Zephyr facilities used: PSA ITS secure storage, `tls_credentials`, MQTT,
  `http_client` (for EST), SNTP, settings.
- Cumulocity tenant: the `certificate-authority` and `mqtt-service.smartrest`
  features, one Smart Function.
- Open problems carried over from `c8y-direct-spikes` (design.md): P1
  (reconnect) and P2 (reset) are in scope; P3, P6, P8, P9 and P12 shape the
  design; P4, P5, P7, P10, P11 and P13 belong to later features.
