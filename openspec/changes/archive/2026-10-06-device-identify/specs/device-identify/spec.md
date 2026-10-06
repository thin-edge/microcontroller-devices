## ADDED Requirements

### Requirement: The cloud can make a device show itself on its LED

A tedge-* image built with `APP_IDENTIFY` and the Zephyr shell SHALL provide a
`tedge identify [seconds]` shell subcommand under the client's `tedge` root.
It SHALL show the identify pattern on the status LED (the fast blink, white on
an RGB LED) for the given number of seconds, or `APP_IDENTIFY_DURATION_S`
(default 30) when none is given, and then return to the pattern shown before.
A value above 300 SHALL be capped at 300; a value that is not a positive
integer, or a board with no status LED, SHALL make the command fail with a
message saying why, and leave the LED unchanged. The command SHALL return as
soon as the blinking starts, printing the duration. Running it again while the
LED is blinking SHALL restart the blinking with the new duration.

The cloud SHALL trigger it through the existing `c8y_Command` operation: every
shipped image that carries the shell command (`CONFIG_TEDGE_SHELL_COMMAND`)
SHALL list `tedge identify` on its allow-list. No other operation type SHALL be
added for identify.

#### Scenario: Identify from the Shell tab

- **WHEN** an operator sends `tedge identify` as a `c8y_Command` to a
  connected device whose image carries the shell command
- **THEN** the status LED fast-blinks for about 30 s and then returns to its
  connected pattern
- **AND** the operation succeeds within a few seconds, before the blinking
  ends, with the command's output (the duration) as its result

#### Scenario: Identify with a duration

- **WHEN** the command is `tedge identify 10`
- **THEN** the status LED fast-blinks for about 10 s

#### Scenario: Duration out of range

- **WHEN** the command is `tedge identify 3600`
- **THEN** the LED blinks for 300 s and the operation succeeds
- **WHEN** the command is `tedge identify 0` or `tedge identify soon`
- **THEN** the LED does not change and the operation fails with the reason

#### Scenario: Board without a status LED

- **WHEN** `tedge identify` runs on a board with neither `led0` nor
  `led-strip`
- **THEN** the operation fails with "this board has no status LED"

#### Scenario: Image without the shell command

- **WHEN** the image is built without `CONFIG_TEDGE_SHELL_COMMAND`
- **THEN** it builds and runs, the cloud cannot trigger identify, and the
  button half still works

### Requirement: An operator at the device can make it announce itself in the cloud

A tedge-* image built with `APP_IDENTIFY` on a board with `sw0` SHALL recognize
an **identify gesture**: exactly `APP_IDENTIFY_PRESS_COUNT` (default 2) short
presses within `APP_WIFI_PROV_PRESS_WINDOW_MS`, followed by the same quiet gap
that ends the provisioning pattern. On the gesture the device SHALL publish an
event of type `zephyr_Identify` whose text names the device's hostname and
how it was triggered, and SHALL acknowledge the gesture on the status LED (the
identify pattern for about 1 s). The gesture SHALL NOT change the device's mode
or reboot it. The build SHALL fail when `APP_IDENTIFY_PRESS_COUNT` equals
`APP_WIFI_PROV_PRESS_COUNT`. Gestures recognized less than 5 s apart SHALL
send one event, not one each.

#### Scenario: Double press sends an event

- **WHEN** the operator presses `sw0` twice in quick succession on a connected
  device
- **THEN** the LED acknowledges the press
- **AND** within a few seconds a `zephyr_Identify` event appears on that
  device in the cloud, so the operator can tell which cloud device this board
  is
- **AND** the device keeps serving its industrial protocol throughout

#### Scenario: Gesture while offline

- **WHEN** the gesture is made while the device is not connected
- **THEN** the event is queued like any other event and sent after the
  device reconnects, if the queue has not overflowed

#### Scenario: Repeated presses do not flood the cloud

- **WHEN** the operator makes the identify gesture three times within 5 s
- **THEN** one `zephyr_Identify` event is sent

#### Scenario: Board without sw0

- **WHEN** the image runs on a board without `sw0`
- **THEN** it builds and runs, and only the operation half of identify is
  present

### Requirement: Device events reach Cumulocity on the MQTT Service transport

The repository SHALL ship a Smart Function, `tedge-zephyr-events`, that maps
every `te/device/<id>///e/<type>` message to a Cumulocity event of type
`<type>` on the device whose external ID (`c8y_Serial`) is the MQTT client ID,
with the payload's `text`, its `time` when present (the message time
otherwise), and any further payload fields kept as fragments. Its tests SHALL
run under `cumulocity/smart-functions/test.py`, and `cumulocity/README.md`
SHALL list it with the other functions.

#### Scenario: Identify event is mapped

- **WHEN** the device publishes `{"text":"Identify button pressed on
  tedge-abc123"}` on `te/device/abc123///e/zephyr_Identify`
- **THEN** the tenant has an event of type `zephyr_Identify` with that text on
  the device with external ID `abc123`

#### Scenario: Any other event type is mapped too

- **WHEN** the device publishes on `te/device/<id>///e/app_test`
- **THEN** an `app_test` event is created on that device, not dropped
