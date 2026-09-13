## ADDED Requirements

### Requirement: Writable control data points
The address space SHALL expose writable control nodes under the Device object in
the application namespace (ns=1): an integer `Setpoint` (Int32, `ns=1;s=Setpoint`)
and a boolean `Running` (`ns=1;s=Running`), each with read and write access.
Their values are held in RAM (not persisted across reboot).

#### Scenario: Setpoint is writable and reads back
- **WHEN** a client writes an in-range value to the `Setpoint` node
- **THEN** the write SHALL succeed and reading `Setpoint` SHALL return the written value

#### Scenario: Running flag is writable
- **WHEN** a client writes true or false to the `Running` node
- **THEN** the write SHALL succeed and reading `Running` SHALL return the written value

### Requirement: Firmware reacts to writes
The firmware SHALL be notified when a writable control node is written and SHALL
act on the new value (at minimum: log it, and reflect it on the status display
where one is present).

#### Scenario: Write is observed by the firmware
- **WHEN** a client writes `Setpoint` or `Running`
- **THEN** the firmware SHALL observe the new value (logged, and shown on the display if present) rather than silently storing it

### Requirement: Writable values are validated
Writes to control nodes SHALL be validated; out-of-range or invalid values SHALL
be clamped to a documented range or rejected, so the device is not left in an
invalid state.

#### Scenario: Out-of-range setpoint handled
- **WHEN** a client writes a `Setpoint` outside the supported range
- **THEN** the server SHALL either clamp it to the range or reject the write, and never store an invalid value
