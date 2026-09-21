## Context

The provisioning image (`apps/wifi-provisioner`, introduced by
`ble-wifi-provisioning`) is a separate MCUboot-booted image in its own `prov`
partition. It carries all the Bluetooth in the system; application images carry
none. It speaks Improv Wi-Fi, stores verified credentials through
`wifi_credentials`, clears the boot request and reboots into the application.

`lab-ztp-provisioner` approaches provisioning from the other end. The device is
an unconnected BLE **peripheral**; the operator's phone or laptop is the
**central** and acts as a relay with internet access. The device publishes a
signed enrollment envelope; the relay forwards it to a ZTP server; the server
returns a provisioning bundle that the relay writes back. The bundle's `c8y.v2`
module carries `url`, `tenant`, `external_id` and `one_time_password`, and
because the token is a secret the server seals that module end-to-end with the
device's ephemeral key-agreement key (X25519 in the original suite, P-256 in
the one this change uses) — the relay carries ciphertext it cannot read.

That is a better fit than Improv for onboarding, but it was designed against a
Go agent on Linux. Three of its choices do not survive contact with Zephyr, and
since the ZTP repo is under the same ownership, the cheapest fix for two of them
is on the server side rather than on the MCU.

Current state that this design leans on:

- `tedge-zephyr/src/tedge_enroll.c:140` — `ensure_otp()` already reads
  `tedge/enroll/otp` from settings before generating one. An externally
  supplied password needs a setter, not a new code path.
- `tedge_set_c8y_url()` (`include/tedge/tedge.h:165`) already persists the
  tenant host to `tedge/c8y/url`. It has no callers.
- The `storage` partition is already shared between the provisioning image and
  the application image.
- The provisioning image already tests credentials by joining before storing
  them (`improv.c:540`), and already reboots through a boot request.

## Goals / Non-Goals

**Goals:**

- One BLE session provisions Wi-Fi *and* Cumulocity onboarding, with no console
  and no per-fleet firmware build.
- Interoperate with `lab-ztp-provisioner`'s existing relay implementations —
  the Web Bluetooth SPA and the desktop binding — with no device-specific
  branch in either.
- Keep the Cumulocity enrollment token unreadable by the relay.
- Add no RAM to application images and fit the existing `prov` partition.
- Keep `tedge-zephyr` free of provisioning knowledge: it gains one setter and
  learns nothing about BLE or ZTP.

**Non-Goals:**

- Server-signature verification (TOFU first; see Decision 4).
- Modules beyond `wifi.v2` and `c8y.v2`, the acknowledgement leg, or
  whole-bundle encryption.
- Running both provisioning protocols in one image.

## Decisions

### 1. P-256 ECDSA envelopes instead of Ed25519 — changed on the server

The protocol signs with Ed25519 (`pkg/protocol/sign.go`). **Mbed TLS does not
implement Ed25519**, and Zephyr's PSA stack is Mbed TLS. Options considered:

| Option | Cost |
|---|---|
| Vendor an Ed25519 implementation (TweetNaCl, Monocypher) | ~8–13 KB flash plus SHA-512, a second crypto stack in an image that already has PSA, and key storage outside PSA ITS |
| Reuse MCUboot's bundled fiat-crypto curve25519 | Bootloader code is not linkable from an application image; would still need SHA-512 |
| **Add `ecdsa-p256-sha256` to the protocol's `alg` field** | A `switch` in the server's `Verify`; **zero** new crypto on the device |

Chosen: the third. `SignedEnvelope.alg` is already a string on the wire and
`Verify` already rejects unknown values, so the extension point exists — the
server simply has to populate it. On the device this reuses exactly the
primitives Cumulocity EST enrollment already compiles in
(`PSA_WANT_ALG_ECDSA`, `PSA_WANT_ALG_SHA_256`, `PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_*`),
with the key held in PSA ITS under its own ID.

**Landed** in lab-ztp-provisioner#2 as a per-profile `crypto: {suite: p256}`
option rather than a bare algorithm flag, because sealing has the same problem
and the two should not drift apart. The suite covers both:

| | `ed25519-x25519` (default) | `p256` |
|---|---|---|
| envelope + bundle signature | `ed25519` | `ecdsa-p256-sha256` |
| sealing | `x25519-chacha20poly1305` | `p256-hkdf-sha256-chacha20poly1305` |
| device ephemeral field | `ephemeral_x25519` | `ephemeral_p256` |

So the device links **one** curve, not two — which is what the `prov`
partition budget actually needs. Wire encodings were chosen to match PSA
exactly: raw `r‖s` signatures (not DER) and uncompressed SEC 1 points, so
`psa_sign_hash` and `psa_export_public_key` output goes straight on the wire.

