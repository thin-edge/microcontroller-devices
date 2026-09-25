## ADDED Requirements

### Requirement: Constant data lives in flash

Tables that the firmware never writes at runtime SHALL be placed in flash
(`const`, or the library's ROM-table option), not in RAM `.data`/`.bss`. This
applies to generated third-party tables (open62541 data-type descriptions),
crypto tables (mbedTLS AES T-tables) and the applications' own lookup tables.
A regenerated vendored library SHALL keep this property: the regeneration
script applies it, it is not a manual edit.

#### Scenario: open62541 type tables are read-only

- **WHEN** the OPC-UA application is built for any board
- **THEN** `UA_TYPES` and its member arrays are in a flash-mapped section, and
  a standard OPC-UA client can browse and read every node as before

#### Scenario: Regeneration keeps tables const

- **WHEN** `scripts/regen-open62541.sh` regenerates the amalgamation
- **THEN** the generated type tables are declared `const` without further
  editing, and the build fails rather than silently placing them in RAM if the
  patch no longer applies

#### Scenario: AES tables in flash

- **WHEN** any image with TLS is built (tedge profiles, ZTP provisioner)
- **THEN** mbedTLS's AES forward/reverse tables are not in RAM, and TLS
  sessions to Cumulocity complete as before

### Requirement: Stacks and heaps are sized from measurements

Each configured size SHALL come from a measurement: every thread stack, the
system heap, the mbedTLS heap and the client's private heap that this
repository configures SHALL be sized from a measured high-water mark (thread analyzer, heap statistics or mbedTLS memory debug) taken on the
board under the workload that stresses it most, plus a documented margin: at
least 25 % or 512 B for a stack, whichever is larger, and a stated byte margin
for each heap. The configuration file that sets the size SHALL record the
measured peak, the workload and the date. A size that has no effect (for
example a `CONFIG_HEAP_MEM_POOL_SIZE` below the sum of the drivers' `ADD_SIZE`
minimums) SHALL NOT be set.

#### Scenario: Stack trimmed with margin

- **WHEN** a stack size is lowered
- **THEN** its config line or comment states the measured peak and workload,
  and the new size is at least the peak plus the margin

#### Scenario: No-op heap setting removed

- **WHEN** a board's resolved system heap size is larger than its configured
  `CONFIG_HEAP_MEM_POOL_SIZE`
- **THEN** the configuration either drops the setting or makes it effective
  with `CONFIG_HEAP_MEM_POOL_IGNORE_MIN`, and states the heap size the image
  actually gets

#### Scenario: Explicit system heap keeps margin

- **WHEN** a board sets its system heap below the drivers' combined minimum
- **THEN** a measured low-water mark over a soak run with the board's heaviest
  workload (Wi-Fi reconnect, firmware download, tunnel) stays above the
  documented margin

### Requirement: PSRAM carries what does not need internal RAM

A PSRAM board SHALL keep internal RAM for what needs it: on a board whose
configuration enables PSRAM, the mbedTLS heap, the Wi-Fi driver heap and the
network buffer allocations SHALL be placed in PSRAM unless
a measured failure shows one of them must stay internal. That exception is
recorded in the board configuration.

#### Scenario: PSRAM board internal RAM relieved

- **WHEN** an application is built for the S3-DevKitC, QT Py S3 or ESP32-CAM
  with its tedge board configuration
- **THEN** the Wi-Fi heap and network buffers are in the external RAM region,
  and the board passes the release run (connection, telemetry, firmware
  update, protocol reads, and a tunnel where remote access is built)

### Requirement: A PSRAM board runs the full feature profile

Every application SHALL build and run the full device-management profile on
every supported board that has PSRAM. Where an application's own allocations
are the limit rather than static placement, the application SHALL allocate
from external RAM on those boards instead of from the internal libc arena, and
SHALL keep using the internal arena on boards without PSRAM. A board that
cannot reach the full profile SHALL record in its configuration which
placement failed and what it ships instead.

#### Scenario: ESP32-CAM at the full profile

- **WHEN** an application is built for the ESP32-CAM with the full profile
- **THEN** it links within dram1, and the board passes its release run with
  remote access, log upload, parameters and certificate renewal all working,
  where before it could ship only the ota profile

#### Scenario: OPC-UA does not compete for the internal arena

- **WHEN** the OPC-UA application is built for a board with PSRAM
- **THEN** its server allocations come from a heap in external RAM, and a
  standard client holds sessions and reads nodes for 10 minutes beside the
  device-management client without `BadOutOfMemory`

#### Scenario: No PSRAM, no change

- **WHEN** the OPC-UA application is built for a board without PSRAM
  (ESP32-C6, ESP32-WROOM-32)
- **THEN** it allocates from the libc arena exactly as before

#### Scenario: Placement that does not work is recorded

- **WHEN** a PSRAM placement fails on a board (for example network buffers on
  a classic ESP32)
- **THEN** the board's configuration states which placement failed, how it
  failed and which profile the board ships, rather than the board silently
  keeping a smaller profile

### Requirement: Flash is budgeted per partition

Each image SHALL fit a documented share of the partition that holds it: the
signed application at most 80 % of `slot0` and the provisioner at most 92 % of
`prov`, leaving the existing 95 % ceiling as the hard limit an over-the-air
update requires. Flash savings SHALL come from encoding and from code that is
never reached, never from removing a log level, diagnostic or command from an
application image; the provisioner, whose logs serve provisioning only, is the
stated exception.

#### Scenario: Provisioner within budget

- **WHEN** the Wi-Fi provisioner is built for a 4 MB board
- **THEN** its image is at most 92 % of the `prov` partition, and a fresh
  device still completes both Improv BLE and ZTP provisioning

#### Scenario: Application over budget fails the build

- **WHEN** a signed application image exceeds 80 % of `slot0`
- **THEN** the size step fails, naming the image, the partition and the
  percentage

#### Scenario: Diagnostics are kept

- **WHEN** flash is reduced in an application image
- **THEN** the log levels, shell commands and diagnostics that the build
  shipped before are still present, and the reduction comes from encoding or
  unreached code

### Requirement: Per-build RAM and flash baselines are enforced

The release size report SHALL include, for each build, the static use of every
RAM region, the RAM left for the libc malloc arena, and the signed image size
against its partition, and SHALL compare them with a committed baseline for
that build. A pull request whose build uses more of any RAM region or more
flash than its baseline, beyond a small tolerance, SHALL fail until the
baseline is updated in the same pull request.

#### Scenario: Regression caught

- **WHEN** a change increases a release build's dram0 use or image size by
  more than the tolerance and does not update the baseline
- **THEN** the size step fails and names the build, the region or partition,
  and the delta

#### Scenario: Savings recorded

- **WHEN** a change lowers a build's RAM or flash use
- **THEN** the size report shows the reduction against the baseline, and
  updating the baseline records the new level

### Requirement: Footprint work preserves functionality

A footprint optimisation SHALL NOT remove a feature, protocol object,
capability advertised to the cloud, or observable protocol behaviour from any
build that shipped it. A runtime-sized change (heap, buffer or stack) SHALL be
accepted only after the build passes on the board the release run that
`release/devices.yml` requires for that build.

#### Scenario: Release run after a heap change

- **WHEN** a board's mbedTLS or system heap is lowered
- **THEN** that board's release builds pass their release run (connection,
  telemetry, firmware update that confirms, a tunnel if remote access is
  built, protocol reads alongside) before the change merges

#### Scenario: Newly fitting build

- **WHEN** a build that did not fit before now links and passes its board run
- **THEN** it is added to `release/devices.yml` with a `measured` link to its
  `DEVICES.md` entry; a build that only links is not added
