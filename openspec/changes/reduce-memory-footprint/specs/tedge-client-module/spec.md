## MODIFIED Requirements

### Requirement: Bounded resource ownership

The module SHALL run its work on its own thread(s), each with a stack size and
priority set in Kconfig. It SHALL NOT block the system work queue. It SHALL
allocate its dynamic memory from its own bounded heap, sized in Kconfig, and not
from the application's heap. The private heap's default size SHALL follow the
features built, so a build without the features that use it most (parameters)
does not reserve their share. TLS memory is the exception: it comes from
Zephyr's mbedTLS heap, which is global and part of the application's
configuration, so the module's documentation SHALL state the mbedTLS heap each
profile needs (per concurrent TLS session, per TLS input record size and per
TLS output record size). The profiles SHALL use an output record buffer smaller
than the input buffer where the client never sends large records. The module's
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

#### Scenario: Private heap follows features

- **WHEN** the client is built with the ota profile (no parameters)
- **THEN** the default private heap is smaller than in the full profile, and
  the health telemetry's free-heap value after enrolment, a firmware update
  and a twin update stays above the margin documented for that profile

#### Scenario: Split TLS record buffers

- **WHEN** a profile is built with the module's mbedTLS user configuration
- **THEN** the TLS output buffer is smaller than the input buffer, and MQTT
  publishing, an HTTPS firmware download, a log upload and a remote-access
  tunnel all complete with it
