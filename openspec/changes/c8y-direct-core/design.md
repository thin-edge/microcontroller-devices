## Context

`c8y-direct-spikes` (archived 2026-09-19) proved the direct transport on the
ESP32-C6 and ESP32-S3: Cumulocity CA enrollment on the device, mTLS to the MQTT
Service on 9883, SmartREST operations, restart, firmware update and remote
access. All of it lives in `apps/c8y-spike`, a throwaway app with spike
shortcuts (one file per spike, globals shared between threads, a shell for
driving tests, oversized measuring buffers, `lib/common` calls).

`tedge-zephyr/` is still a skeleton: the Kconfig menu, `VERSION`,
`tedge_version()`, an unstable outline in `include/tedge/tedge.h`, the
independence check and draft profiles. The baseline specs
`tedge-client-module` and `device-management-features` describe the
contract; the spike results live in the archived change's `design.md`
("Spike results", U1–U12, P1–P13).

This change turns the parts every later feature needs into module code:
onboarding, the connection, and device state. It is roadmap step P1.

Constraints:
- **The module depends only on Zephyr** and its modules (hal_espressif,
  mbedTLS/TF-PSA-Crypto, MCUboot). No `lib/`, no `apps/`.
- **The application owns the network.** The module reacts to network events.
- **Budgets** (from the spikes): one TLS session is 51.8 KB peak / 34.8 KB
  connected with 16 KB records; the text of TLS + MQTT + SNTP + DNS is about
  130 KB and enrollment about 18 KB. Next to OPC-UA on the C6, about 256 KB of
  libc heap is left for everything else.

## Goals / Non-Goals

**Goals:**
- A public `tedge_*` API that is stable enough for P2–P7 to build on.
- CA onboarding (primary) and bootstrap basic auth (fallback) inside the
  module, with the registration URL handed to the application.
- One robust MQTT session to 9883 (CA) or 8883 (basic auth), recovering by
  itself from network loss (P1) without leaking TLS heap or TCP contexts.
- Inventory, supported operations, required interval, restart and state on
  `te/` topics.
- `samples/minimal` and the Modbus app on the C6 integrate it; `CONFIG_TEDGE=n`
  builds stay byte-identical.

**Non-Goals:** remote access, firmware update, telemetry API, logs, config,
certificate renewal (P2–P7); the gateway transport (P8); a production
SoftAP provisioner; hardware key protection (P9). The `lib/common` reset fix
is a separate change.

## Decisions

### D1: Module layout, with an internal transport interface

```
tedge-zephyr/
  include/tedge/tedge.h        public API (single header)
  src/tedge_core.c             lifecycle, client thread, state machine, API queue
  src/tedge_c8y.c              Cumulocity transport: MQTT session, SmartREST
                               dispatch, JWT, twin/inventory publishing
  src/tedge_smartrest.c        CSV parsing/quoting (no I/O; unit-tested)
  src/tedge_enroll.c           CA onboarding: key, OTP, CSR, EST, PKCS#7
  src/tedge_bootstrap.c        bootstrap-user onboarding (s/ucr -> 70)
  src/tedge_time.c             SNTP, unless the clock is already valid
  src/tedge_platform.c         default reset and device-ID helpers
  src/tedge_internal.h         the transport ops and shared internals
  certs/                       default trust anchors (overridable)
```

The feature code (restart now; firmware, remote access later) talks to a small
`struct tedge_transport` (`connect`, `poll`, `publish_twin`, `op_executing`,
`op_succeeded`, `op_failed`, `subscribe_ops`). Only `tedge_c8y.c` implements it
in this change. The gateway transport (P8) adds a second implementation.

*Alternative: port `apps/c8y-spike` files as they are.* Rejected: they share
globals across threads and mix measurement code with behaviour.

### D2: One client thread owns the MQTT client

Zephyr's MQTT client isn't thread-safe (the spike needed a message queue for
the remote-access thread). The module's single client thread
(`TEDGE_THREAD_STACK_SIZE`, `TEDGE_THREAD_PRIORITY`) owns the socket, the
MQTT context and all publishing. Public API calls from other threads (later:
telemetry, operation results) go through a bounded `k_msgq`; they never touch
the socket. Hooks run on the client thread, as the header already states.

