## MODIFIED Requirements

### Requirement: Bounded resource ownership

The module SHALL run its work on its own thread(s), each with a stack size and
priority set in Kconfig. It SHALL NOT block the system work queue. It SHALL
allocate its dynamic memory from its own bounded heap, sized in Kconfig, and not
from the application's heap. TLS memory is the exception: it comes from
Zephyr's mbedTLS heap, which is global and part of the application's
configuration, so the module's documentation SHALL state the mbedTLS heap each
profile needs (per concurrent TLS session and TLS record size). The module's
documentation SHALL also list what it requires from the application's
configuration: socket, TCP-connection and poll-slot counts, mbedTLS options,
and task-watchdog channels.

#### Scenario: Client memory is capped

- **WHEN** the client's operations need more memory than its configured heap
- **THEN** the affected operation fails with a reason, and the application's
  own allocations are unaffected

#### Scenario: TLS heap too small

- **WHEN** the application's mbedTLS heap is smaller than the documented need
  and a handshake runs out of memory
- **THEN** the client logs that the TLS handshake failed for lack of mbedTLS
  heap, backs off, and the application keeps running

### Requirement: Integration hooks for the host application

The public API SHALL let the host application:

- provide the device identity: external ID, name, type, and firmware name and
  version (with a documented default for each);
- push telemetry: measurements, events and alarms;
- publish its own twin data;
- register its own operation handlers, log types and configuration types;
- prepare for or veto a restart requested by the cloud;
- replace the platform reset the client uses to restart the device;
- add its own health checks that must pass before a newly updated firmware image
  is confirmed;
- observe the client's state (for example to drive a status LED);
- read the registration URL while the device awaits registration, and set the
  tenant host at runtime;
- optionally report liveness progress to the application's watchdog.

The module SHALL NOT reset the device without first giving the application's
restart hook the chance to respond. Its default reset SHALL be a full-system
reset on platforms where a CPU-only reset can leave the bootloader hung.
API functions for features not yet implemented or not built in SHALL return
`-ENOTSUP`.

#### Scenario: Application-supplied telemetry

- **WHEN** the application calls the telemetry API with a measurement
- **THEN** the client publishes it on the configured transport, and no telemetry
  is read from anything outside the calls the application makes

#### Scenario: Application vetoes a restart

- **WHEN** a restart operation arrives and the application's restart hook
  declines it
- **THEN** the client does not reboot and marks the operation as failed with the
  reason returned by the hook

#### Scenario: Application supplies the reset

- **WHEN** the application registers a reset hook and a restart is allowed
- **THEN** the client calls that hook instead of its platform default

#### Scenario: Feature not available

- **WHEN** the application calls an API function whose feature is not built
  into the image
- **THEN** the call returns `-ENOTSUP` and has no other effect
