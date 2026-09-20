## ADDED Requirements

### Requirement: An operation's status names the operation

The device SHALL report an operation's progress and outcome for the
operation it was given, and SHALL NOT report a status that the cloud could
apply to a different operation. A result SHALL NOT be absorbed by an
operation left behind by an earlier boot, and a restart SHALL be completed
for the operation that caused it.

#### Scenario: Two operations in the same period

- **WHEN** two operations are created for a device and both are carried out
- **THEN** each one ends with its own result, whatever order they finish in

#### Scenario: An operation left executing by a reset

- **WHEN** a device resets while an operation is executing, and a later
  operation is carried out afterwards
- **THEN** the later operation gets its own result, and the interrupted one
  is not reported as that result

#### Scenario: A restart across the reboot

- **WHEN** a device restarts because the cloud asked it to
- **THEN** the restart operation that asked for it is the one reported as
  successful once the device is back
