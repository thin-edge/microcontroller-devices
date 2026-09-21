## ADDED Requirements

### Requirement: The provisioning image speaks one protocol, chosen at build time

The provisioning image SHALL implement exactly one provisioning protocol,
selected by the Kconfig choice `APP_PROV_PROTOCOL`: `APP_PROV_IMPROV` (the
default) or `APP_PROV_ZTP`. The image SHALL NOT register the GATT services of
both protocols, and SHALL NOT advertise more than one 128-bit service UUID.
Application images SHALL continue to contain no Bluetooth under either
selection.

#### Scenario: Default build is unchanged

- **WHEN** a provisioning image is built without setting `APP_PROV_PROTOCOL`
- **THEN** it speaks Improv Wi-Fi and behaves exactly as before this change

#### Scenario: ZTP build advertises only the ZTP service

- **WHEN** a provisioning image is built with `APP_PROV_ZTP`
- **THEN** its advertisement carries the ZTP service UUID and the local name
  `ztp`, the Improv service is absent from the GATT table, and the
  advertisement fits the 31-byte primary PDU

### Requirement: The device serves the ZTP GATT service as a peripheral

With `APP_PROV_ZTP` selected, the device SHALL advertise and serve the ZTP
service `6e400001-b5a3-f393-e0a9-e50e24dcca9e` with four characteristics:
`6e400002-…` request (write, write-without-response), `6e400003-…` response
(read, notify), `6e400004-…` status (read, notify) and `6e400005-…` timesync
(write). Messages on the request and response characteristics SHALL be framed
as a 2-byte big-endian length followed by that many payload bytes, repeated,
with a zero length marking end-of-message. The status characteristic SHALL
carry a single byte: 0 idle, 1 relaying, 2 done, 3 error.

#### Scenario: A stock ZTP relay discovers the device

- **WHEN** an unmodified `lab-ztp-provisioner` relay scans while the device is
  in ZTP provisioning mode
- **THEN** the device matches on its advertised service UUID, the relay
  connects and discovers all four characteristics, and no device-specific
  handling is required in the relay

#### Scenario: Status reflects progress

- **WHEN** the relay writes the kick and the device begins producing its
  envelope
- **THEN** status notifies 1 (relaying), then 2 (done) once a bundle has been
  applied, or 3 (error) if the exchange fails

### Requirement: The device publishes a signed enrollment envelope on demand

The device SHALL treat a zero-length fragment written to the request
characteristic with no preceding payload as a request to enroll. On receiving
it the device SHALL generate an ephemeral P-256 key pair, build an
`EnrollRequest`, sign it with its persistent ZTP identity key, and notify the
resulting `SignedEnvelope` JSON on the response characteristic. The signed
bytes SHALL be RFC 8785 canonical JSON, emitted with object members in
lexicographic order. The envelope's `alg` SHALL be `ecdsa-p256-sha256`.

The identity key SHALL be a P-256 key persisted in PSA secure storage at
`CONFIG_APP_PROV_ZTP_PSA_KEY_ID`, distinct from `CONFIG_TEDGE_PSA_KEY_ID`, and
SHALL be generated on first use. The device SHALL NOT export the private key.

#### Scenario: Envelope is byte-identical to the reference canonicaliser

- **WHEN** the device builds an `EnrollRequest` with a known field set
- **THEN** the signed bytes match, byte for byte, the output of the ZTP
  project's `Canonicalize` for the same input

#### Scenario: Identity is stable across provisioning sessions

- **WHEN** a device is provisioned, reboots, and is later put back into
  provisioning mode
- **THEN** it presents the same `public_key`, and the server does not report a
  key mismatch

#### Scenario: Ephemeral key is fresh per session

- **WHEN** two enrollment attempts occur
- **THEN** each carries a different `ephemeral_p256` value and a different
  `nonce`

### Requirement: The device applies a bundle delivered as a text manifest

The device SHALL request `response_format: "text"` and accept the server's
response in its text rendering, written to the request characteristic using
the same framing. It SHALL parse the response's `key=value` records —
`status`, `reason`, `retry_after`, `server_time` and `manifest.payload` — and
then parse the manifest obtained by base64-decoding `manifest.payload`,
reading `module=<type> <base64>` and
`module-sealed=<type> <format> <ephemeral_pub> <nonce> <ciphertext>` lines.
At both levels it SHALL ignore keys it does not recognise, including the
`bundle.*` records. The device SHALL NOT require a JSON parser to consume the
response.

A sealed module SHALL carry the algorithm `p256-hkdf-sha256-chacha20poly1305`
and SHALL be opened by P-256 ECDH against the manifest's `ephemeral_pub`
using the session's ephemeral private key, HKDF-SHA256 over the shared secret
with an empty salt and the info string `ztp/seal/v1` to derive the key, then
ChaCha20-Poly1305 decryption with the given nonce. A module whose
authentication tag does not verify SHALL abort the whole bundle.

#### Scenario: Device opens the reference sealed vector

- **WHEN** the device's unsealing path is given the key, ephemeral point,
  nonce and ciphertext from `testdata/vectors/seal_p256.json` in
  `lab-ztp-provisioner`
- **THEN** it recovers exactly the vector's plaintext

#### Scenario: Pending response is read from the text rendering

- **WHEN** the server answers `status=pending` with a `retry_after` record
- **THEN** the device stays in provisioning mode and applies nothing, and it
  uses `server_time`, when present, to correct the timestamp of its next
  envelope

#### Scenario: Sealed Cumulocity module is opened

- **WHEN** the manifest contains a `module-sealed=c8y.v2 raw …` line addressed
  to the session's ephemeral key
