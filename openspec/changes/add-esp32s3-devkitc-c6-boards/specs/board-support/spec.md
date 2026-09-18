## ADDED Requirements

### Requirement: Board ports are additive configuration only

Adding support for a new board SHALL require only per-application files under
`apps/<app>/boards/` — a `<fully-qualified-board>.conf` and, where the board
devicetree must be adjusted, a sibling `<fully-qualified-board>.overlay`. A board
port SHALL NOT require changes to `lib/common/`, to any protocol frontend
library, or to any application's `src/`, `prj.conf`, or `Kconfig`. The
fully-qualified filename SHALL match the board target with `/` replaced by `_`.

#### Scenario: New board added without touching shared code

- **WHEN** support for `esp32s3_devkitc/esp32s3/procpu` or
  `esp32c6_devkitc/esp32c6/hpcore` is added to an application
- **THEN** the only files added or changed for that application are
  `apps/<app>/boards/esp32s3_devkitc_esp32s3_procpu.{conf,overlay}` or
  `apps/<app>/boards/esp32c6_devkitc_esp32c6_hpcore.{conf,overlay}`
- **AND** `lib/common/`, the frontend libraries, and the application's `src/`,
  `prj.conf`, and `Kconfig` are unchanged

#### Scenario: A port that would require shared-code changes is reported

- **WHEN** a board cannot be supported without changing shared or application
  code
- **THEN** the required change is reported as a portability finding against the
  shared core rather than being absorbed silently into the board port

### Requirement: Supported boards build every application

Every board declared as supported SHALL produce a successful build of all three
applications — `apps/opcua-server`, `apps/modbus-server`, and `apps/snmp-agent` —
using the documented per-application build command, with no board-specific source
changes.

#### Scenario: All applications build for a supported board

- **WHEN** `west build -b <board> apps/<app>` is run for each of the three
  applications against a supported board
- **THEN** each build completes successfully and produces a flashable image

#### Scenario: An application does not fit a board

- **WHEN** an application's image cannot fit the board's flash or RAM
- **THEN** the board is documented as supporting only the applications that do
  fit, naming the application that does not and the limit it exceeded
- **AND** the board is not claimed as supported for that application

### Requirement: Wi-Fi station capability is enabled per board

A Wi-Fi board port SHALL ensure the SoC Wi-Fi node is enabled so
`CONFIG_WIFI_ESP32` can be selected, supplying a devicetree overlay to enable it
only when the board's own devicetree leaves it disabled. The port SHALL configure
the station-mode driver, IPv4 stack, DHCP, and mDNS/DNS-SD responder consistent
with the application's transport needs.

#### Scenario: Board devicetree already enables Wi-Fi

- **WHEN** the board's devicetree already sets the Wi-Fi node to `okay` (as
  `esp32s3_devkitc` and `esp32c6_devkitc` do)
- **THEN** the port supplies no Wi-Fi overlay
- **AND** `CONFIG_WIFI_ESP32=y` is selected from the board `.conf` alone

#### Scenario: Board devicetree leaves Wi-Fi disabled

- **WHEN** the board's devicetree leaves the Wi-Fi node disabled
- **THEN** the port supplies an overlay setting `&wifi { status = "okay"; }` so
  the driver's devicetree dependency is satisfied

### Requirement: A console is reachable on the board's connected port

A board port SHALL provide a console on the USB port the board is actually
connected through. When the board's devicetree routes the console to a peripheral
that is not exposed on that port, the port SHALL supply an overlay re-chosing the
console, and the documentation SHALL state which physical port carries it.

#### Scenario: Board connected over native USB with a UART-routed console

- **WHEN** a board whose devicetree chooses `uart0` for the console is connected
  through its native USB-Serial-JTAG port
- **THEN** the port supplies an overlay enabling `&usb_serial` and setting
  `zephyr,console` and `zephyr,shell-uart` to it
- **AND** boot and connectivity logs are readable on the connected port without
  a second cable

#### Scenario: Early boot output is needed

- **WHEN** a developer needs output from before USB re-enumeration completes
- **THEN** the documentation directs them to the board's UART console as the
  fallback, noting that a USB-Serial-JTAG console drops the earliest boot output

### Requirement: Declared flash size matches the physical part

A board port SHALL ensure the `flash0` size seen by the build matches the flash
actually fitted to the board in hand. When the upstream board devicetree assumes
a different memory variant, the port SHALL correct the size in its overlay.

#### Scenario: Board devicetree overstates the fitted flash

- **WHEN** a board's devicetree declares 8 MB (an N8 variant) but the module in
  hand is a 4 MB N4 part
- **THEN** the port's overlay sets `&flash0` to the real 4 MB size
- **AND** the partition table still fits entirely within the corrected size

#### Scenario: Corrected size conflicts with the board's partition table

- **WHEN** correcting the flash size conflicts with the partition layout the
  board includes
- **THEN** the conflict is recorded and the uncorrected size retained, rather
  than the partition table being rewritten as part of a board port

### Requirement: Connectivity is verified on the data path, not by association

Before a board is documented as verified on hardware, the port SHALL demonstrate
end-to-end data-path connectivity: an IPv4 address obtained by DHCP, reachability
to the gateway, discovery over mDNS, and a successful protocol round-trip from an
external client. Wi-Fi association alone SHALL NOT be reported as verification.

#### Scenario: Board passes end-to-end verification

- **WHEN** a board runs an application and an external client on the LAN
  discovers it by its mDNS name and completes a protocol read
- **THEN** the board is documented as verified on hardware for that application

#### Scenario: Board associates but cannot pass traffic

- **WHEN** a board associates and obtains a DHCP lease but its IP data path does
  not pass traffic
- **THEN** it is documented as building and flashing but **not** verified, with
  the observed failure recorded
- **AND** it is not presented as a deployment target

### Requirement: Board status is documented accurately

The repository documentation SHALL list every board target with the state it has
actually reached — verified on hardware, builds only, or config authored and
unbuilt — so a reader can tell which targets are trustworthy without running
them.

#### Scenario: Targets table reflects reality

- **WHEN** the targets table is read after a board port
- **THEN** each board's row states its true verification state, and any board
  with a known defect links to the note describing it

#### Scenario: Flashing procedure documented per board

- **WHEN** a developer flashes a newly supported board
- **THEN** the documentation gives that board's chip name, flash offset, reset
  behaviour, and any minimum tool version its chip requires
