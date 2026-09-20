## MODIFIED Requirements

### Requirement: Integration hooks for the host application

The public API SHALL let the host application:

- provide the device identity: external ID, name, type, and firmware name and
  version (with a documented default for each);
- push telemetry: measurements, events and alarms;
- register its own operation handlers, log types and configuration types;
- prepare for or veto a restart requested by the cloud;
- add its own health checks that must pass before a newly updated firmware image
  is confirmed;
- observe the client's state (for example to drive a status LED);
- optionally report liveness progress to the application's watchdog.

The module SHALL NOT call `sys_reboot()` without first giving the application's
restart hook the chance to respond.

#### Scenario: Application narrows remote-access targets

- **WHEN** the application registers a remote-access target hook and the
  cloud asks for a tunnel to a target the built-in policy allows but the hook
  refuses
- **THEN** the client opens no connection and fails the operation with the
  hook's reason

#### Scenario: Application vetoes a new firmware image

- **WHEN** a newly installed image is running and connected, and the
  application's firmware-check hook reports that the application is not
  healthy
- **THEN** the client does not confirm the image, so the bootloader reverts
  it on the next reset

#### Scenario: Application-supplied telemetry

- **WHEN** the application calls the telemetry API with a measurement
- **THEN** the client publishes it on the configured transport, and no telemetry
  is read from anything outside the calls the application makes

#### Scenario: Application vetoes a restart

- **WHEN** a restart operation arrives and the application's restart hook
  declines it
- **THEN** the client does not reboot and marks the operation as failed with the
  reason returned by the hook
