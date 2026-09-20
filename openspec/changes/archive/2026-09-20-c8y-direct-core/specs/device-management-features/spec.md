## MODIFIED Requirements

### Requirement: Boards ship with a feature profile that fits them

Each supported board and application combination SHALL have a documented default
device-management profile, provided as a Kconfig overlay, that fits its measured
RAM and flash budget with headroom. A profile SHALL never select more concurrent
TLS sessions than the board's measured heap supports. The profiles SHALL follow
the measured results of `c8y-direct-spikes`: the ESP32-C6 uses the full
profile; the ESP32-S3-DevKitC-1 uses the full profile with the mbedTLS heap in
PSRAM; the ESP32-WROOM-32 uses the direct transport only as a remote-access
enabler with no protocol application, and otherwise the gateway transport.

#### Scenario: Constrained board

- **WHEN** the ESP32-WROOM-32 OPC-UA application is built with its default
  profile
- **THEN** the profile excludes the direct transport's TLS session, and the
  image boots and serves OPC-UA as before

#### Scenario: Larger board

- **WHEN** the ESP32-C6 application is built with its default profile
- **THEN** all implemented features are included, and the documented free heap
  after connecting stays above the margin set in the footprint table

#### Scenario: Board with PSRAM

- **WHEN** the ESP32-S3-DevKitC-1 application is built with its default
  profile
- **THEN** the mbedTLS heap is placed in PSRAM, and all implemented features
  are included without overflowing internal RAM
