## 1. Spike: does it fit, and does the crypto exist?

Nothing else is worth starting until these two answers are known. Both are
build-only; no protocol work.

- [x] 1.1 Build a throwaway provisioning image with PSA ECDSA P-256 + SHA-256,
      P-256 ECDH + HKDF-SHA256, ChaCha20-Poly1305 and base64 enabled, and record the flash
      and RAM delta against today's Improv image on `esp32c6_devkitc` and
      `esp32_devkitc` (the two 4 MB boards).
      → C6 +68.8 KB (prov 96.8%), WROOM +32.8 KB (prov 74.6%, dram0 91.1%).
- [x] 1.2 Confirm the `prov` partition still fits with Improv removed from that
      build; if it does not, record by how much and stop for a decision on the
      fallbacks named in design.md.
      → Fits with Improv still present; dropping Improv (5.2 KB) and the
        self-test (3.4 KB) leaves ~41.7 KB on the C6 for the protocol code.
- [x] 1.3 Prove the PSA calls end to end on-device: generate a persistent P-256
      key, sign a fixed message, and open the sealed module from
      `testdata/vectors/seal_p256.json` (P-256 ECDH → HKDF-SHA256 with info
      `ztp/seal/v1` → ChaCha20-Poly1305). Log the results over the console.
      → All pass on the ESP32-S3-DevKitC-1, including tamper rejection and the
        identity key persisting across a reboot. The C6 was wedged (known
        BT+Wi-Fi reset hang, needs a power cycle); a C6/WROOM run is still to
        do before 7.2.
- [x] 1.4 Record the measured per-board flash/RAM table in design.md, replacing
      the placeholder, and note whether the second PSA key is affordable.

## 2. Server: `lab-ztp-provisioner` protocol changes

