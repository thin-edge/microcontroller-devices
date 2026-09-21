# wifi-provisioning Specification

## Purpose
How a device gets onto a Wi-Fi network without a rebuild: a separate provisioning image, launched when the application has no credentials or on a button gesture, that receives credentials over BLE, verifies them by joining before it stores them, and hands back to the application. Bluetooth never runs in an application image. This spec covers the Improv Wi-Fi protocol; the lab-ztp-provisioner protocol, which also carries Cumulocity onboarding, is the `ztp-provisioning` capability.
## Requirements
### Requirement: Provisioning runs in a separate image

BLE provisioning SHALL be implemented by a dedicated provisioning image (the
provisioner), stored in its own flash partition and launched by the bootloader.
Application images (OPC-UA, Modbus, SNMP) SHALL contain no Bluetooth host or
controller code. One provisioner build per board SHALL serve every
application. Boards without a BLE radio SHALL have no provisioner and keep
using compile-time credentials.

#### Scenario: Application images carry no Bluetooth

- **WHEN** any application is built for a BLE-capable board
- **THEN** its image has `CONFIG_BT` unset and no Bluetooth RAM reservation
- **AND** its libc heap is within a few KB of the same application built
  before this change

#### Scenario: One provisioner for all applications

- **WHEN** the device runs the Modbus application and is later reflashed with
  the OPC-UA application
- **THEN** the same provisioner image in the `prov` partition provisions it,
  advertising the OPC-UA application's hostname and URL

#### Scenario: Board without BLE

- **WHEN** the firmware is built for a board with no BLE radio (e.g. the
  ESP32-S2 Feather)
- **THEN** no provisioner is built for it and the documentation lists the
  board as not supporting BLE provisioning

### Requirement: Entering provisioning mode

The device SHALL boot the provisioner when a boot request for it is set (see
the `boot-layout` capability), and the application otherwise. The application
SHALL set the boot request and reboot when no Wi-Fi credentials resolve
(neither stored nor compile-time). The firmware SHALL NOT require the operator
to hold a button through reset, because the button is the SoC's
boot-strapping pin.

#### Scenario: Fresh device with no credentials

- **WHEN** a device with an application built without compile-time credentials
  boots with empty credential storage
- **THEN** the application sets the boot request and reboots, and the
  provisioner starts and advertises the Improv BLE service
- **AND** no protocol frontend runs

#### Scenario: Device with credentials boots normally

- **WHEN** a device boots with stored or compile-time credentials and no
  boot request
- **THEN** the bootloader boots the application, which serves without any
  Bluetooth

### Requirement: Button-triggered re-provisioning and erase

In station mode the firmware SHALL monitor the board's `sw0` button for two
distinct gestures. The **provisioning pattern** is exactly
`APP_WIFI_PROV_PRESS_COUNT` (default 3) short presses, completed within
`APP_WIFI_PROV_PRESS_WINDOW_MS` (default 2000 ms). When it is recognized, the
application SHALL acknowledge it on the status LED, set the boot request, and
reboot into the provisioner, keeping its stored credentials. The **erase
gesture** is one continuous hold of at least `APP_WIFI_PROV_ERASE_HOLD_S`
(default 10 s). On release the application SHALL delete all stored
credentials, set the boot request, and reboot, and the status LED SHALL show that
the erase is armed before the button is released. No other press sequence
SHALL change the device's mode. A board without `sw0` SHALL build and run with
this trigger absent.

#### Scenario: Button pattern re-provisions and keeps the old network

- **WHEN** the operator presses `sw0` three times in quick succession, then
  abandons provisioning
- **THEN** the device reboots into the provisioner
- **AND** once the provisioning window expires it reboots into the
  application and rejoins the previously stored network

#### Scenario: Very long hold erases credentials

- **WHEN** the operator holds `sw0` for at least the erase hold time and
  releases it
- **THEN** the stored credentials are deleted and the device reboots into the
  provisioner

#### Scenario: Other presses have no effect in station mode

- **WHEN** `sw0` is pressed once, twice, more times than the pattern, too
  slowly to fit the window, or held for less than the erase hold time
- **THEN** the device keeps serving and no request is recorded

### Requirement: No automatic fallback to provisioning mode

A device with credentials (stored or compile-time) SHALL NOT enter provisioning
mode by itself because those credentials fail to connect, however long or
however often they fail. It SHALL keep applying the normal reconnect and
last-resort reboot behaviour. Provisioning mode SHALL be entered only when no
credentials resolve, or after the button pattern.

#### Scenario: Access point password changed

- **WHEN** the stored network rejects the device's credentials across many
  reconnect attempts and last-resort reboots
- **THEN** the device keeps trying to join that network in station mode and
  never advertises the provisioning service
- **AND** an operator at the device can use the button pattern to
  re-provision it

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

### Requirement: Credentials are verified before they are stored

On receiving Wi-Fi settings, the device SHALL move to the "provisioning" state
and try to join that network, using those credentials, with a bounded timeout.
It SHALL persist the credentials only after it has obtained an IPv4 address.
On success it SHALL notify the "provisioned" state, put the device's service URL
(`<scheme>://<hostname>.local:<port>`, from the identity record) in the RPC
result, give the client a bounded time to read it, clear the boot request, and
reboot into the application. On failure it SHALL
notify "unable to connect", leave any previously stored credentials unchanged,
and return to accepting settings.

