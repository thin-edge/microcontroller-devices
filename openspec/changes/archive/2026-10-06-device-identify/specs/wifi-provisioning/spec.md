## MODIFIED Requirements

### Requirement: Button-triggered re-provisioning and erase

In station mode the firmware SHALL monitor the board's `sw0` button for two
distinct gestures that change the device's mode, plus the identify gesture
(`device-identify`), which does not. The **provisioning pattern** is exactly
`APP_WIFI_PROV_PRESS_COUNT` (default 3) short presses, completed within
`APP_WIFI_PROV_PRESS_WINDOW_MS` (default 2000 ms). When it is recognized, the
application SHALL acknowledge it on the status LED, set the boot request, and
reboot into the provisioner, keeping its stored credentials. The **erase
gesture** is one continuous hold of at least `APP_WIFI_PROV_ERASE_HOLD_S`
(default 10 s). On release the application SHALL delete all stored
credentials, set the boot request, and reboot, and the status LED SHALL show that
the erase is armed before the button is released. No other press sequence,
the identify gesture included, SHALL change the device's mode. The `sw0`
button SHALL be watched whenever the image has the provisioning hand-off or
the identify feature, independent of each other. A board without `sw0` SHALL build and run with
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

- **WHEN** `sw0` is pressed once, more times than the pattern, too slowly to
  fit the window, or held for less than the erase hold time
- **THEN** the device keeps serving and no request is recorded

#### Scenario: Identify gesture does not change the mode

- **WHEN** `sw0` is pressed exactly `APP_IDENTIFY_PRESS_COUNT` times (default
  2) within the window
- **THEN** the device keeps serving, no boot request is recorded, and only the
  identify event is sent
