## MODIFIED Requirements

### Requirement: A commanded shell is allow-listed and off by default

When the shell command feature is built in, the client SHALL run only
commands permitted by its configured allow-list, and SHALL refuse every
command when no allow-list is configured, with a reason that names what to
configure. The client SHALL refuse a command that tries to chain or redirect
into another command, whatever the allow-list says. A command SHALL NOT run
on the client's own thread, and only one command SHALL run at a time.
The reported output of a command, whether it succeeded or failed, SHALL keep
its line breaks, and SHALL NOT contain carriage returns. A `help` (or `?`)
command SHALL be answered without running anything, with a header line
followed by each allow-listed command on a line of its own.

#### Scenario: No allow-list configured

- **WHEN** the cloud sends a command to a device whose allow-list is empty
- **THEN** the operation fails with a reason naming the configuration
  needed, and nothing runs

#### Scenario: An allowed command

- **WHEN** the cloud sends a command that the allow-list permits
- **THEN** the device runs it and reports what it printed

#### Scenario: Smuggling a second command

- **WHEN** a command begins with an allowed prefix but chains another
  command onto it
- **THEN** the operation fails and neither command runs

#### Scenario: The connection during a slow command

- **WHEN** a permitted command takes several seconds
- **THEN** the client stays connected and keeps handling other messages

#### Scenario: Asking for help

- **WHEN** the cloud sends `help` to a device whose allow-list is
  `kernel uptime,net iface,tedge diag`
- **THEN** the operation succeeds with a result whose lines are a header,
  then `kernel uptime`, `net iface` and `tedge diag`, one per line

#### Scenario: Multi-line output keeps its lines

- **WHEN** a permitted command prints several lines ending in `\r\n`
- **THEN** the reported result has the same lines separated by `\n`, with
  no carriage returns and no lines joined by spaces
