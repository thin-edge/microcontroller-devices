## MODIFIED Requirements

### Requirement: Wi-Fi station bring-up

On Wi-Fi targets the firmware SHALL join the configured network in station mode
on boot and report the network as connected once an IPv4 address is assigned, so
protocol frontends can begin serving. The credentials SHALL be taken from the
first source that provides them: persisted credentials, then the compile-time
`CONFIG_APP_WIFI_SSID`/`PSK`. When no source provides credentials, an
application on a board with a provisioner SHALL set the boot request and reboot
into the provisioner (see the `wifi-provisioning` and `boot-layout`
capabilities); on a board without one it SHALL log that the SSID is unset and
stay offline, as before.

#### Scenario: Connect on boot

- **WHEN** the device boots with valid Wi-Fi credentials
- **THEN** it associates and obtains an IPv4 address
- **AND** the connected state becomes true and frontends start serving

#### Scenario: Connect with provisioned credentials

- **WHEN** a device boots with credentials in persistent storage
- **THEN** it joins that network in station mode, even if the image also
  carries different compile-time credentials

#### Scenario: No credentials on a board with a provisioner

- **WHEN** the application boots with no stored and no compile-time
  credentials on a board that has a provisioner
- **THEN** it reboots into the provisioner instead of logging an error and
  staying offline

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
hold is armed. The indicator SHALL use the board's LED alias,
be enable/disable-able via Kconfig, and be a no-op on boards that define no LED
(and on native_sim).

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

#### Scenario: No LED present

- **WHEN** the board defines no status LED (or the indicator is disabled)
- **THEN** the firmware runs normally with no LED action (e.g. display-only boards
  continue to show status on the TFT)
