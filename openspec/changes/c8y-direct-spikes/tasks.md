## 1. Tenant and host preparation (cloud and host side)

- [x] 1.1 Pick the Cumulocity test tenant. Confirm the tenant features `mqtt-service.smartrest` and the Cumulocity CA are enabled, and note the tenant URL in a git-ignored overlay (`c8y.local.conf`)
- [x] 1.2 Create a device user (basic auth) for Spike A, and confirm bootstrap-user credentials are available for the Spike C fallback
- [x] 1.3 Capture the tenant's TLS chain for 9883 and 8883 (`openssl s_client -showcerts`). Record the root CAs, key types and whether max-fragment-length is negotiated (`-maxfraglen 4096`)
- [x] 1.4 Prepare a Raspberry Pi with `sshd` on the same LAN as the C6, and create two Cloud Remote Access endpoints on the spike device: SSH to the Pi's `<ip>:22`, and Telnet to `127.0.0.1:23` (for Spike F)
- [x] 1.5 Check the host-side paths with `mosquitto_pub`/`mosquitto_sub`, so that a device failure can't be mistaken for a tenant problem: SmartREST `100`/`114` on 9883 and 8883, one free-form publish, and `s/uat` → `s/dat`

## 2. Module skeleton (firmware)

- [x] 2.1 Create `tedge-zephyr/` in the D8 layout (`zephyr/module.yml` named `tedge`, `CMakeLists.txt`, `Kconfig`, `VERSION`, `README.md`, `include/tedge/`, `profiles/`, `samples/`, `tests/`)
- [x] 2.2 Write the D2 Kconfig menu: `TEDGE` umbrella, transport, endpoint and auth choices, one option per feature, the remote-access targets choice and session cap, hidden `TEDGE_FEATURE_AVAILABLE_*` gates, `TEDGE_HTTP`, and the thread, heap and TLS-tag options
- [x] 2.3 Encode the dependencies from the spec (firmware update needs MCUboot, remote access needs CA authentication, file features select `TEDGE_HTTP`). Confirm that an invalid combination fails at configure time
- [x] 2.4 Outline `include/tedge/tedge.h` from D7 (identity, `tedge_publish_*`, `tedge_register_*`, restart, firmware-confirm and remote-access hooks, state callback, progress hook). Declarations and doc comments only, marked unstable
- [x] 2.5 Add `samples/minimal` (build-only for now) that builds with Zephyr and `tedge-zephyr` only. Build it for the C6 and `native_sim`
- [x] 2.6 Add the extraction guard: a check script (and a CI step) that fails if anything under `tedge-zephyr/` references `lib/`, `apps/` or `../`
- [x] 2.7 Show that a protocol app built with `TEDGE=n` matches its baseline flash and RAM (`build_sb_c6_modbus` against a build that adds the module through `ZEPHYR_EXTRA_MODULES`)
- [x] 2.8 Create `apps/c8y-spike/` (sysbuild, C6 and S3-DevKitC-1 board files reusing the shared layouts, spike Kconfig options `SPIKE_TLS_MQTT`/`SPIKE_OTA`/`SPIKE_ENROLL`/`SPIKE_REMOTE_ACCESS`), composing `lib/common` and `tedge-zephyr`

## 3. Spike A: TLS and MQTT to Cumulocity (firmware)

- [ ] 3.1 SNTP before the first TLS connect. Log the time, and refuse to connect until the clock is set
- [ ] 3.2 MQTTS to 9883 with basic auth and the embedded tenant CA, `MBEDTLS_SSL_MAX_CONTENT_LEN=16384`. Connect, subscribe to `s/ds` and `s/e`, publish `100`/`114`/`117`
- [ ] 3.3 Handle `510` (restart): report `501`, persist a "restart pending" marker in settings, reboot, then report `503` after reconnecting
- [ ] 3.4 Publish a thin-edge.io-shaped free-form measurement every 10 s (the spike app feeds it from `data_source_*`, as a user app would feed its own data). Request a JWT on `s/uat` and log that `s/dat` arrives (never the token itself)
- [ ] 3.5 Measure on the C6: flash delta, static RAM, heap at idle, handshake peak and steady state, and handshake time over 10 connects. Record them in design.md
- [ ] 3.6 Retry with a 4096-byte record size and max-fragment-length (U2). Record whether the handshake and a large downlink message work
- [ ] 3.7 Repeat 3.2–3.5 against Core MQTT 8883 (U3 fallback), then repeat 3.5 on the S3-DevKitC-1
- [ ] 3.8 Reconnect test: drop Wi-Fi at the AP for 60 s, three times. Record the time back to "connected" and the heap after each cycle (leak check)

## 4. Spike B: OTA into slot1 (firmware)

- [ ] 4.1 Streamed HTTP download into `slot1` with `flash_img` from a laptop `http.server`. `flash_img_check()`, `boot_request_upgrade(BOOT_UPGRADE_TEST)`, reboot. Record the download rate and the swap time
- [ ] 4.2 The new image confirms itself with `boot_write_img_confirmed()` from a shell command. Verify the version persists across a further reset
- [ ] 4.3 Revert run: don't confirm, reset, and check that MCUboot restores the previous image. Also check the watchdog-reset case with liveness on
- [ ] 4.4 Verify that the Wi-Fi credentials in `storage`, the provisioner in `prov` and `bootreq` are untouched after 4.1–4.3 (the device reconnects without provisioning, and the triple-press still reaches the provisioner)
- [ ] 4.5 HTTPS download of a Cumulocity binary using the JWT from 3.4. Record the peak heap with MQTT and HTTPS open at once (U1 concurrency)
- [ ] 4.6 Try a redirecting URL (a GitHub release asset) and record what the HTTP client does (U6)