#### Scenario: Correct credentials

- **WHEN** the client sends a valid SSID and password for a reachable network
- **THEN** the device obtains an IPv4 address, stores the credentials, and
  reports "provisioned" with a URL such as `opc.tcp://<hostname>.local:4840`
- **AND** it reboots into the application, which joins that network and
  serves its protocol

#### Scenario: Wrong password

- **WHEN** the client sends a password the access point rejects
- **THEN** within the connect timeout the device reports "unable to connect"
- **AND** nothing is written to credential storage, and the client can retry

### Requirement: Credential persistence

Accepted credentials SHALL be persisted in non-volatile storage (Zephyr
`wifi_credentials` on the settings backend in `storage_partition`) as a single
network entry that replaces any previous one. They SHALL survive a reboot, a
power cycle, and a reflash or OTA update of the application image, since
neither writes the storage partition.

#### Scenario: Survives a firmware update

- **WHEN** a provisioned device is reflashed with a new application image by
  writing only the application partition
- **THEN** after reboot it rejoins the provisioned network without being
  provisioned again

### Requirement: Credential precedence

On a provisioning-enabled build, the firmware SHALL resolve Wi-Fi credentials
in this order: stored credentials, then non-empty compile-time
`CONFIG_APP_WIFI_SSID`/`PSK`, then none (which leads to the provisioner).

#### Scenario: Stored credentials override a baked-in default

- **WHEN** an image built with compile-time credentials for network A is
  provisioned over BLE to network B
- **THEN** after reboot the device joins network B

#### Scenario: Erase falls back to the baked-in default

- **WHEN** the stored credentials of such a device are erased and the
  provisioning window then expires unused
- **THEN** the device reboots and joins network A from the compile-time
  credentials

### Requirement: Bounded provisioning window

Advertising in provisioning mode SHALL stop after a configurable window
(`APP_WIFI_PROV_WINDOW_S`). When the window expires and credentials resolve, the
provisioner SHALL clear the boot request and reboot into the application. When the window expires and no
credentials resolve, the device SHALL stop advertising and stay idle until a
press of `sw0` restarts the window.

#### Scenario: Unattended fresh device stops advertising

- **WHEN** a device with no credentials is left in provisioning mode past the
  window
- **THEN** it stops advertising, and its LED shows the idle pattern
- **AND** a press of `sw0` makes it advertise again for a new window

### Requirement: Optional physical-presence authorization

When `APP_WIFI_PROV_REQUIRE_AUTH` is enabled, the device SHALL start in the
Improv "authorization required" state and SHALL refuse Wi-Fi settings with the
"not authorized" error until `sw0` is pressed on the device. It SHALL then stay
"authorized" for a limited time (about 60 s).

#### Scenario: Settings refused before the button is pressed

- **WHEN** authorization is required and a client sends Wi-Fi settings without
  anyone pressing `sw0`
- **THEN** the device responds "not authorized" and does not try to connect

#### Scenario: Settings accepted after the button is pressed

- **WHEN** the operator presses `sw0` and the client then sends Wi-Fi settings
  within the authorization period
- **THEN** the device proceeds to test the credentials

### Requirement: Provisioning and serving are mutually exclusive

Bluetooth and a protocol frontend SHALL never run in the same boot: the
provisioner contains no protocol frontend and the applications contain no
Bluetooth. The provisioner SHALL have no connectivity-watchdog reconnects and
no last-resort offline reboot, and SHALL keep the liveness watchdog. Every exit
from the provisioner SHALL clear the boot request and reboot. A reset inside
the provisioner that happens before it clears the request (a crash, a watchdog
reset, a power cut) SHALL bring the device back into the provisioner.

#### Scenario: No last-resort reboot while waiting for credentials

- **WHEN** the provisioner waits longer than `APP_NET_REBOOT_TIMEOUT_S`
- **THEN** it does not reboot for being offline

#### Scenario: Bluetooth absent while serving

- **WHEN** the application is serving its protocol
- **THEN** the device does not advertise over BLE

#### Scenario: Power cut during provisioning

- **WHEN** power is lost while the provisioner is waiting for or testing
  credentials
- **THEN** on the next power-up the device boots the provisioner again

### Requirement: Board support is documented and verified

A board/application pair SHALL be declared as supporting BLE provisioning only
if the application and the provisioner both build and fit their partitions,
and the pair is verified end to end on hardware: provisioned from a standard
Improv client, then reachable over its protocol on the data path. For the
WROOM-32 OPC-UA pair this includes passing the existing connection-churn
stress test. The README SHALL list each pair as supported or unsupported,
giving the reason for an unsupported pair.

#### Scenario: A pair does not fit

- **WHEN** an application or the provisioner exceeds its partition or RAM, or
  the pair fails its stress test
- **THEN** that pair is documented as unsupported, naming the limit it
  exceeded, and is not claimed as supported

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

