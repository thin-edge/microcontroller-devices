# tedge-telemetry Specification

## Purpose
TBD - created by archiving change c8y-direct-telemetry. Update Purpose after archive.
## Requirements
### Requirement: An application can publish measurements, events and alarms

When the telemetry feature is built in, the client SHALL let the host
application publish measurements, events and alarms, and clear an alarm it
raised. A call SHALL be safe from any thread and SHALL NOT block on the
network. The client SHALL NOT produce telemetry the application did not ask
for, beyond its own health.

#### Scenario: A measurement reaches the cloud

- **WHEN** the application publishes a measurement while the device is
  connected
- **THEN** the cloud records it with its series and values

#### Scenario: Published from another thread

- **WHEN** the application publishes from its own thread while the client is
  busy
- **THEN** the call returns promptly and the message is sent by the client

#### Scenario: An alarm is raised and cleared

- **WHEN** the application raises an alarm and later clears it
- **THEN** the cloud shows the alarm raised with its severity and text, and
  then cleared

### Requirement: Telemetry carries the time it was taken

Every message SHALL carry the time it was produced, so that a message held
while the device is offline is recorded with the time of the reading rather
than the time it arrived. The application MAY supply the time; otherwise the
client SHALL use the current time. When the device has no valid clock, the
client SHALL still send the message and let the cloud stamp it.

#### Scenario: A reading is buffered and sent later

- **WHEN** the application publishes a measurement while the device is
  offline, and the device reconnects a minute later
- **THEN** the cloud records the measurement at the time it was taken

### Requirement: A short outage does not lose data silently

The client SHALL hold messages it cannot send yet in a bounded buffer and
send them, oldest first, once it is connected. When the buffer is full, the
client SHALL drop the oldest measurement rather than refuse new data or grow
its memory, SHALL count what it dropped, and SHALL report that count. Events
and alarms SHALL take precedence over measurements.

#### Scenario: Brief disconnection

- **WHEN** the connection drops for a few seconds while the application
  keeps publishing
- **THEN** the messages are sent once the client reconnects

#### Scenario: The buffer fills

- **WHEN** an application publishes faster than the connection can carry for
  long enough to fill the buffer
- **THEN** the oldest measurements are dropped, the client keeps running,
  and the number dropped is reported

### Requirement: The client reports its own health

When device health is built in, the client SHALL publish its own vital signs
on an interval: how long it has been running, how much of its heap is free,
how many telemetry messages it has dropped, and why the device last reset.

#### Scenario: A device that is degrading

- **WHEN** a device's free heap falls over hours while it keeps running
- **THEN** the trend is visible in the cloud without connecting to the
  device

### Requirement: Telemetry works on both transports

The same API calls SHALL produce free-form `te/` messages on a transport
that has them, and the equivalent SmartREST messages where it does not, with
no change to the application.

#### Scenario: The same application on either transport

- **WHEN** an application that publishes measurements is built once for the
  MQTT Service and once for Core MQTT
- **THEN** both builds report the same measurements to the cloud

