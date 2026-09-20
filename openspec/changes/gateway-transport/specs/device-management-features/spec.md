## MODIFIED Requirements

### Requirement: Feature dependencies are enforced at configure time

Kconfig SHALL express what each feature needs, so that an image that cannot
work is refused at configure time rather than at run time. Features that
transfer files (firmware update, log upload, parameters where they transfer
anything) SHALL bring in the HTTP client. Firmware update SHALL require a
bootloader that can swap images. A feature that exists only on one transport
SHALL depend on that transport, and its help SHALL say why.

#### Scenario: Firmware update without a bootloader

- **WHEN** an image selects firmware update without MCUboot
- **THEN** the configuration fails, naming the missing dependency

#### Scenario: File transfer brings in what it needs

- **WHEN** an image selects log upload
- **THEN** the HTTP client is part of the build without the application
  asking for it

#### Scenario: A feature that has no equivalent on the gateway

- **WHEN** an image selects the gateway transport together with remote
  access or certificate renewal
- **THEN** those features are not selectable, and the reason is stated where
  an integrator will read it