- **THEN** the device recovers the plaintext INI payload and the relay never
  held anything but ciphertext

#### Scenario: Tampered ciphertext is rejected

- **WHEN** any byte of a sealed module's ciphertext or nonce is altered in
  transit
- **THEN** the AEAD tag fails, the device applies no module from that bundle,
  sets status to error, and retains its previous credentials

#### Scenario: Oversized bundle fails closed

- **WHEN** the manifest exceeds `CONFIG_APP_PROV_ZTP_RX_BYTES`
- **THEN** the device abandons the exchange, sets status to error, and retains
  its previous credentials, rather than applying a truncated bundle

### Requirement: Modules are dispatched by type and unknown types are skipped

The device SHALL dispatch each module to an applier selected by the module's
type string, through a registration table. A module whose type has no
registered applier SHALL be skipped without failing the bundle. The first
implementation SHALL register `wifi.v2` and `c8y.v2`.

#### Scenario: Unknown module does not fail provisioning

- **WHEN** a bundle contains `ssh.authorized_keys.v2` alongside `wifi.v2`
- **THEN** the Wi-Fi module is applied, the SSH module is skipped, and
  provisioning succeeds

### Requirement: Wi-Fi credentials from a bundle are verified before they are stored

The `wifi.v2` applier SHALL parse the INI payload's `[network]` sections and,
before storing anything, join the network to verify the credentials, reusing
the same verification path as Improv provisioning. Credentials SHALL be
persisted only after a successful join. The Cumulocity module SHALL be
committed only after the Wi-Fi module has succeeded, so a failed bundle never
leaves the device with a tenant but no network.

#### Scenario: Unreachable network leaves the device untouched

- **WHEN** a bundle names an SSID the device cannot join
- **THEN** no Wi-Fi credentials and no Cumulocity fields are stored, the device
  stays in provisioning mode, and status reports error

### Requirement: Onboarding data is handed to the application through the provisioner's own settings subtree

The provisioning image SHALL write the `c8y.v2` fields to
`prov/c8y/url`, `prov/c8y/tenant`, `prov/c8y/external_id` and `prov/c8y/otp` in
the shared `storage` partition. It SHALL NOT write to the `tedge/` settings
subtree and SHALL NOT link the `tedge-zephyr` module.

The application image SHALL read those keys at boot, pass the tenant host and
one-time password to the client through its public API, and use the external ID
as the device identity. It SHALL delete the URL, tenant and one-time password
once the client has accepted them. It SHALL keep the external ID: the server
issued the one-time password for that ID and the Cumulocity certificate is
issued for it, so it remains the device's identity on every later boot. The
bundle's external ID SHALL take precedence over the application's own
hostname-derived default, since a password registered for one ID cannot enroll
another.

#### Scenario: Provisioned device connects without a console

- **WHEN** a device is provisioned over ZTP with a bundle carrying a tenant and
  a one-time password, and then reboots into the application
- **THEN** the application configures the client from `prov/c8y/*`, the client
  enrolls with the supplied password, and no registration URL needs to be read
  from the console

#### Scenario: Consumed keys do not linger

- **WHEN** the application has handed `prov/c8y/*` to the client
- **THEN** the URL, tenant and one-time password are deleted, and a later
  reboot does not re-apply a stale one-time password

#### Scenario: The issued external ID stays the device's identity

- **WHEN** a device enrolled with a ZTP-issued external ID reboots
- **THEN** it connects under that same external ID, not its hostname-derived
  default, and its certificate still matches

#### Scenario: A rejected handoff is retried, not lost

- **WHEN** the client refuses the supplied tenant or password
- **THEN** the data stays stored and the next boot tries again, rather than
  the one-time password being discarded

#### Scenario: The provisioner stays independent of the client module

- **WHEN** the provisioning image is built
- **THEN** it links no symbol from `tedge-zephyr`, and the module contains no
  reference to ZTP or to BLE

### Requirement: The bundle's authenticity is trusted on first use, with pinning available

When `CONFIG_APP_PROV_ZTP_SERVER_PUBKEY` is empty the device SHALL accept the
manifest without verifying a server signature, and SHALL log a warning naming
this as trust-on-first-use. The device SHALL NOT treat an unverified bundle as
verified. The option SHALL exist so that a later change can require a valid
server signature without a wire-format change.

#### Scenario: TOFU mode warns

- **WHEN** a device with no pinned server key applies a bundle
- **THEN** it logs a warning that the bundle was not verified, and applies it

### Requirement: The device corrects its clock from the relay before enrolling

The device SHALL accept an RFC 3339 UTC timestamp written to the timesync
characteristic and use it to compute a clock offset for the `timestamp` field
of its enrollment envelope. It SHALL apply the value as an offset only and
SHALL NOT set a wall clock that would conflict with SNTP after the network
comes up.

#### Scenario: A device with no NTP produces an acceptable timestamp

- **WHEN** a device that has never synced time is provisioned and the relay
  writes the current time before the kick
- **THEN** the envelope's timestamp falls inside the server's skew window and
  the request is not rejected as stale

### Requirement: A rejected or pending enrollment leaves the device provisionable

When the server's response is not an accepted bundle, the device SHALL remain
in provisioning mode and SHALL build a fresh envelope, with a new timestamp and
nonce, for the relay's next attempt. It SHALL NOT store partial state from an
unaccepted response.

#### Scenario: Operator approves after an initial pending result

- **WHEN** the first attempt returns pending, the operator approves the device
  in the ZTP server, and the relay retries
- **THEN** the second attempt carries a fresh timestamp, returns a bundle, and
  provisioning completes without reflashing or power-cycling the device
