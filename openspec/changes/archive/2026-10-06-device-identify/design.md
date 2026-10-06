## Context

The pieces mostly exist already:

- `lib/common/status_led.{c,h}` has `STATUS_LED_IDENTIFY` (5 Hz, white on an
  RGB LED) and `status_led_flash(mode, ms)`, which shows a pattern for a while
  and returns to the base pattern. The Improv provisioner already uses it for
  its identify RPC.
- `lib/common/button_gesture.{c,h}` is a host-tested classifier for `sw0`
  (N short presses, long hold). `prov_handoff.c` owns the GPIO, debounce and
  work item, and is only built and started under `CONFIG_APP_PROV_HANDOFF`
  (`net.c`).
- tedge-zephyr runs `c8y_Command` (`CONFIG_TEDGE_SHELL_COMMAND`,
  `tedge_shell_cmd.c`): an allow-listed Zephyr shell command executed
  in-process against the dummy backend on a thread of its own, its output
  returned as the operation result. The module defines one `tedge` shell root;
  applications hang subcommands off it with `SHELL_SUBCMD_ADD((tedge), …)`
  (`tedge diag`, `tedge event`, …). The allow-list is empty by default;
  `lib/common/tedge-boards/extras/shell-diagnostics.conf` fills it, and
  `release/devices.yml` adds that file to the S3-DevKitC and QT Py
  modbus-server and tedge-agent full images only (DEVICES.md, "Shell
  diagnostics measured").
- `tedge_publish_event()` goes to `te/device/<id>///e/<type>` on the MQTT
  Service transport or SmartREST `400` otherwise.

One gap surfaced while reading the code: **no event reaches the tenant on the
MQTT Service.** Only measurements, twin and firmware progress have Smart
Functions; `te/…/e/*` is unmapped.

## Goals / Non-Goals

**Goals:**

- `tedge identify` sent as a `c8y_Command` blinks the LED on any image that
  carries the shell command and has a status LED.
- A two-press gesture on `sw0` produces a `zephyr_Identify` event on the right
  cloud device, on any tedge-* image with `sw0`, with or without the
  provisioning hand-off.
- Keep `tedge-zephyr/` free of `lib/` knowledge (LED and button stay in the
  application side).

**Non-Goals:**

- A new LED pattern, a dedicated operation type, a UI plugin, runtime
  configuration, or bringing the shell to more images (see proposal).

## Decisions

### D1. The cloud trigger is a shell subcommand run through `c8y_Command`

`lib/common/identify.c` adds `SHELL_SUBCMD_ADD((tedge), identify, NULL,
"blink the status LED to find this board: identify [seconds]", cmd_identify,
1, 1)`. The operator uses the Shell tab (or `c8y operations create --template
"{c8y_Command:{text:'tedge identify'}}"`); nothing new reaches the module, the
supported-operations list or the tenant.

*Why:* the cloud already has a UI and an API for `c8y_Command`, the module
already handles its delivery, status and output, and the same command works on
a local console during bench work. A dedicated operation would need a UI
plugin to be usable from Device Management.

*Consequence:* cloud identify exists only where the shell command does. On the
WROOM, ESP32-CAM, C6 and snmp-agent images the shell does not fit or is not
carried, and adding it for identify alone is out of proportion (DEVICES.md
measures the shell at several percent of flash, and on the C6 it starves the
Wi-Fi driver's RAM). On those images the button half still works. Revisit if
the shell gets cheaper or a smaller image grows room for it.

### D2. Return immediately, don't wait for the blink to end

`cmd_identify` calls `status_led_flash(STATUS_LED_IDENTIFY, s * 1000)`, prints
`identifying for <s> s` and returns 0. The shell command thread is single and
`TEDGE_SHELL_COMMAND_TIMEOUT_S` defaults to 30 s, so a command that waited for
the blink would time out at the default duration and block the next command.
A repeated command restarts the flash (that is `status_led_flash()`'s
behaviour already). Bad argument → `shell_error()` and `-EINVAL`; no status
LED (`!status_led_present()`) → `shell_error("this board has no status LED")`
and `-ENODEV`; both become the failure reason.

### D3. Allow-list: `tedge identify` in `extras/shell-diagnostics.conf`

The allow-list is one string Kconfig the module owns, and the module's
profile keeps it empty by design, so the entry goes where the other entries
are: `shell-diagnostics.conf`. Matching is by prefix, so the entry also allows
`tedge identify <n>`; the module already refuses anything that could chain a
second command. Identify is harmless (it changes nothing but the LED for at
most 5 minutes), so it fits the file's "read-only and returns within the
timeout" rule; the comment block lists it with the others.

### D4. `sw0` wiring moves to `lib/common/button.c`

`prov_handoff.c` keeps the provisioning actions; GPIO setup, debounce and the
work item move to `button.c`, built when `APP_PROV_HANDOFF || APP_IDENTIFY`,
started from `net.c` outside the `APP_PROV_HANDOFF` block. `button.c` calls
`gesture_*()` and dispatches each result: provisioning/erase to `prov_handoff`
(when built), identify to `identify.c` (when built). With `APP_PROV_HANDOFF`
off, the provisioning and erase gestures are disabled in the config (count 0 /
hold 0 meaning "off"), so a triple press or long hold does nothing.

### D5. Gesture classifier gains a second press count

`struct gesture_cfg` gets `identify_count` (0 = disabled) and the classifier
returns a new `GESTURE_IDENTIFY` when a sequence ends quietly with exactly that
many short presses, alongside the existing `GESTURE_PROVISION`. Both use the
same window, short-press limit and quiet gap, so the classifier already waits
for the quiet gap before deciding between 2 and 3 — a triple press never fires
identify first. `BUILD_ASSERT(APP_IDENTIFY_PRESS_COUNT !=
APP_WIFI_PROV_PRESS_COUNT)` when both are built. Host tests in
`tests/button_gesture/` cover: 2 → identify, 3 → provision, 2 with identify off
→ none, 2 slow → none, hold → erase unaffected.

Default 2 because it is the quickest deliberate gesture and a fumbled triple
press that comes out as two is harmless (an event, no mode change).

### D6. Event: `zephyr_Identify`, rate-limited, from the work queue

`identify.c` handles `GESTURE_IDENTIFY` on the button work queue: flash
`STATUS_LED_IDENTIFY` for 1 s, then `tedge_publish_event("zephyr_Identify",
"Identify button pressed on <hostname>", 0)` unless the previous one was under
5 s ago. `tedge_publish_event()` only enqueues, so the work queue never waits on
the network. Text carries the hostname (`app_net_hostname()`), which is also
the mDNS name, so the operator sees both names side by side. The `zephyr_`
prefix matches the repo's other device-defined types.

### D7. `identify.c` lives in `lib/common`

The shell subcommand registers itself at link time (`SHELL_SUBCMD_ADD`, under
`CONFIG_SHELL`), and the gesture handler is called by `button.c`, so the apps
need no call of their own. Keeping it in `lib/common` rather than the module
respects the module's independence rule (the module knows nothing about the
LED) and avoids four copies in the apps' glue.

### D8. Events Smart Function maps all types generically

`tedge-zephyr-events/events.js`, topic `te/device/*///e/*`: type from the last
topic segment, `text` (fallback: the type), `time` from the payload or the
message, every other payload key copied as a fragment, `externalSource`
`c8y_Serial` = client ID (same as the firmware-progress function). Tests:
`zephyr_Identify`, an arbitrary type, a payload without text. Generic rather
than identify-only because no event reaches the tenant today and a per-type
function would have to be repeated for every app event.

### D9. Failed shell commands: strip terminal escapes before reporting

Found on hardware: `shell_error()` output reaches the capture backend as
`\r\n ESC[1;31m <text> \r\n ESC[m`. Cumulocity accepts line breaks inside a
quoted SmartREST field but refuses the escape character, so the `505` was
rejected and the operation stayed EXECUTING. `tedge_shell_clean_output()`
(pure, unit-tested) removes CSI and two-character escape sequences and other
C0 controls except `\n`, `\r`, `\t` from the captured output in
`tedge_shell_cmd.c`; `sr_quote()` also drops C0 controls (except `\n` when
keeping lines, `\r` handled as before, and tab) as a backstop for every
other text the client quotes. Line breaks stay in both results and failure
reasons.

*Alternative:* `CONFIG_SHELL_VT100_COLORS=n`. Rejected: it is an image-wide
setting the integrator owns, and it would leave the client one shell option
away from the same stuck operations.

## Risks / Trade-offs

- **Event lost while offline** → acceptable; it uses the existing telemetry
  queue with QoS 1 once connected. The operator sees the LED ack regardless.
- **Accidental double press** → produces one harmless event; rate limit stops
  floods.
- **Older images** → `tedge identify` is refused as "not on the device's
  allow-list" (or not a known command) — correct and self-explaining.
- **Cloud identify missing on most boards** → see D1; documented per image in
  DEVICES.md so nobody expects it on a WROOM.
- **Smart Function not deployed** → the event silently never appears on the
  MQTT Service transport. README and the operator docs say to deploy it; the
  identify docs name it explicitly.
- **RGB boards** → they rely on the RGB backend from `rgb-status-led`, which
  is already in `status_led.c` (its archive is pending); without it
  `status_led_present()` would be false on exactly the boards with the shell.

## Migration Plan

Firmware: none; shipped through the normal release and OTA. Tenant: deploy
`tedge-zephyr-events` once. Rollback: older firmware refuses the command; the
Smart Function can stay deployed.

## Open Questions

- Should cloud identify reach the boards without the shell (WROOM, CAM, C6,
  snmp-agent)? Options would be a minimal shell (no kernel/net/wifi shells,
  only the `tedge` root) or a fallback dedicated operation; both are a
  separate change with their own footprint measurement.

- Should the identify event also set a managed-object fragment (e.g.
  `zephyr_LastIdentified`) so the device list can be sorted by it? Left out;
  the event list is enough for the first version.