Two consequences worth carrying into the firmware:

- The P-256 seal derives its AEAD key with **HKDF-SHA256** (`info` =
  `"ztp/seal/v1"`, empty salt), unlike the X25519 path which uses the raw ECDH
  secret. A P-256 shared secret is a field element, not a uniform string, so
  it must not be used as a key directly. On the device this is a single PSA
  call: `PSA_ALG_KEY_AGREEMENT(PSA_ALG_ECDH, PSA_ALG_HKDF(PSA_ALG_SHA_256))`.
- Cross-language vectors now exist at `testdata/vectors/{sign,seal}_p256.json`.
  The signature vectors are for the device to **verify**; ECDSA is randomised,
  so a device cannot reproduce them byte-for-byte. Only the canonical bytes
  must match, which is exactly what Decision 3 is about.

The device's ZTP identity key is **separate** from
`CONFIG_TEDGE_PSA_KEY_ID`. They live in different images, answer to different
trust roots (the ZTP server versus the Cumulocity CA), and have different
lifetimes — the Cumulocity key must survive a re-provision that legitimately
re-keys the ZTP identity.

### 2. Text manifest for the response — changed on the server

The bundle arrives as JSON: `EnrollResponse` wrapping a `SignedEnvelope` whose
`payload` is base64 of a canonical `ProvisioningBundle`, whose modules carry
base64 payloads and, when sealed, five more base64 fields. Parsing that on an
MCU means a JSON parser Zephyr's `json.h` cannot provide (heterogeneous arrays,
optional objects), or a hand-rolled scanner.

`pkg/protocol/textmanifest.go` already renders a bundle as sorted `key=value`
lines exactly so the shell agent needs no JSON parser:

```
device_id=<id>
issued_at=<RFC3339>
module=wifi.v2 <base64 INI>
module-sealed=c8y.v2 raw <eph_pub_b64> <nonce_b64> <ciphertext_b64>
protocol_version=1
```

It is already the signature input in its own right, and already deterministic.
The server change is to select it for the BLE path — an explicit
`response_format: "text"` hint on `EnrollRequest` rather than content
negotiation, because the device has no HTTP request of its own: the relay
POSTs on its behalf, so `Accept: text/plain` is unreachable from the device.

**Landed** in lab-ztp-provisioner#2, along with `max_response_bytes` so an
oversized bundle comes back as a `rejected` status naming both sizes instead
of a truncated write the device would have to detect.

What the device actually receives is one level up from the manifest: the
**enroll response** in text form (`pkg/protocol/enrolltext.go`), whose records
carry the manifest base64-encoded:

```
protocol_version=1
status=accepted
server_time=2026-09-21T09:00:00Z
bundle.alg=… bundle.payload=…           (JSON bundle — ignore)
manifest.alg=ecdsa-p256-sha256
manifest.payload=<base64 of the manifest lines above>
manifest.signature=…
```

So parsing is two passes of the same `key=value` splitter: the response, then
the base64-decoded `manifest.payload`. `status`, `retry_after` and
`server_time` come from the first pass, which also gives a *pending* device a
server clock to correct its next envelope against. Under `encrypt_bundle` only
`encrypted.*` records are present and the decrypted plaintext is the
`manifest.*` records, so it feeds the same parser.

The `bundle.*` records are dead weight for a text device — roughly doubling
what crosses GATT — and dropping them for text-format requests is a small,
optional server follow-up (tasks.md 2.9).

The **request** direction stays JSON. The device *emits* it, and emitting
canonical JSON is trivial (Decision 3), whereas parsing it is not. The
asymmetry is deliberate: each side does the easy direction.

Consequence: the device needs base64 decode, a line splitter and a field
splitter. No JSON parser, in either direction.

### 3. Canonical JSON by construction, not by canonicalisation

RFC 8785 requires lexicographically sorted keys and no insignificant
whitespace. The device builds its own `EnrollRequest`, so it emits the fields in
sorted order from a single `snprintf` template and is canonical by
construction — no generic canonicaliser, no intermediate DOM. The field set is
fixed:

```
capabilities, device_id, ephemeral_p256, facts, max_response_bytes,
nonce, protocol_version, public_key, response_format, timestamp
```

with `facts` a nested object whose own keys are likewise emitted sorted
(`agent_version`, `hostname`, `mac_addresses`, `model`, `os`). This is exactly
the shape of the `EnrollRequest` case in `testdata/vectors/sign_p256.json`,
whose `canon_b64` is the fixture to compare against. Omitted optional fields
simply are not printed, matching Go's `omitempty`.

