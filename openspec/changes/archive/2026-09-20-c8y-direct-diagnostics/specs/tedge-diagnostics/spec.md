## ADDED Requirements

### Requirement: A device can be asked for its logs

When log upload is built in, the client SHALL advertise the log types the
running image offers, SHALL produce the requested type on request, and SHALL
make it available to the cloud as a retrievable file. The client SHALL apply
the filters a request carries as far as the type allows, and SHALL bound
what one request can produce.

#### Scenario: Fetching the log of a device in the field

- **WHEN** an operator asks a connected device for its log
- **THEN** the log is retrievable from the cloud without anyone visiting the
  device

#### Scenario: A filtered request

- **WHEN** a log is requested with a search text and a line limit
- **THEN** the returned log contains only matching lines, up to that limit

#### Scenario: More log than the device may send

- **WHEN** the requested log is longer than the configured maximum
- **THEN** the device sends what fits and says in the file that it was
  truncated, rather than refusing the request

### Requirement: The client keeps a log of its own

When log upload is built in, the client SHALL keep its own recent log in a
bounded amount of RAM, oldest lines dropped first, and SHALL offer it as a
log type without the application registering anything.

#### Scenario: A device whose application has no log

- **WHEN** an application that never registered a log type is asked for a
  log
- **THEN** the client's own recent log is returned

### Requirement: A crash outlives the device's memory

When crash dumps are built in, the client SHALL store a dump when the device
faults, SHALL offer the stored dump as a log type after the device restarts,
and SHALL keep it until the cloud has taken it. The dump SHALL be uploaded in
a form the standard Zephyr tooling can decode.

#### Scenario: A device that crashed and rebooted

- **WHEN** a device faults and restarts, and an operator asks for the dump
- **THEN** the dump from before the restart is retrievable and can be
  decoded with Zephyr's coredump tooling

#### Scenario: An upload that failed

- **WHEN** the upload of a stored dump does not complete
- **THEN** the dump is still stored and can be requested again

### Requirement: A commanded shell is allow-listed and off by default

When the shell command feature is built in, the client SHALL run only
commands permitted by its configured allow-list, and SHALL refuse every
command when no allow-list is configured, with a reason that names what to
configure. The client SHALL refuse a command that tries to chain or redirect
into another command, whatever the allow-list says. A command SHALL NOT run
on the client's own thread, and only one command SHALL run at a time.

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

### Requirement: Diagnostics never block the connection

Producing or uploading a log SHALL NOT stop the client from serving its
connection, and a failed upload SHALL fail its operation with a reason
rather than leave it pending.

#### Scenario: An upload while the device is working

- **WHEN** a log is uploaded
- **THEN** the device stays connected throughout and telemetry keeps flowing

#### Scenario: An upload that cannot complete

- **WHEN** a log upload fails
- **THEN** the operation is reported as failed with a reason
