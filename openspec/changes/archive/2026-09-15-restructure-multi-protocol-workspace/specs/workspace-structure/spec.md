## ADDED Requirements

### Requirement: Single manifest repository with layered layout

The repository SHALL remain a single west manifest repository whose code is
organised into three layers: a shared `lib/common/` core, per-protocol
`lib/<protocol>/` libraries, and per-protocol `apps/<protocol>/` applications.
Protocol-agnostic functionality SHALL live only in `lib/common/`; protocol
frontends SHALL live in their own library and application directories.

#### Scenario: Layout present after restructure

- **WHEN** the repository is inspected after this change
- **THEN** `lib/common/`, `lib/opcua/`, and `apps/opcua-server/` exist
- **AND** connectivity, status display, device identity, and the data-model
  abstraction reside under `lib/common/`
- **AND** the OPC-UA server, address space, and vendored open62541 reside under
  `lib/opcua/`
- **AND** no protocol-specific source remains in a shared/top-level location

#### Scenario: Single west manifest preserved

- **WHEN** `west.yml` is inspected
- **THEN** it is still the single manifest at the repository root with unchanged
  project pins and name-allowlist
- **AND** the OPC-UA stack (open62541) is referenced only by `lib/opcua/`

### Requirement: Shared core is a reusable Zephyr module

`lib/common/` SHALL be a Zephyr module (declaring `zephyr/module.yml` with its
CMake and Kconfig entry points) so that every application can consume it without
copying files, and its Kconfig symbols are available to all apps.

#### Scenario: Common module consumed by an application

- **WHEN** an application under `apps/<protocol>/` is built
- **THEN** it links the `lib/common/` module and gains its connectivity,
  identity, and data-model code and Kconfig symbols
- **AND** the common code is compiled once from its single source location

### Requirement: Per-protocol applications compose common plus one frontend

Each `apps/<protocol>/` SHALL be a standalone Zephyr application providing its
own `main.c`, `prj.conf`, and `Kconfig`, and SHALL compose `lib/common/` with
exactly one protocol frontend library for its primary purpose. An application
SHALL declare its own firmware name via `CONFIG_APP_FIRMWARE_NAME`.

#### Scenario: OPC-UA application composition

- **WHEN** `apps/opcua-server/` is built
- **THEN** it composes `lib/common/` with `lib/opcua/`
- **AND** its `CONFIG_APP_FIRMWARE_NAME` defaults to `zephyr-opcua-server`
- **AND** the resulting firmware exposes the OPC-UA server on its configured port

#### Scenario: Unused protocol libraries are not compiled

- **WHEN** an application that does not use a given protocol is built
- **THEN** that protocol's library and third-party stack are not compiled into
  the image, so per-image flash/RAM does not grow for unused protocols

### Requirement: Per-application build command

Firmware SHALL be built by targeting the application directory, i.e.
`west build -b <board> apps/<protocol>` rather than building the repository root.

#### Scenario: Build the OPC-UA firmware

- **WHEN** a developer runs `west build -b esp32_devkitc/esp32/procpu apps/opcua-server`
- **THEN** the OPC-UA server firmware is produced
- **AND** the previous root-level `west build -b <board> .` invocation is no
  longer the documented build command

#### Scenario: Board portability retained

- **WHEN** an application is built for `native_sim/native/64` or a supported
  hardware board
- **THEN** the application's own `apps/<protocol>/boards/<board>.conf` overlays
  apply (Zephyr auto-merges board overlays relative to the application), and the
  Docker/macOS build+flash workflow continues to apply unchanged
- **AND** per-app board overlays allow each protocol workload to tune RAM/buffers
  independently