The spike measured the MQTT thread at 3.0–3.6 KB peak including enrollment
and the mTLS handshake. The default stack becomes 6144 (2× headroom, as on
the WROOM enabler).

### D3: Memory: a module heap for transients, mbedTLS's global heap for TLS

- **Module heap** (`TEDGE_HEAP_SIZE`, a `k_heap`): transient buffers only.
  These are the EST response (up to 4 KB), the CSR/PEM work buffers (~2.5 KB)
  and queued API messages. Allocations fail with a reason instead of touching
  the application's heap.
- **TLS memory** comes from Zephyr's mbedTLS heap (`MBEDTLS_HEAP_SIZE`), which
  is global and belongs to the application's configuration; a module can't
  have a private mbedTLS heap. The README and profiles state the need: 56 KB
  for one session with 16 KB records (40 KB with 8 KB), with +40 KB per
  concurrent session in later features. On the S3 the profile puts it in
  PSRAM (`MBEDTLS_HEAP_CUSTOM_SECTION`).
- **Static buffers** stay small: MQTT rx/tx 2 KB each, JWT 1 KB, identity
  strings.

This needs a MODIFIED `Bounded resource ownership` requirement: TLS memory is
the application's mbedTLS heap, sized from the documented figures.

### D4: Onboarding: CA enrollment first, bootstrap as a build choice

`TEDGE_AUTH_C8Y_CA` (default):
1. A persistent P-256 key in PSA ITS (`SECURE_STORAGE`, ITS over settings) at
   `TEDGE_PSA_KEY_ID` (default `0x0007E571`, inside `PSA_KEY_ID_USER_MAX`).
2. A 32-character one-time password from `psa_generate_random`, stored at
   `tedge/enroll/otp` until the certificate arrives.
3. The registration URL
   `https://<tenant>/apps/devicemanagement/index.html#/deviceregistration?externalId=<id>&one-time-password=<otp>`
   is available from `tedge_registration_url()`, and the state becomes
   `TEDGE_STATE_AWAITING_REGISTRATION` (the `on_state` hook tells the app to
   show it). It is never logged by the module above DEBUG.
