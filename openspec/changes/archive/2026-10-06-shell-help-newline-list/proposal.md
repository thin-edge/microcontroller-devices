## Why

Sending `help` as a cloud shell command (c8y_Command) should list the commands
the device will run, one per line. The device already builds that list one
command per line, but the reported result arrives in Cumulocity as a single
run-on line (`Commands this device runs (arguments may follow):   kernel uptime   net iface   ...`),
because `tedge_sr_quote()` turns every newline into a space before the result
is published. With seven allow-listed commands this is hard to read, and the
same flattening hides the line structure of every other command's output.

## What Changes

- The result of a c8y_Command operation (successful or failed) keeps its line
  breaks: newlines are sent inside the quoted SmartREST field instead of being
  replaced with spaces. Carriage returns are dropped, so Zephyr's `\r\n`
  output becomes plain `\n`.
- `help` (and `?`) therefore show up in Cumulocity as a header line followed by
  one allow-listed command per line.
- Other SmartREST messages (remote access, log upload, firmware, events, etc.)
  keep today's single-line quoting; nothing outside c8y_Command changes.

## Non-goals

- Changing what `help` lists, or its wording, beyond what is needed to read
  well once line breaks survive.
- Raising the size of the reported result (still bounded by the 224-byte quoted
  buffer and `CONFIG_TEDGE_SHELL_COMMAND_OUTPUT_BYTES`).
- Changing Zephyr's own `help`, which the device still does not run.
- Any change to the allow-list rules.

## Scope

- **Phase**: 3 (thin-edge.io / Cumulocity client, `tedge-zephyr`).
- **Protocol**: none; protocol-agnostic, affects every app built with
  `CONFIG_TEDGE_SHELL_COMMAND` (modbus-server, tedge-agent, opcua-server; snmp
  does not link with the shell today).
- **Boards**: those that build the shell-diagnostics extra — ESP32-S3-DevKitC,
  ESP32-C6-DevKitC, QT Py ESP32-S3 (and any other board where it fits).
- **Kconfig**: none added.
- **Resource budget**: a few tens of bytes of flash for a quoting variant, no
  new RAM (existing buffers are reused). C6 builds sit at 93–95% RAM, so the
  change must not add buffers.
- **Public `tedge_*` API**: unchanged; the new helper is internal
  (`tedge_internal.h`).

## Capabilities

### New Capabilities

_None._

### Modified Capabilities

- `tedge-diagnostics`: the commanded-shell requirement gains the rule that the
  reported output keeps its line breaks, and that `help` lists the allowed
  commands one per line.

## Impact

- `tedge-zephyr/src/tedge_smartrest.c` / `tedge_internal.h`: a quoting variant
  that keeps `\n` and drops `\r`.
- `tedge-zephyr/src/tedge_c8y.c`: c8y_Command results use that variant.
- `tedge-zephyr/tests/unit/src/main.c`: unit tests for the new quoting.
- Cumulocity: the c8y_Command result text becomes multi-line; the device
  management UI shows it as such.
