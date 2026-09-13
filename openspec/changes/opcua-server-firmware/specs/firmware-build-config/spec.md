## ADDED Requirements

### Requirement: Zephyr application builds for the co-primary hardware boards
The firmware SHALL build as a Zephyr application for both co-primary boards —
ESP32-WROOM-32 and the ESP32-S2 (Feather ESP32-S2 TFT) — using the standard
Zephyr toolchain (`west build`). It SHOULD also build for the Raspberry Pi
Pico W as an additional target.

#### Scenario: Build for each ESP32 co-primary board
- **WHEN** a developer runs the documented `west build` command for the ESP32-WROOM-32 and ESP32-S2 boards
- **THEN** each build SHALL succeed and produce a flashable firmware image

#### Scenario: Fits each board's resource budget
- **WHEN** the firmware is built for a target board
- **THEN** the reported RAM and flash usage SHALL fit within that board's available memory (using PSRAM on the ESP32-S2 where needed)

#### Scenario: PSRAM used on ESP32-S2
- **WHEN** the firmware is built for the ESP32-S2 board
- **THEN** the build SHALL enable and use the board's 2 MB PSRAM for the OPC-UA stack's dynamic memory

### Requirement: Hardware-free dev/CI target
The firmware SHALL build and run on the `native_sim` target so the same
application logic can be exercised on a host machine without hardware.

#### Scenario: Build and run on native_sim
- **WHEN** a developer builds and runs the application for `native_sim` following the documentation
- **THEN** the OPC-UA server SHALL start and be reachable by a host-side OPC-UA client

### Requirement: Portable configuration
The application SHALL keep board-independent logic separate from board-specific
configuration, exposing tunable settings (at least server port, device name,
mDNS hostname, sampling interval, and Wi-Fi credentials) through Zephyr
configuration (Kconfig/`prj.conf`) with sensible defaults, and board specifics
through board overlays/conf files.

#### Scenario: Change a setting without editing application code
- **WHEN** a developer changes the server port or device name via configuration
- **THEN** the change SHALL take effect on the next build without modifying application source code

#### Scenario: Adding a board does not require core changes
- **WHEN** support for an additional Zephyr Wi-Fi board is added via a board overlay/conf
- **THEN** the core application and OPC-UA logic SHALL require no changes to build for it

### Requirement: Wi-Fi credentials configurable without committing secrets
The firmware SHALL take Wi-Fi SSID and password via configuration, and SHALL
provide a way to supply them without hard-coding secrets into committed source
(e.g. a git-ignored local `.conf`/overlay or build-time definitions).

#### Scenario: Provide credentials via local configuration
- **WHEN** a developer supplies Wi-Fi SSID/password through the documented local configuration mechanism
- **THEN** the firmware SHALL use those credentials to join the network, and no secret SHALL need to be committed to the repository

### Requirement: Build, flash, and connect documentation
The change SHALL provide documentation covering how to build and flash the
firmware and how to connect a standard OPC-UA client to the running server. The
documentation SHALL cover **macOS** (the primary dev machine) explicitly, and
call out where Linux steps differ.

#### Scenario: Documented path from build to connected client on macOS
- **WHEN** a new user on macOS follows the documentation
- **THEN** they SHALL be able to build/flash the firmware (including any macOS-specific USB/serial driver, port naming, or toolchain setup) and read a node value from a standard OPC-UA client

#### Scenario: Linux differences are called out
- **WHEN** a step differs between macOS and Linux (e.g. serial device path or driver install)
- **THEN** the documentation SHALL note the Linux equivalent so a Linux user can also follow it

#### Scenario: Device found via mDNS in docs
- **WHEN** a user follows the documentation to locate the device
- **THEN** it SHALL show how to discover the device by its `.local` hostname / `_opcua-tcp._tcp` service (e.g. using `dns-sd` on macOS) instead of requiring a known IP