## 5. Spike C: Cumulocity CA enrollment (firmware)

- [ ] 5.1 Persistent P-256 key in PSA ITS (`SECURE_STORAGE` over settings). It survives a reboot and a reflash of the app
- [ ] 5.2 Generate a 32-character one-time password from the CSPRNG and store it. Print the registration URL on the console and from a `tedge enroll` shell command
- [ ] 5.3 Build a PKCS#10 CSR with `CN=<external id>` signed by the PSA key (`MBEDTLS_X509_CSR_WRITE_C`). Check it on the host with `openssl req -verify`
- [ ] 5.4 Poll `simpleenroll` every 10 s with Basic `<id>:<otp>`. On `200`, store the certificate. Log the error bodies seen before the device is registered
- [ ] 5.5 mTLS connect to 9883 (and 8883) with the enrolled certificate, reusing the Spike A loop. Record the extra RAM of client-certificate authentication and how long the exported key is resident (U9)
- [ ] 5.6 Call `simplereenroll` with mTLS only, then with a Bearer JWT. Record which one works (U8)
- [ ] 5.7 Bootstrap fallback: `s/ucr` polling with the bootstrap user until `70,…`, store the credentials, reconnect as the device user

## 6. Spike F: remote access to a LAN host (firmware)

- [ ] 6.1 Subscribe to and parse `c8y_RemoteAccessConnect` (SmartREST) on the Spike A connection. Log the fields received (never the connection key) and confirm the template (U12)
- [ ] 6.2 Target policy check (own IPv4 subnet, allow-list, local only). A denied target fails the operation with a reason and opens no socket
- [ ] 6.3 Open TCP to the target, then WSS to Cumulocity's device-side remote-access endpoint for the connection key, using mTLS or JWT (whichever works, recorded for U12)
- [ ] 6.4 Bridge thread: `zsock_poll()` on both sockets, fixed per-direction buffers, `app_alive()`-style progress, clean close on EOF or error from either side, and an idle timeout
- [ ] 6.5 Report the operation (executing, then successful once the tunnel is up, or failed with a reason) and publish tunnel open and close events naming the target
- [ ] 6.6 Measure on the C6: heap per session with MQTT connected, interactive SSH latency, `scp` throughput for 10 MB, and the session cap (a second connect while one is open is refused). Repeat the latency check on the S3-DevKitC-1
- [ ] 6.7 Local target: Zephyr `shell_telnet` bound to loopback, reached through the Telnet endpoint, and not reachable from the LAN directly
- [ ] 6.8 Failure cases: Pi powered off (connect fails, operation failed), Wi-Fi drop mid-session (tunnel closes and heap returns to baseline), user closes the browser tab (bridge exits)

## 7. Cloud-side verification (host side)

- [ ] 7.1 Register the device from the printed URL. Confirm the managed object, the external ID, the certificate CN and the supported operations in Device Management
- [ ] 7.2 Trigger restart from the UI and confirm the operation goes to SUCCESSFUL after the reboot
- [ ] 7.3 Open SSH to the Pi and Telnet to the device from the Cumulocity UI; confirm the tunnel events appear on the device
- [ ] 7.4 Confirm how the free-form telemetry appears (or doesn't) in the tenant, and write down which consumer would map it (Dynamic Mapper or other)

## 8. Footprint and profiles (firmware and tooling)

- [ ] 8.1 Write `scripts/measure_tedge.sh` (D5): TEDGE off, `samples/minimal`, minimal profile, then each feature added on its own, for each board and app. Report flash and static RAM deltas and `zephyr.signed.bin` against the slot
- [ ] 8.2 Run it for the C6, S3-DevKitC-1 and WROOM-32 (build-only) with all three apps. Add the runtime heap numbers from 3.5, 4.5, 5.5 and 6.6
- [ ] 8.3 Draft `tedge-zephyr/profiles/minimal.conf` and `full.conf`, and a per-board profile recommendation (in particular what, if anything, fits the WROOM running OPC-UA)

## 9. Spike E (optional): SoftAP provisioner feasibility (firmware)

- [ ] 9.1 Build a SoftAP, DHCP-server, catch-all DNS and HTTP-form provisioner variant for the C6. Measure it against the 1024 KB `prov` partition with BLE removed
- [ ] 9.2 Check whether AP and station at the same time works with Zephyr's ESP32 Wi-Fi driver (serve the form while a station connect tests the credentials)
- [ ] 9.3 Test captive-portal behaviour on one iOS and one Android phone, and record the result

## 10. Wrap-up

- [ ] 10.1 Fill in "Spike results" in design.md: one go/no-go per unknown U1–U12, with the measurements
- [ ] 10.2 Update the `SCOPE.md` Phase 3 roadmap with anything the results changed (transport default, board profiles, onboarding path)
- [ ] 10.3 Draft the `c8y-direct-core` proposal from the results