4. CSR signed inside PSA (`mbedtls_pk_wrap_psa`), EST `simpleenroll` with
   Basic `<id>:<otp>`, polled every `TEDGE_ENROLL_POLL_S` (10 s) with back-off
   to 60 s, until 200. The PKCS#7 reply is unwrapped with the spike's ASN.1
   walk (no PKCS#7 module).
5. Certificate at `tedge/enroll/cert`; OTP deleted; credentials registered at
   `TEDGE_TLS_TAG_BASE + 1` (key exported as DER for `tls_credentials`, P9).

`TEDGE_AUTH_BOOTSTRAP`: connect to 8883 as the bootstrap user, subscribe
`s/dcr`, poll `s/ucr` every 5 s, store `70,<tenant>,<user>,<password>` at
`tedge/bootstrap/*`, reconnect as the device. Bootstrap credentials come from
Kconfig (`TEDGE_BOOTSTRAP_USER`/`_PASSWORD`, meant for a fleet overlay) or at
runtime through `tedge_set_bootstrap_credentials()`.

**Tenant host:** `TEDGE_C8Y_URL` is the default. A settings value at
`tedge/c8y/url`, set through `tedge_set_c8y_url()`, overrides it, so one image
serves several tenants (for example set from the provisioner hand-off later).

**Trust anchors:** the module embeds `certs/*.pem` listed in
`TEDGE_C8Y_CA_FILES` (default: Go Daddy Root G2, which the spikes' tenant
chain ends in) at `TEDGE_TLS_TAG_BASE + 0`. An application points the option
at its own files for other Cumulocity instances.

*Alternative: enrollment in the provisioner image.* It has no TLS stack today,
and would need one only for this. Rejected for now; it stays an option for
tight boards.

### D5: Connection state machine and back-off

```
STOPPED -> WAITING_NETWORK --(IPv4 address)--> WAITING_TIME --(clock ok)-->
  [AWAITING_REGISTRATION] --> CONNECTING --(CONNACK)--> CONNECTED
CONNECTED --(disconnect / keepalive loss / net down)--> back-off --> CONNECTING
any --(net down)--> WAITING_NETWORK
```

- Network: `NET_EVENT_L4_CONNECTED`/`DISCONNECTED` from the connection
  manager, falling back to `NET_EVENT_IPV4_ADDR_ADD/DEL`. The module never
  touches Wi-Fi.
- Time: skip SNTP when the realtime clock is already after the build date
  (the app may have set it); otherwise query `TEDGE_SNTP_SERVER`.
- Back-off: 3 s, doubling to `TEDGE_RECONNECT_BACKOFF_MAX_S` (default 300 s),
  ±20% jitter, reset after 60 s connected. The 3 s floor comes from the
  spike's TCP-context exhaustion; the README states the extra `NET_MAX_CONN`
  (+4) the client needs.
- Keepalive 60 s; `117,<TEDGE_REQUIRED_INTERVAL_MIN>`.
- **Client-ID takeover:** three sessions in a row closed by the broker within
  10 s of CONNACK log a warning, "another client may be using this ID", and
  the back-off continues (spike B finding 6).
- Every exit path releases the TLS socket; tests check that the TLS heap and
  `NET_MAX_CONN` usage return to their pre-connect baseline (P1).

### D6: Session start: create, then subscribe, then state

On CONNACK: `100,<name>,<type>` first, then one SUBSCRIBE per topic (`s/ds`,
`s/e`, `s/dat`, `devicecontrol/notifications` when a feature needs JSON
operations), then `114,<supported ops>`, `115` (when firmware update is built
in, later), `117`, `s/uat`, then the twin fragments. A SUBACK with 0x80
(refused while the device doesn't exist yet, P8) is retried after 5 s up to
five times rather than treated as final. The supported-operations list is
assembled from the features compiled in (`TEDGE_RESTART` → `c8y_Restart`;
later features add theirs) plus `tedge_register_operation()` entries.

An operation for a feature that isn't built in gets `502,<op>,"<feature> is
not built into this image"` (baseline requirement).

### D7: JWT as an internal service

`s/uat` after every connect and 50 minutes after each token; the latest token
is kept in a 1 KB buffer (`tedge_internal.h`: `tedge_c8y_jwt()`). Only
certificate devices get one; basic-auth builds skip the request (the MQTT
Service refuses it). Nothing uses it in this change except that later
features and `simplereenroll` (P6) need it; it is included now because the
session start owns it.

### D8: Restart through a hook and a full-system reset

`510` → `restart_request` hook (veto → `502` with the hook's reason) →
`501` → marker `tedge/restart` → graceful MQTT disconnect → reset. After the
reboot, the next CONNACK sends `503,c8y_Restart` and clears the marker. This
was verified in the spike: SUCCESSFUL after 17–32 s, and it survives a hung
reboot.

The reset is `tedge_hooks.reset` if the application provides it, else
`tedge_platform_reset()`: on Espressif SoCs `esp_rom_software_reset_system()`
(a full-system reset; the CPU reset from `sys_reboot()` hangs MCUboot on the
C6, P2), else `sys_reboot(SYS_REBOOT_COLD)`. This satisfies the baseline
rule that the module never reboots without the application's hook.

### D9: State on `te/` topics, with a Core MQTT fallback

- Twin data: `te/device/<id>///twin/<fragment>` (QoS 1) on the MQTT Service,
  or `inventory/managedObjects/update/<id>` with `{"<fragment>":…}` on Core
  MQTT. It is republished on every connect, with no reliance on retained
  messages. The first built-in fragment is `tedge_Agent`
  (`{"name":"tedge-zephyr","version":"<VERSION>","transport":"c8y-mqtt-service"}`),
  so a reference Smart Function has something to map from the start. The
  application can add its own through `tedge_publish_twin()`.
- Health: `te/device/<id>/service/tedge-zephyr/status/health`
  `{"status":"up","time":<unix s>}` on connect. Availability in Cumulocity
  keeps using `117` and the connection itself; a last-will "down" is only
  added if the MQTT Service is shown to deliver it (open question).
- The module ships reference Smart Functions under
  `tedge-zephyr/smartfunctions/` (twin → inventory fragment, health →
  inventory), starting from the tenant owner's working `tedge_RemoteAccess`
  function (P12).

### D10: Public API for this change

Implemented now, and marked stable for P1:
`tedge_version`, `tedge_init`, `tedge_start`, `tedge_stop`,
`tedge_get_state`, `tedge_registration_url`, `tedge_set_c8y_url`,
`tedge_set_bootstrap_credentials`, `tedge_publish_twin`,
`tedge_register_operation` / `tedge_operation_*` (custom SmartREST
operations: the dispatcher needs them anyway), and hooks `on_state`,
`restart_request`, `reset` (new), `progress`.

Still declared but unimplemented (they return `-ENOTSUP`, and the header
says which change implements them): telemetry, events and alarms (P4),
log and config types (P5/P7), `firmware_confirm_check` (P3),
`remote_access_allow` (P2). `TEDGE_API_UNSTABLE` stays until P3, since
firmware update will still add to the hooks.

### D11: Integrations and tests

- **`samples/minimal`:** Wi-Fi from its own tiny `net_mgmt` connect (no
  `lib/common`), `tedge_init(NULL, &hooks)`, logs state changes and the
  registration URL.
- **`apps/modbus-server` on the C6:** `apps/modbus-server/src/tedge_glue.c`
  maps `lib/common` identity (firmware name/version) and the status LED
  onto the API, and uses `boot_request_reboot()` as the reset hook. It is
  enabled by a C6 board overlay plus `tedge-zephyr/profiles/full.conf`.
- **Unit tests (`native_sim`, ztest):** SmartREST parsing and quoting, the
  PKCS#7 unwrap (fixture from the spike's captured reply), the back-off
  sequence, the twin JSON and topic builders, and the settings round-trip.
- **Kconfig tests:** the existing `tests/kconfig` cases plus new ones for the
  added options.
- **Hardware checks (C6, then S3):** enrollment from an erased device; restart
  from Cumulocity; the P1 reconnect test (Wi-Fi drop 3 × 60 s via the
  application's shell `wifi disconnect`, plus one real AP drop), recording
  time to CONNECTED and TLS-heap/TCP-context baselines; the footprint rerun
  with `scripts/measure_tedge.sh` (C6 Modbus with and without `TEDGE`).

## Risks / Trade-offs

- [SmartREST on the MQTT Service is Public Preview] → Core MQTT stays a
  one-option switch (`TEDGE_C8Y_CORE_MQTT`), tested on hardware in this
  change.
- [The global mbedTLS heap is shared with the application's own TLS] → The
  README documents the client's need; the footprint script reports it; the
  module fails the connect with a clear log when a handshake runs out.
- [Trust anchors differ per Cumulocity instance] → Configurable
  `TEDGE_C8Y_CA_FILES`; a wrong anchor fails with a named
  certificate-verification error, not a timeout.
- [The device key is only obfuscated (P9)] → Documented in the README as a
  development-grade limitation; no production claim.
- [8 KB records have ~2 KB margin (P3)] → Profiles default to 16 KB; 8 KB is
  only in the enabler profile, with the risk written next to it.
- [Reconnect behaviour was never measured (P1)] → The state machine is
  designed around it and the hardware check is a task, not an afterthought.
- [A hook blocks the client thread] → Documented: hooks must return promptly;
  the `progress` hook lets the app's watchdog notice a stuck client.

## Migration Plan

- Apps without `CONFIG_TEDGE` are unaffected: the byte-identical check in
  `tests/kconfig` / CI stays.
- `apps/c8y-spike` keeps building against its own sources. It is not ported
  to the module in this change. It remains the reference for P2/P3 and is
  deleted once those land.
- Devices enrolled by the spike (`spike/enroll/*` settings, key
  `0x0007E571`) are not migrated. The module uses `tedge/…` keys, so a
  spike-enrolled device enrolls again (a new registration). Test devices only.

## Open Questions

- **Smart Function format and delivery (P12):** what the tenant owner's
  `tedge_RemoteAccess` function looks like (source, trigger topic, output),
  so the reference set can copy its shape, and whether functions can be
  installed from a file or CLI.
- **Last will on the MQTT Service:** does it deliver an LWT to Smart
  Functions? This decides whether health "down" is possible without a
  heartbeat check.
- **Trust anchors for other Cumulocity instances** (public cloud regions,
  on-prem): which roots should ship by default?
- **Registration URL delivery:** the BLE provisioner's Improv result and the
  SoftAP success page could both carry it; that's a provisioner change after
  this one.
