## Why

Once an operator has several of these boards on a bench or in a cabinet, the
cloud's device list no longer says which physical board is which: they look the
same, and the external ID is printed nowhere on them. Matching the two today
means reading a serial console or unplugging boards one at a time. The device
should be identifiable from both ends: the cloud asks a device to show itself,
and an operator at the device makes it announce itself in the cloud.

## What Changes

- **Cloud → device: `tedge identify` over the shell command.** A new shell
  subcommand, `tedge identify [seconds]`, hung off the client's `tedge` root,
  blinks the status LED in the existing identify pattern (fast 5 Hz blink;
  white on an RGB LED) for 30 s by default, or for the given number of seconds
  (capped). The cloud triggers it with the existing `c8y_Command` operation
  (the Shell tab in Device Management), so no new operation type is added. The
  command returns as soon as the blinking starts, so the operation never holds
  up the next one. `tedge identify` is added to the shell-command allow-list of
  the images that carry the shell command. It works from a local console too.
- **Device → cloud: an identify button gesture.** A new `sw0` gesture, exactly
  two short presses by default (`APP_IDENTIFY_PRESS_COUNT`), sends a
  `zephyr_Identify` event to the cloud and acknowledges the press on the LED.
  The device keeps serving; the gesture never changes the device's mode. The
  provisioning pattern (three presses) and the erase hold are unchanged, and a
  build refuses the same press count for both patterns.
- **The `sw0` button no longer depends on the provisioning hand-off.** Today
  the button is only watched when `APP_PROV_HANDOFF` is built in; the identify
  gesture needs it in every tedge-* image with a button.
- **tedge-zephyr: a failed shell command is reported as failed.** Testing
  `tedge identify 0` on an S3-DevKitC left the operation EXECUTING: the
  Zephyr shell wraps `shell_error()` text in ANSI colour codes, the client
  passed them through into the `505` status, and Cumulocity refused the
  message ("contains invalid characters"). Any command that fails through
  `shell_error()` was affected. The client now strips escape sequences from
  captured output, and SmartREST quoting drops any remaining control
  character (line breaks stay, inside the quoted field). Fixed here because
  identify's own failure cases depend on it.
- **Cumulocity: an events Smart Function.** On the MQTT Service transport
  Cumulocity stores only what a Smart Function maps, and nothing maps
  `te/device/*///e/*` yet, so no tedge-zephyr event reaches the tenant today. A
  new `cumulocity/smart-functions/tedge-zephyr-events/` maps every event type
  to a Cumulocity event on the device that sent it; the identify event is the
  first user.

Phase: 3 (tedge-zephyr device management).

Targets: the tedge-* images of all four applications (modbus-server,
opcua-server, snmp-agent, tedge-agent) on the tedge boards: ESP32-DevKitC
(WROOM-32), ESP32-CAM, ESP32-C6-DevKitC, ESP32-S3-DevKitC-1 and QT Py ESP32-S3.
No industrial-protocol behaviour changes. The cloud half needs a status LED and
an image with the shell command (`CONFIG_TEDGE_SHELL_COMMAND`, today the
S3-DevKitC and QT Py modbus-server and tedge-agent full images, which add
`extras/shell-diagnostics.conf`); the button half needs `sw0` (the BOOT button
on these boards) and works on every tedge-* image. A board or image missing
either builds and runs with that half absent.

## Non-goals

- A new LED pattern or colour for identify: the existing identify pattern from
  the Improv provisioner is reused.
- A dedicated identify operation (`c8y_Identify` or similar) or a Cumulocity UI
  plugin: the shell command is the cloud entry point.
- Bringing the shell command to more images. Where it does not fit today
  (WROOM, ESP32-CAM, the C6, the snmp-agent images) cloud identify is absent
  until the shell does fit; that is a separate footprint question.
- Identifying a device that is not connected to the cloud (the event is sent
  through the normal telemetry queue and is lost if the queue overflows before
  the device reconnects, like any other event).
- Configuring the identify duration or gesture at runtime (Kconfig only).
- The thin-edge.io gateway transport.

## Kconfig options added

| Option | Where | Default | Purpose |
|---|---|---|---|
| `APP_IDENTIFY` | `lib/common` | `y` when `TEDGE` | the `tedge identify` subcommand (with `SHELL`) and the button gesture |
| `APP_IDENTIFY_DURATION_S` | `lib/common` | 30 (range 1–300) | blink time when the command names none |
| `APP_IDENTIFY_PRESS_COUNT` | `lib/common` | 2 (range 2–6) | short presses in the identify gesture |

No new `CONFIG_TEDGE_*` option and no public `tedge_*` API change; the
tedge-zephyr fix is internal. The existing
`CONFIG_TEDGE_SHELL_COMMAND_ALLOW_LIST` gains `tedge identify` in
`extras/shell-diagnostics.conf`.

## Resource constraints

The cost must stay small enough not to matter on the tightest image (the
ESP32-CAM and WROOM ota profiles): a gesture branch, an event call and a few
bytes of state, plus one shell subcommand on the images that already have the
shell. The shell itself is not added anywhere. Expected well under 1 KiB flash and under 64 B RAM per
image; the measured flash/RAM delta per target board goes into the footprint
table (`device-management-features`, "Per-feature footprint is measured and
documented") and must pass the size gate against the baseline.

## Capabilities

### New Capabilities

- `device-identify`: identifying a physical device from the cloud (the
  `tedge identify` command run through `c8y_Command`, and its LED pattern) and the cloud device from the
  physical one (the button gesture, the `zephyr_Identify` event and the events
  Smart Function that delivers it).

### Modified Capabilities

- `tedge-diagnostics`: "A commanded shell is allow-listed and off by
  default" also forbids escape sequences and control characters in reported
  output, so a failed command always ends FAILED.
- `wifi-provisioning`: "Button-triggered re-provisioning and erase" gains a
  third gesture that does not change the device's mode, and its "other presses
  have no effect" scenario no longer lists two presses.

## Impact

- `lib/common/`: `button_gesture.{c,h}` (new gesture, host tests in
  `tests/button_gesture/`), the `sw0` wiring split out of `prov_handoff.c`, a
  new `identify.{c,h}`, `Kconfig`, `CMakeLists.txt`.
- `lib/common/tedge-boards/extras/shell-diagnostics.conf`: allow-list entry,
  and the shell-diagnostics measurements in DEVICES.md re-checked.
- `tedge-zephyr/src/tedge_shell_cmd.c`, `tedge_shell_allow_list.c`
  (`tedge_shell_clean_output()`), `tedge_smartrest.c`, `tedge_internal.h`,
  and unit tests: the failed-command fix.
- `cumulocity/smart-functions/tedge-zephyr-events/` (new) and
  `cumulocity/README.md`.
- README / DEVICES.md: how to trigger identify from the cloud (Shell tab or
  `c8y_Command`) and from the button, and which images support each.
- RGB LED boards (C6, S3-DevKitC-1, QT Py) use the RGB backend of the
  `rgb-status-led` change, which is already implemented (only its archive is
  pending).
