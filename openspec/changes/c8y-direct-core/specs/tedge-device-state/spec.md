## ADDED Requirements

### Requirement: Inventory and supported operations on every connect

On every connect the client SHALL publish the device's name and type, its
required interval, and a supported-operations list assembled from the features
compiled into the image and the operations the application registered. An
operation received for anything not in that list SHALL be marked failed with a
reason.

#### Scenario: Restart built in

- **WHEN** an image with the restart feature connects
- **THEN** its supported operations include `c8y_Restart`

#### Scenario: Application-registered operation

- **WHEN** the application registers an operation before the client starts
- **THEN** it is advertised as supported, and its handler receives the
  operation with the operation already marked as executing

### Requirement: Restart runs through the application and a full-system reset

On a restart operation the client SHALL call the application's restart hook,
if any. When the hook vetoes it, the client SHALL mark the operation failed with
the hook's reason. Otherwise it SHALL mark the operation as executing, persist
a marker, disconnect cleanly and reset the device, using the application's
reset hook when provided and otherwise a full-system reset of the platform.
After the reboot it SHALL mark the operation successful once connected, and
clear the marker.

#### Scenario: Restart from Cumulocity

- **WHEN** an operator restarts the device from Cumulocity
- **THEN** the device reboots, reconnects and the operation ends SUCCESSFUL

#### Scenario: ESP32-C6 restart does not hang

- **WHEN** the client resets an ESP32-C6 built with MCUboot and Wi-Fi running,
  without an application reset hook
- **THEN** it uses a full-system reset, and MCUboot boots the application

### Requirement: State is published as twin data and health on thin-edge.io topics

The client SHALL publish its own twin fragment (`tedge_Agent`: module name,
version and transport) and its health (`up`) on every connect, on
`te/device/<id>///twin/tedge_Agent` and
`te/device/<id>/service/tedge-zephyr/status/health` when connected to the MQTT
Service. On Core MQTT it SHALL send the twin fragment as a direct inventory
update. The application SHALL be able to publish its own twin fragments through
the API, which the client republishes after every reconnect.

#### Scenario: Application twin fragment survives a reconnect

- **WHEN** the application publishes a twin fragment and the connection is
  lost and re-established
- **THEN** the client publishes the fragment again after the reconnect

### Requirement: Reference Smart Functions ship with the module

The module SHALL include reference Cumulocity Smart Functions that map its
`te/` twin and health messages onto the device's managed object, with
instructions for installing them on a tenant.

#### Scenario: Twin fragment appears in the inventory

- **WHEN** the reference twin function is installed and a device publishes
  `tedge_Agent`
- **THEN** the device's managed object shows a `tedge_Agent` fragment with the
  published values