This is a correctness-critical invariant that is easy to break by adding a
field in the wrong place, so it gets a unit test on `native_sim` that compares
the emitted bytes against a fixture generated by the Go canonicaliser.

### 4. TOFU first, pinning laid out

The device does not verify the server's signature in this change; it
base64-decodes the manifest and applies it, which is the protocol's documented
BLE TOFU mode. `CONFIG_APP_PROV_ZTP_SERVER_PUBKEY` is defined now and honoured
later, so enabling verification is a Kconfig value plus one `psa_verify_message`
call — P-256 again, if the server signs bundles with the same algorithm it
accepts.

The exposure is bounded and worth stating plainly: a hostile relay can feed the
device a Wi-Fi network and a Cumulocity tenant of its choosing. It cannot read
the real token (sealing is to the device's ephemeral key), and it cannot
impersonate the device to the real server (the identity key stays in PSA ITS).
For a lab fleet provisioned by its own operator this is acceptable; for a real
deployment it is not, which is why the option exists.

### 5. Onboarding data crosses images through a `prov/` settings subtree

The provisioning image must not link `tedge-zephyr` — the module is meant to be
liftable into its own repository and must not acquire a provisioning
dependency, and the image cannot afford it anyway. Writing `tedge/c8y/url`
directly from the provisioner would reach into the module's private namespace
by string literal.

Instead the provisioner owns `prov/c8y/{url,tenant,external_id,otp}` in the
shared `storage` partition, alongside the existing `prov/ident`. The
application image, which does link the module, reads them in
`lib/common/prov_handoff.c` and calls:

```c
tedge_set_c8y_url(url);        /* exists, first caller */
tedge_set_enroll_otp(otp);     /* new */
/* external_id -> struct tedge_identity passed to tedge_init() */
```

`tedge_set_enroll_otp()` writes `tedge/enroll/otp`, which `ensure_otp()`
already prefers over generating one. The module needs no other change, and the
direction of dependency stays correct: the application knows about both the
provisioner and the module; neither of those knows about the other.

The keys are deleted by the application once consumed, so a stale token from an
earlier provisioning cannot resurface after a factory-ish reset of the tedge
subtree.

### 6. Both protocols behind a Kconfig choice

Two 128-bit service UUIDs cannot be advertised together: a 31-byte primary PDU
minus 3 bytes of Flags minus an 18-byte UUID AD entry leaves 10 bytes, and
BlueZ-style stacks often add a 3-byte TX-Power entry on top. The ZTP central
matches primarily on the advertised service UUID, and `ble.go`'s own comments
record what happens when the payload overflows into the scan response: passive
scanners stop seeing the device. Registering both GATT services while
advertising only one would make the other unreachable to any client that scans
by UUID, which is the normal case for both protocols.

Beyond the radio, the `prov` partition is at 92% on the 4 MB boards. A choice
is therefore both the correct and the affordable answer.

### 7. Reuse the existing verify-then-store path

`wifi.v2` credentials go through the same `app_net_try_credentials()` join test
Improv uses before anything is written, and the `c8y.v2` fields are only
committed once the Wi-Fi test passes. A bundle that names an unreachable
network leaves the device exactly as it was, still in provisioning mode, rather
than half-provisioned with a tenant but no network.

Ordering within a bundle is therefore: unseal everything → apply `wifi.v2`
(test-then-store) → apply `c8y.v2` → clear boot request → reboot.

## Spike results (tasks 1.1–1.4, 2026-09-21)

Measured with the boot-time self-test (`CONFIG_APP_PROV_ZTP_CRYPTO_SELFTEST`,
`apps/wifi-provisioner/overlay-ztp-selftest.conf`), which links and *calls*
every PSA operation the suite needs, so the linker cannot discard any of it.
Built as the provisioner image of a `modbus-server` sysbuild, against the
unchanged Improv provisioner as baseline. `prov` is 1024 KB on both 4 MB
boards.

| Board | Improv baseline | + p256 crypto | Δ | `prov` used | Tightest RAM |
|---|---|---|---|---|---|
| C6 (`esp32c6_devkitc`) | 944,923 B | 1,015,404 B | +68.8 KB | **96.8%** (33.2 KB free) | sram0 59.5% → 59.6% |
| WROOM-32 (`esp32_devkitc`) | 748,886 B | 782,440 B | +32.8 KB | 74.6% | **dram0 90.7% → 91.1%** (+0.6 KB) |

**It fits on both.** The binding constraints differ by board — flash on the
C6, dram0 on the WROOM — and neither is exceeded.

What the numbers are made of:

- Bluetooth already links Mbed TLS and the PSA core (Zephyr 4.x's BT host
  uses PSA for its own crypto), so the cost is only the algorithms. Symbol
  growth is dominated by P-256 ECC and bignum; `ztp_crypto.c` itself is
  1.6 KB.
- The production ZTP image drops Improv (5.2 KB) and the self-test with its
  vectors (3.4 KB), so the **ZTP protocol code has ~41.7 KB to fit in on the
  C6**: framing, canonical JSON emit, the text parser and two appliers. Base64
  is already linked.
- **No dedicated Mbed TLS heap.** A first build gave Mbed TLS an 8 KB heap,
  which cost the WROOM 8 KB of dram0 (96.2%). The self-test passes on the
  libc heap (146 KB on these boards) as the Bluetooth host's PSA use already
  does, ~15% slower. The dedicated heap is not used.
- **AES-GCM stays linked regardless** — the Espressif Wi-Fi supplicant pulls
  it in for GCMP (`SOC_WIFI_GCMP_SUPPORT`). Switching the ITS transform to
  ChaCha20-Poly1305 saved 143 bytes, so the provisioner keeps the default
  transform, the same as the application.

**On hardware** (ESP32-S3-DevKitC-1; only the `prov` partition and the boot
request were written): all four checks pass against the Go vectors — the
server's sealed `c8y.v2` module opens to its exact plaintext and fails on one
flipped byte; the server's signature over a canonical `EnrollRequest`
verifies and fails on one flipped byte; the persistent identity key signs and
verifies; a session key is created. After a reboot the identity key is found,
not regenerated, with the same public key — so the device keeps one identity
across provisioning sessions. The application then booted and connected to
Cumulocity over mTLS, confirming the ZTP key (`0x0007E572`) coexists with the
Cumulocity key (`0x0007E571`) in one ITS store.

The C6 could not be tested: it was wedged and did not answer esptool at all,
which matches the known C6 hang after a CPU reset with BT and Wi-Fi up (only a
power cycle clears it). It runs the same Mbed TLS C code as the S3; a C6 and
WROOM run is still worth doing before task 7.2.

**Timings** (S3, software ECC, libc heap): ECDH + HKDF + AEAD open ≈ 0.85 s,
ECDSA verify ≈ 1.1 s, sign ≈ 0.4 s, session key generation ≈ 0.3 s. A full
enrollment spends ~1.5 s in crypto — fine for provisioning, but far too long
for a Bluetooth callback: **the envelope build and the bundle open must run on
a work item, never in the GATT write handler** (the Go peripheral does the
same with `go p.respond(...)`).

## Protocol sequence

```
relay (central)                          device (peripheral)
      |                                        |
      |  scan: service 6e400001-…, name "ztp"  |
      |<---------------------------------------|
      |  connect, discover 4 characteristics   |
      |--------------------------------------->|
      |  write timesync = RFC3339 UTC          |
      |--------------------------------------->|  set clock offset
      |  subscribe response, status            |
      |--------------------------------------->|
      |  write request = {0,0}  (the "kick")   |
      |--------------------------------------->|  generate ephemeral P-256
      |                                        |  build canonical EnrollRequest
      |                                        |  sign P-256 over PSA
      |  notify response: framed envelope JSON |
      |<---------------------------------------|  status := relaying
      |  POST /v1/enroll  ------> ZTP server   |
      |  <------ text enroll response          |
      |  write request = framed response       |
      |--------------------------------------->|  parse status/server_time,
      |                                        |  base64-decode manifest.payload,
      |                                        |  parse manifest lines
      |                                        |  ECDH+HKDF, ChaCha20-Poly1305 unseal
      |                                        |  apply wifi.v2 (join test)
      |                                        |  apply c8y.v2 -> prov/c8y/*
      |  notify status := done                 |
      |<---------------------------------------|  clear boot request, reboot
```

Framing on both characteristics is 2-byte big-endian length, payload, repeated,
with a zero length as end-of-message; fragments are 180 bytes. The same
`request` characteristic carries both the kick and the bundle, distinguished
only by whether any payload preceded the terminator — matching the Go
peripheral's `onWrite`, which calls its handler on every zero-length fragment.

A `pending` or `rejected` response leaves the device advertising and waiting for
the relay to come back with a fresh attempt; the envelope is rebuilt each time
so its timestamp stays inside the server's skew window.

## Risks / Trade-offs

- **The `prov` partition is nearly full on the C6.** → Measured (see *Spike
  results*): it fits, at 96.8% with the crypto linked and ~41.7 KB left for the
  protocol code. The margin is thin enough that a Zephyr upgrade could eat it.
  Re-measure at task 6.4, and keep `CONFIG_SIZE_OPTIMIZATIONS`, fewer log
  strings, or trimming Bluetooth host features in hand as the first levers.
  Growing `prov` at the expense of the A/B slots stays the last resort, since
  it collides with OTA.
- **The protocol change must land in two repos at once.** → Sequenced: the
  server PR (lab-ztp-provisioner#2) is open and green, and is additive — every
  new field is `omitempty`, so existing agents' canonical signing bytes are
  unchanged and the default suite is untouched. The Zephyr side stays blocked
  on that merge. Keep the device's `protocol_version` handling strict so a
  mismatched server fails loudly rather than half-applying a bundle.
- **Canonical-JSON-by-construction is fragile under edit.** → Fixture test on
  `native_sim` against Go-generated bytes; a reviewer checklist item on the
  field-ordering comment in `ztp.c`.
- **TOFU lets a hostile relay choose the tenant.** → Documented above and in the
  spec; `APP_PROV_ZTP_SERVER_PUBKEY` is defined now so the follow-up is small.
  The README's existing plaintext-link caveat gains a ZTP paragraph.
- **A fixed RX buffer meets an unbounded bundle.** → `APP_PROV_ZTP_RX_BYTES`
  caps it, and the bounded-response server change makes an oversized bundle a
  clean `rejected` rather than a truncation. Without the server change the
  device must still fail closed: abort, set `status := error`, keep the old
  credentials.
- **Clock skew before the first Wi-Fi join.** → The protocol's `timesync`
  characteristic exists for exactly this and the relay writes it before the
  kick; the device applies it as an offset only, never as a wall-clock set that
  could confuse SNTP later.
- **Two PSA keys on a device whose ITS is backed by the shared settings
  store.** → Distinct key IDs, and the ZTP key is generated lazily on first
  provisioning so images that never provision pay nothing.

## Migration Plan

1. Land the `lab-ztp-provisioner` changes (P-256 `alg`, `response_format`,
   bounded responses) with the Go agent still on Ed25519 — both algorithms
   accepted, nothing breaks for existing Linux devices.
2. Land the Zephyr side with `APP_PROV_IMPROV` as the default, so no existing
   build changes behaviour.
3. Opt individual boards in by setting `APP_PROV_ZTP` in their
   `boards/*_tedge.conf`.

Rollback is a Kconfig value and a reflash of the `prov` partition; the
application image and the A/B slots are untouched. A device already provisioned
through ZTP keeps working — its credentials and `prov/c8y/*` are in `storage`,
which neither path erases.

## Open Questions

- ~~Does the ZTP server mint the Cumulocity token?~~ **Resolved:** yes. The
  lab's `zephyr` profile runs the Cumulocity provider with
  `issuer.mode: local` against the `c8y-tedge-eu-latest` credential, so every
  bundle carries a freshly minted, per-device token and the device never needs
  to generate one. The console registration URL is not part of the ZTP flow.
- ~~Should `external_id` from the bundle override the application's own
  identity?~~ **Resolved: it must.** The server mints the one-time password
  *for* the `external_id` it puts in the bundle, so enrolling under any other
  ID cannot succeed. `app_identity_device_id()` returns it ahead of the
  hostname, and it is **kept** after handoff (unlike the URL and password)
  because it becomes the certificate's CN.
- ~~**Re-provisioning an already-enrolled device to a different tenant or
  external ID.**~~ **Resolved (hit on the first real run):** the C6 still held
  its tedge-dev05 certificate, skipped enrollment and got CONNACK 5 from the
  new tenant. `tedge_set_enroll_otp()` now discards the stored certificate —
  a supplied password *is* the signal that the device is being onboarded
  again. Original note: `tedge_auth_prepare()` uses any stored certificate as-is, so
  a device holding a certificate for tenant A that is re-provisioned for tenant
  B keeps presenting A's certificate and never enrolls with B's password. The
  handoff needs a way to tell the module "forget the old certificate" when the
  tenant or external ID changes — a new public API (e.g.
  `tedge_reset_enrollment()`), since only the module may touch `tedge/`. Out of
  scope for the first delivery; task 7.6 will fail until it exists, and should
  be re-scoped or split when it is reached.
- Is a second PSA key acceptable within the ITS budget on the 4 MB boards, or
  should the ZTP identity key be non-persistent and re-generated per
  provisioning session — accepting that the server sees a new public key each
  time, which interacts with its `key_mismatch` rejection?
