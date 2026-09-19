## MODIFIED Requirements

### Requirement: Connectivity status indicator (LED)

On boards with a status LED, the firmware SHALL reflect connectivity on it so an
operator can tell at a glance whether the device is on the network: the LED SHALL
**blink while not connected** (booting/associating/reconnecting) and be **steady
once connected** (an IPv4 address is assigned and the frontend is serving). In
**provisioning mode** it SHALL repeat a pattern of **two short blinks followed by
a pause** (about 100 ms on, 150 ms off, 100 ms on, then about 1.5 s off), which
is distinct from both. It SHALL be **off** while provisioning is idle (the
window has expired). It SHALL **fast-blink** for an identify request and to
acknowledge a recognized button pattern, and it SHALL **flicker** while an erase
hold is armed. The indicator SHALL use the board's plain LED alias (`led0`) or,
when the board has none, its addressable RGB LED (`led-strip`), be
enable/disable-able via Kconfig, and be a no-op on boards that define neither
(and on native_sim). On an RGB LED each state SHALL also have its own colour:
green connected, amber not connected, blue provisioning, white identify and
acknowledgement, red erase armed.

#### Scenario: Disconnected shows a blinking LED

- **WHEN** the device is not connected (no IPv4 address / (re)connecting)
- **THEN** the status LED blinks
- **AND** an operator can distinguish a device-side network problem from a
  collector-side one without tools

#### Scenario: Connected shows a steady LED

- **WHEN** the device is connected with an IPv4 address and serving
- **THEN** the status LED is steady (not blinking)

#### Scenario: Provisioning mode is distinguishable

- **WHEN** the provisioner is running, waiting for credentials
- **THEN** the status LED repeats two short blinks followed by a pause
- **AND** an operator can tell it apart from the even "not connected" blink
  and the steady "connected" light

#### Scenario: RGB LED shows the state in colour

- **WHEN** the board's status LED is an addressable RGB LED (e.g. the ESP32-C6
  or ESP32-S3-DevKitC-1)
- **THEN** it shows the same patterns as a plain LED, in green when connected,
  amber when not connected, blue in provisioning mode, white for identify and
  acknowledgement, and red while an erase is armed

#### Scenario: No LED present

- **WHEN** the board defines no status LED (or the indicator is disabled)
- **THEN** the firmware runs normally with no LED action (e.g. display-only boards
  continue to show status on the TFT)
