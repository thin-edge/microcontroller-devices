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

## MODIFIED Requirements

### Requirement: Improv Wi-Fi BLE protocol

In an Improv build (`APP_PROV_IMPROV`, the default) the device SHALL, in
provisioning mode, act as a BLE peripheral implementing the Improv Wi-Fi BLE
service (UUID `00467768-6228-2272-4663-277478268000`) with its
current-state, error-state, RPC-command, RPC-result and capabilities
characteristics, and SHALL include the service UUID and the Improv service data
in its advertisement, so unmodified Improv clients can discover and provision
it. It SHALL accept the "send Wi-Fi settings" and "identify" RPCs, SHALL
reassemble an RPC frame written across several writes, and SHALL reject a frame
with a bad checksum, a malformed length, an SSID longer than 32 bytes or a
password longer than 64 bytes with the "invalid RPC" error. It SHALL reject an
unknown command with the "unknown RPC" error. The advertised BLE device name
SHALL be the unique network hostname the application uses, taken from the
identity record the application leaves in storage; before any application has
run, the provisioner SHALL use a generic MAC-derived name.

#### Scenario: Discovered by a standard Improv client

- **WHEN** a device is in provisioning mode and a user opens an Improv BLE
  client (e.g. the Improv web page in a Web-Bluetooth browser)
- **THEN** the device appears under its unique hostname and the client can
  connect and read its state

#### Scenario: Corrupt RPC

- **WHEN** the client writes an RPC frame whose checksum does not match
- **THEN** the device notifies error state "invalid RPC" and stays in its
  current provisioning state

#### Scenario: Identify

- **WHEN** the client sends the identify RPC
- **THEN** the device fast-blinks its status LED for about 10 s (on boards with
  `led0`), and the capabilities characteristic advertises identify support only
  on such boards
