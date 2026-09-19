## 1. Module foundations (firmware)

- [ ] 1.1 Add the D1 source layout to `tedge-zephyr/` (`src/tedge_core.c`, `tedge_c8y.c`, `tedge_smartrest.c`, `tedge_enroll.c`, `tedge_bootstrap.c`, `tedge_time.c`, `tedge_platform.c`, `tedge_internal.h`, `certs/`), compiled per Kconfig; keep `scripts/check-independence.py` passing
- [ ] 1.2 Add the new Kconfig options (`TEDGE_RECONNECT_BACKOFF_MAX_S`, `TEDGE_ENROLL_POLL_S`, `TEDGE_PSA_KEY_ID`, `TEDGE_C8Y_CA_FILES`, `TEDGE_SNTP_SERVER`, `TEDGE_REQUIRED_INTERVAL_MIN`, `TEDGE_BOOTSTRAP_USER`/`_PASSWORD`), set the thread stack default to 6144, make `TEDGE_HEALTH` and `TEDGE_RESTART` selectable without `TEDGE_EXPERIMENTAL_FEATURES`, and have the module select what it needs (MQTT, sockets, TLS, SNTP, settings, PSA ITS). Add `tests/kconfig` cases for them
- [ ] 1.3 Embed the trust anchors from `TEDGE_C8Y_CA_FILES` (default: Go Daddy Root G2) at `TEDGE_TLS_TAG_BASE + 0`, via a generated `.inc`
- [ ] 1.4 Update `include/tedge/tedge.h` to D10: add `reset` to `tedge_hooks`, `tedge_set_c8y_url()`, `tedge_set_bootstrap_credentials()`, `tedge_publish_twin()`, and mark which change implements each declared-but-unimplemented function (they return `-ENOTSUP`)
- [ ] 1.5 Implement `tedge_init`/`tedge_start`/`tedge_stop`/`tedge_get_state`: the client thread, the module `k_heap`, the API message queue and the `on_state`/`progress` hooks (D2, D3)

## 2. Connection (firmware)

- [ ] 2.1 Network wait on connection-manager L4 events (falling back to IPv4 address events), never touching the interface
- [ ] 2.2 Time: skip SNTP when the realtime clock is after the build date, otherwise query `TEDGE_SNTP_SERVER` with retries
- [ ] 2.3 MQTT session to 9883 (CA) or 8883 (basic auth / Core MQTT) with the trust anchors and device credentials; keepalive 60 s; one filter per SUBSCRIBE
- [ ] 2.4 Session start order (D6): `100`, subscriptions with 0x80 retry, `114` from compiled features and registered operations, `117`, `s/uat`, twin and health
- [ ] 2.5 Reconnect back-off (3 s floor, doubling to the maximum, ±20% jitter, reset after 60 s connected) and the duplicate-client-ID warning (three broker closes within 10 s of CONNACK)
- [ ] 2.6 JWT: request after connect and 50 min after each token, CA builds only; `tedge_c8y_jwt()` internal accessor
- [ ] 2.7 SmartREST dispatcher: parse `s/ds` lines, route `510` and registered operations, answer unknown or not-built-in operations with `502` and a reason naming the feature

## 3. Onboarding (firmware)

- [ ] 3.1 CA enrollment (D4): PSA key, OTP, registration URL, PSA-signed CSR, `simpleenroll` polling with back-off, PKCS#7 unwrap, storage under `tedge/enroll/`, credential registration; `TEDGE_STATE_AWAITING_REGISTRATION`; `tedge_registration_url()` (OTP only at debug level)
- [ ] 3.2 Tenant host from `TEDGE_C8Y_URL`, overridable by `tedge_set_c8y_url()` and persisted at `tedge/c8y/url`
- [ ] 3.3 Bootstrap onboarding: 8883 as the bootstrap user, `s/dcr` + `s/ucr` polling, store `70` credentials under `tedge/bootstrap/`, reconnect as the device; `tedge_set_bootstrap_credentials()`; passwords never logged

## 4. Device state (firmware)

