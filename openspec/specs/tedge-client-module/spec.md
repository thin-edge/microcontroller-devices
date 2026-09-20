# tedge-client-module Specification

## Purpose
How the tedge-zephyr client is packaged and integrated: a self-contained Zephyr module with a namespaced API that any Zephyr application can include, where the host application owns connectivity and the module owns bounded resources, and which can move into its own repository unchanged.
## Requirements
### Requirement: The client is a self-contained Zephyr module

The client SHALL be a Zephyr module named `tedge` in the repository's top-level
`tedge-zephyr/` directory. That directory SHALL contain everything the module
needs (`zephyr/module.yml`, `Kconfig`, `CMakeLists.txt`, `include/tedge/`,
sources, `samples/`, `tests/` and `README.md`), so that it can move into its own
repository and west project without changing its contents. The module SHALL
depend only on Zephyr and its modules (and on MCUboot for firmware update). It
SHALL NOT include, link or reference anything under this repository's `lib/` or
`apps/`.

#### Scenario: Built without this repository's code

- **WHEN** `tedge-zephyr/samples/minimal` is built on its own for a supported
  board, with the module as the only extra Zephyr module
- **THEN** the build succeeds without `lib/common` or any other `lib/` module

#### Scenario: Dependency leak is caught

- **WHEN** a source or CMake file under `tedge-zephyr/` references a path or
  header from `lib/` or `apps/`
- **THEN** the repository's check (the minimal-sample build, or a grep in CI)
  fails and names the offending file

### Requirement: Namespaced public API and configuration

All public symbols of the module SHALL use the `tedge_` prefix (types, functions
and macros), and its headers SHALL live under `include/tedge/`. Its Kconfig
symbols SHALL use the `TEDGE_` prefix. Its persistent settings SHALL live under
a single `tedge/` settings subtree. Its TLS credentials SHALL use a configurable
tag range. None of these SHALL collide with the host application's names.

#### Scenario: Coexisting with an application's own TLS and settings

- **WHEN** an application that already uses its own settings keys and TLS
  credential tags includes the module
- **THEN** neither the application's nor the module's settings or credentials
  are overwritten

### Requirement: The application owns connectivity

The module SHALL NOT bring up, configure or reconnect the network interface.
It SHALL start connecting when Zephyr reports that an interface has an IPv4
address (network-management events). It SHALL stop and back off when the
interface goes down. The application's connectivity code (in this repository,
`lib/common/net.c`) stays responsible for Wi-Fi.

#### Scenario: Network appears after start

- **WHEN** the application starts the client before Wi-Fi is connected
- **THEN** the client waits without busy-looping, and it connects to the cloud
  once the interface has an address

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
- register its own operation handlers, log types and configuration types;
- prepare for or veto a restart requested by the cloud;
- add its own health checks that must pass before a newly updated firmware image
  is confirmed;
- observe the client's state (for example to drive a status LED);
- optionally report liveness progress to the application's watchdog.

Every one of these calls SHALL be present whether or not the feature behind
it is built in: a call belonging to a feature that is absent SHALL return
"not supported", so that an application compiles against the API once and
chooses its features in Kconfig.

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

#### Scenario: Telemetry from the application's own threads

- **WHEN** the application publishes telemetry from a thread of its own
  while the client is busy with an operation
- **THEN** the call returns without waiting for the network, and the client
  sends the message on its own thread

#### Scenario: Application vetoes a restart

- **WHEN** a restart operation arrives and the application's restart hook
  declines it
- **THEN** the client does not reboot and marks the operation as failed with the
  reason returned by the hook

#### Scenario: Application adds a log type

- **WHEN** the application registers a log type and the cloud asks for it
- **THEN** the client asks the application to produce it and makes the result
  retrievable from the cloud

#### Scenario: Registering against a feature that is not built in

- **WHEN** an application that registers a log type is built without log
  upload
- **THEN** it still compiles and links, and the registration returns "not
  supported"

### Requirement: Example integrations

The repository SHALL show how an application integrates the module, through
`tedge-zephyr/samples/minimal` and at least one of this repository's protocol
applications. Integrations SHALL use only the module's public API. In a protocol
application, the glue that maps `lib/common`'s data model and connectivity
state onto that API SHALL live in the application, not in the module.

#### Scenario: Protocol application as a user application

- **WHEN** a protocol application is built with `CONFIG_TEDGE` enabled
- **THEN** it serves its industrial protocol as before and also runs the
  client, and the code connecting the two lives under `apps/`