Separate repo, separate PR — **[reubenmiller/lab-ztp-provisioner#2](https://github.com/reubenmiller/lab-ztp-provisioner/pull/2)**,
branch `feat/p256-crypto-suite`. Must merge before section 5 can be tested.
The Go, Rust and shell agents were left on Ed25519 throughout.

- [x] 2.1 Add `ecdsa-p256-sha256` to `pkg/protocol/sign.go`: `Verify` now
      dispatches on `SignedEnvelope.alg`, `Sign` keeps Ed25519, unknown
      algorithms are still rejected. Raw `r‖s` signatures and uncompressed
      SEC 1 points, matching what PSA emits.
- [x] 2.2 P-256 vectors in `testdata/vectors/{sign,seal}_p256.json`, generated
      by `pkg/protocol/testvectors_p256_gen_test.go`. Signature vectors are for
      the device to **verify**, not reproduce — ECDSA is randomised, so only
      the canonical bytes must match exactly.
- [x] 2.3 `response_format: "text"` on `EnrollRequest` selects the existing
      `TextManifest` rendering; the API layer honours it alongside
      `Accept: text/plain`.
- [x] 2.4 `max_response_bytes` on `EnrollRequest`; an oversized bundle returns
      `rejected` with the measured and permitted sizes.
- [x] 2.5 `internal/transport/ble/doc.go` documents both options under a
      "Constrained peripherals" heading.
- [x] 2.6 Full `go test ./...` passes, including the existing Go-agent
      end-to-end tests — no regression for existing devices.
- [x] 2.7 Per-profile `crypto: {suite: p256}`, plus a `zephyr.yaml` example
      profile. The suite also covers sealing
      (`p256-hkdf-sha256-chacha20poly1305`), so the device needs one curve
      rather than two.
- [x] 2.8 Merged 2026-09-21 (`a8b7ccb`), with two further commits:
      - `a87b53a` — the Rust agent speaks the suite
        (`--crypto-suite p256`), and `/v1/server-info` returns
        `public_key_p256`;
      - `ae8edd4` — text responses stay encrypted under `encrypt_bundle`, carry
        a `server_time` record, and the web/desktop relays hold a *pending*
        text-format device correctly (they previously mis-parsed text bodies).
        The rendering now lives in `pkg/protocol/enrolltext.go`.
- [x] 2.9 Optional server follow-up: an unencrypted text-format response still
      carries the `bundle.*` records (base64 JSON bundle) next to `manifest.*`.
      A text device ignores them, but they roughly double the bytes over GATT
      and count against `max_response_bytes`. Dropping them when
      `response_format` is `text` would halve the BLE transfer.
      → lab-ztp-provisioner#5: MarshalEnrollText renders bundle.* only when
        there is no manifest. An accepted Wi-Fi + Cumulocity response went
        from 2284 to 1064 bytes (-53%). No reader used bundle.* in a text
        response; the device already ignored it, so it needs no change.
- [x] 2.10 Use the Rust agent (`ztp-agent --crypto-suite p256
      --response-format text --profile zephyr`) as a reference device: capture
      a real enrollment exchange against the lab server as a fixture for the
      section 4 parser tests, and as a known-good peer when the Zephyr side
      misbehaves.
      → Superseded (agreed 2026-09-21): the unit-test fixtures are rendered by
        pkg/protocol itself — the code the server runs — and the device has
        enrolled against the real server on the C6, the S3 and the WROOM-32.

## 3. Firmware: protocol selection and skeleton

- [x] 3.1 Add the `APP_PROV_PROTOCOL` choice to `apps/wifi-provisioner/Kconfig`
      with `APP_PROV_IMPROV` as default, plus `APP_PROV_ZTP_PSA_KEY_ID`,
      `APP_PROV_ZTP_SERVER_PUBKEY`, `APP_PROV_ZTP_RX_BYTES` and
      `APP_PROV_ZTP_WINDOW_S`.
      → Choice APP_PROV_IMPROV (default) / APP_PROV_ZTP / APP_WIFI_PROV_SOFTAP (kept its
        name, so overlay-softap.conf still works). **No APP_PROV_ZTP_WINDOW_S:** the
        existing APP_WIFI_PROV_WINDOW_S already covers every protocol.
- [x] 3.2 Make `main.c` and the build select Improv or ZTP from that choice, so
      exactly one protocol is compiled in, and confirm the default build is
      byte-identical in behaviour to today's.
      → CMakeLists.txt and main.c; default build unchanged.
- [x] 3.3 Create `ztp.c`/`ztp.h` with the GATT service definition: the four
      characteristics with the correct UUIDs, permissions and notify support,
      and the advertisement carrying the ZTP service UUID plus the local name
      `ztp`.
      → src/ztp.c; UUID and name "ztp" both in the primary advertisement (26/31 B).
- [x] 3.4 Implement the 2-byte big-endian length framing on both directions,
      including reassembly into the `APP_PROV_ZTP_RX_BYTES` buffer and the
      zero-length end-of-message rule, with the kick distinguished from a
      bundle by whether payload preceded the terminator.
      → Long (queued) writes supported; bytes past a fragment's length ignored as
        in the Go peripheral. Outgoing fragments sized to the negotiated MTU.
- [x] 3.5 Drive the status characteristic through idle → relaying → done/error.

## 4. Firmware: envelope, crypto and manifest

- [x] 4.1 Implement the ZTP identity key in `ztp_crypto.c`: lazily generate a
      persistent P-256 key at `APP_PROV_ZTP_PSA_KEY_ID`, export the public key,
      and sign a message buffer — never exporting the private key.
      → `apps/wifi-provisioner/src/ztp_crypto.c`, written for the spike and
        verified on hardware.
- [x] 4.2 Implement the ephemeral P-256 key pair (exported as an uncompressed
      point for `ephemeral_p256`), ECDH with HKDF-SHA256 in one PSA key
      agreement, and ChaCha20-Poly1305 open. The raw shared secret is never
      the key — that was the X25519 suite's behaviour, not this one's.
      → Same file; opens the Go-sealed vector on hardware.
- [x] 4.3 Build the canonical `EnrollRequest` JSON: fixed lexicographic field
      order, nested `facts` also sorted, optional fields omitted when empty.
      Comment the ordering invariant loudly at the emit site.
      → src/ztp_envelope.c. Timestamps formatted without gmtime_r (see 4.5).
- [x] 4.4 Wrap it in a `SignedEnvelope` with `alg: "ecdsa-p256-sha256"`,
      base64 payload and signature.
- [x] 4.5 Add a `native_sim` unit test comparing the emitted canonical bytes
      against a fixture produced by the Go `Canonicalize`, so a reordered field
      fails the build rather than the field.
      → tests/ztp_provisioner (ztest, native_sim), byte-identical to the Go
        canonicaliser; CI job .github/workflows/wifi-provisioner.yml. Found that
        gmtime_r is undeclared on native_sim (implicit int truncating a pointer):
        replaced by a local civil_from_days formatter, which also saved 1–6 KB.
- [x] 4.6 Implement the text parser in `ztp_manifest.c`, used at two levels:
      first the enroll response (`status`, `reason`, `retry_after`,
      `server_time`, `manifest.payload`, and `encrypted.*`), then the manifest
      obtained by base64-decoding `manifest.payload` (`module=` and
      `module-sealed=`). One `key=value` line splitter serves both, as it
      does for the decrypted plaintext under `encrypt_bundle`. Ignore unknown
      keys (including `bundle.*`), and fail closed on a truncated or
      oversized response.
      → src/ztp_manifest.c. Also rejects a manifest whose device_id is not ours,
        so a clear Wi-Fi module for another device cannot be replayed here.
- [x] 4.7 Unit-test the parser on `native_sim` against fixtures from the Go
      `BuildTextManifest`, including a sealed module and an unknown key.
      → Fixture is a real MarshalEnrollText rendering (wifi.v2 clear, c8y.v2
        sealed, an unknown ssh module); the test opens the sealed module with
        the key it was sealed to. 17 tests, all passing.

## 5. Firmware: module dispatch and appliers

- [x] 5.1 Add the applier registration table in `ztp_apply.c`, with unknown
      module types skipped rather than fatal.
      → src/ztp_apply.c: stage every module (no side effects), then commit in
        table order, stopping at the first failure.
- [x] 5.2 Implement the `wifi.v2` applier: parse the INI `[network]` sections
      and route credentials through the existing `app_net_try_credentials()`
      join test before anything is stored.
      → Picks the highest-priority [network]; refuses to truncate an SSID or
        password.
- [x] 5.3 Implement the `c8y.v2` applier: parse the INI `[c8y]` section and
      write `prov/c8y/{url,tenant,external_id,otp}`, committing only after the
      Wi-Fi module succeeded.
- [x] 5.4 Implement the timesync characteristic as a clock *offset* applied to
      the envelope timestamp, never a wall-clock set.
- [x] 5.5 Handle `pending` and `rejected` responses: stay in provisioning mode,
      rebuild the envelope with a fresh timestamp and nonce for the next
      attempt, store nothing.
- [x] 5.6 On success, clear the boot request and reboot into the application,
      reusing the existing handoff.

## 6. Firmware: application-side handoff

- [x] 6.1 Add `tedge_set_enroll_otp()` to `tedge-zephyr`
      (`include/tedge/tedge.h` + `src/tedge_enroll.c`), persisting to the
      existing `tedge/enroll/otp` key that `ensure_otp()` already prefers.
      → Also fixed two bugs that would have broken ZTP enrollment: ensure_otp()
        only accepted a stored password of exactly 32 bytes (a server-minted
        token of any other length was silently replaced by a generated one),
        and the Basic credential was built in a 128-byte buffer that could
        truncate id:password. Rules moved to src/tedge_otp.c.
- [x] 6.2 Add a module unit test that a supplied password is used, is not
      regenerated, and is deleted once a certificate is stored.
      → Unit-tested: accepted lengths, rejected characters, and that the
        credential builder errors instead of truncating (tedge_otp suite, 3
        tests). tedge_enroll.c itself needs the HTTP/socket stack and is not in
        the unit build, so "used on the first attempt" and "deleted after the
        certificate" are verified end to end in 7.2 instead.
- [x] 6.3 In `lib/common/prov_handoff.c`, read `prov/c8y/*` at boot, call
      `tedge_set_c8y_url()` and `tedge_set_enroll_otp()`, feed `external_id`
      into the identity passed to `tedge_init()`, then delete the keys.
      → Done in lib/common rather than per app where possible:
        app_identity_device_id() returns the ZTP external ID (so all three apps
        pick it up), and prov_c8y_handoff() is one call in each tedge_glue.c.
        The external ID is **kept** after handoff — it is the certificate's CN,
        so deleting it would revert the next boot to a mismatched identity.
- [x] 6.4 Confirm the application image's RAM footprint is unchanged apart from
      the settings reads, and that the provisioning image links no
      `tedge-zephyr` symbol.
      → The provisioner links no tedge_ symbol (checked with nm). The
        application gains 65 B of static RAM (ztp_id + a flag) and ~1.1 KB of
        code.

## 7. Host tooling and end-to-end verification

- [x] 7.1 Write `scripts/ztp_provision.py` as a bench relay: scan, kick, relay
      the envelope to a ZTP server, write the manifest back. Mirror the
      ergonomics of `scripts/improv_provision.py`.
      → scripts/ztp_provision.py: scan, envelope, enroll (with --wait for
        pending approval).
- [x] 7.2 End-to-end on `esp32c6_devkitc` with a real ZTP server: unprovisioned
      device → BLE session → Wi-Fi joins → device boots the application →
      enrolls with the supplied password → appears in Cumulocity, with no
      console interaction.
      → Done 2026-09-21 on the ESP32-C6 against thin-edge-io.eu-latest: bundle
        applied, Wi-Fi tested, enrolled with the ZTP token in 3.4 s, connected,
        no console or Cumulocity UI involved. Found and fixed on the way: the
        ESP32 BT HAL leaves the thread that called bt_enable() with interrupts
        locked (ECC on it dropped the link, HCI 0x08) — slow work now runs on a
        dedicated worker; and the app printed the registration URL with the
        ZTP password in the clear — tedge_registration_url() now refuses a
        supplied password.
        Then verified from a wiped device (storage erased, no Wi-Fi or tenant in
        the image): the app handed over by itself, everything came from the
        bundle, a new Cumulocity key and certificate were created, and the
        password never appeared on the console. This found one more bug, fixed
        in lib/common/net.c: with no credentials the app handed over before
        recording its identity, so a factory-fresh device registered under the
        provisioner's fallback name (tedge-prov<mac>) for good.
- [x] 7.3 Repeat on `esp32_devkitc` (WROOM-32) and one S3 board.
      → **S3-DevKitC-1:** full run from wiped storage, modbus + tedge full
        profile: enrolled with the ZTP token and connected to eu-latest as
        tedge-modbus7c0c5f5a6eb8; the password never on the console.
        **WROOM-32 (no PSRAM):** the ZTP provisioner works — relayed by the
        Mac app across the room, bundle applied, Wi-Fi tested, SNMP agent up
        on the provisioned network, app name correct (tedge-snmp3c71bf10c2e4).
        The **Cumulocity half cannot run on this module**, for a reason outside
        this change: apps/snmp-agent/boards/esp32_devkitc_esp32_procpu_tedge.conf
        is the ESP32-CAM's (4 MB PSRAM for the 96 KB TLS heap); on a
        PSRAM-less WROOM that image aborts at boot, and with the heap moved
        back to internal DRAM (56 KB, firmware update off) dram0 overflows by
        18.8 KB. tedge on a bare WROOM is a board-fit problem of its own.
- [x] 7.4 Verify the stock `lab-ztp-provisioner` relay — the Web Bluetooth SPA
      or the desktop binding, unmodified — provisions the device.
      → The stock lab-ztp-provisioner desktop app (macOS) did the relaying,
        including holding the device through pending approval.
- [x] 7.5 Verify the failure paths: unreachable SSID leaves the device
      untouched and provisionable; tampered sealed ciphertext aborts the whole
      bundle; oversized manifest fails closed.
      → Done on the C6 (already provisioned, so "nothing replaced" was the
        check), with a fake server built on lab-ztp-provisioner's pkg/protocol
        answering the device's real envelope, sealed to its real session key:
        unreachable SSID → "Could not join …; nothing stored" (the staged
        Cumulocity data was never committed); tampered seal → "sealed payload
        did not open (-77)" during staging, no Wi-Fi join attempted; 8 KB
        response → overflow error; manifest for another device_id → refused.
        Status "error" each time, and the device stayed provisionable for the
        next attempt. Back in the application it rejoined its original
        network, ran no ZTP handoff and reconnected with its existing
        certificate — none of the test data reached storage.
- [x] 7.6 Verify a re-provision: a device that already has credentials is put
      back into provisioning mode by the button gesture and accepts a new
      bundle with a different tenant.
      → Exercised by the same C6, which held a tedge-dev05 certificate: it
        presented that to eu-latest (CONNACK 5) and discarded the new password.
        Fixed: tedge_set_enroll_otp() discards an old certificate. Re-run: new
        certificate for eu-latest, connected. Re-provisioning by button
        (rather than boot request) not separately tested.

## 8. Documentation

- [x] 8.1 README: a ZTP provisioning section next to the Improv one — how to
      select the protocol, how to flash, and how to run a provisioning session.
- [x] 8.2 README: extend the existing plaintext-link caveat with the ZTP trust
      model — TOFU by default, what a hostile relay can and cannot do, and how
      to pin a server key later.
- [x] 8.3 SCOPE.md: replace the "Delivery" bullet under Onboarding, which
      describes the superseded Improv-result sketch, with the ZTP relay flow.
- [x] 8.4 DEVICES.md: note that a ZTP-provisioned device does not print a
      registration URL, and how to check `prov/c8y/*` when onboarding
      misbehaves.