- [ ] 4.1 Restart (D8): `restart_request` veto → `502`; else `501`, marker, clean disconnect, reset via the `reset` hook or `tedge_platform_reset()` (full-system reset on Espressif SoCs); `503` after the next CONNACK
- [ ] 4.2 Twin publishing (D9): `tedge_Agent` on every connect; `tedge_publish_twin()` stores application fragments and republishes them after every reconnect; `te/` topics on the MQTT Service, inventory updates on Core MQTT
- [ ] 4.3 Health: `te/device/<id>/service/tedge-zephyr/status/health` `{"status":"up"}` on connect (MQTT Service only)
- [ ] 4.4 Custom operations: `tedge_register_operation()` and `tedge_operation_payload/succeed/fail`, routed from the dispatcher (the handler sees the operation already EXECUTING)

## 5. Unit and configuration tests

- [ ] 5.1 `tests/unit` (ztest, `native_sim`): SmartREST parsing and CSV quoting
- [ ] 5.2 PKCS#7 unwrap against a fixture from the spike's captured `simpleenroll` reply
- [ ] 5.3 Back-off sequence, twin topic/payload builders and inventory-update fallback
- [ ] 5.4 CI: run the unit tests and the extended `tests/kconfig` cases in `.github/workflows/tedge-zephyr.yml`

## 6. Integrations (firmware)

- [ ] 6.1 `samples/minimal`: its own minimal Wi-Fi connect (no `lib/common`), `tedge_init`/`tedge_start`, logs state changes and the registration URL; builds for the C6 and `native_sim`
- [ ] 6.2 `apps/modbus-server/src/tedge_glue.c` (C6): identity from `lib/common`, status LED from `on_state`, `boot_request_reboot()` as the `reset` hook, registration URL on the console; enabled by a C6 overlay with `profiles/full.conf`
- [ ] 6.3 Update `profiles/minimal.conf`, `full.conf` and `remote-access-enabler.conf` to the options that now exist, with the mbedTLS heap each needs (S3: PSRAM section)
- [ ] 6.4 Show that the Modbus, OPC-UA and SNMP apps built with `TEDGE=n` are byte-identical to their baselines

## 7. Reference Smart Functions (cloud side)

- [ ] 7.1 Obtain the tenant owner's working `tedge_RemoteAccess` function as the template (P12)
- [ ] 7.2 Write `tedge-zephyr/smartfunctions/`: twin → inventory fragment, health → inventory, with install instructions
- [ ] 7.3 Install them on the test tenant and check that `tedge_Agent` and the health status appear on the device's managed object

## 8. Hardware verification

- [ ] 8.1 C6 Modbus + tedge from erased storage: registration URL on the console, `c8y deviceregistration register-ca`, certificate, mTLS to 9883, inventory and supported operations in Cumulocity, Modbus still served
- [ ] 8.2 Restart from Cumulocity ends SUCCESSFUL; a vetoing test hook ends FAILED with its reason
- [ ] 8.3 P1 reconnect test: three Wi-Fi drops of 60 s (application shell `wifi disconnect`/`connect`) and one real access-point drop; record the time back to CONNECTED and the TLS heap and TCP-context counts before and after (no leak)
- [ ] 8.4 Core MQTT: the same C6 built with `TEDGE_C8Y_CORE_MQTT` (and once with bootstrap auth) connects, and the twin arrives as an inventory update
- [ ] 8.5 S3-DevKitC-1 with the full profile (mbedTLS heap in PSRAM): enroll, connect, restart
- [ ] 8.6 Footprint: extend `scripts/measure_tedge.sh` to the module builds (C6 Modbus with and without `TEDGE`, S3, `samples/minimal`) and record the table in `tedge-zephyr/README.md`

## 9. Documentation and wrap-up

- [ ] 9.1 `tedge-zephyr/README.md`: integration guide (identity, hooks, required Kconfig for sockets/TCP contexts/mbedTLS heap per profile), onboarding flow, P9 key-protection limitation, P3 record-size note
- [ ] 9.2 Record results and any design changes in design.md; update the `SCOPE.md` roadmap (P1 done)
