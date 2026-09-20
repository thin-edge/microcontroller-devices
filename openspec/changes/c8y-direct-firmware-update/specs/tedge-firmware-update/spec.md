## ADDED Requirements

### Requirement: The device installs a firmware image the cloud offers

When the firmware-update feature is built in, the client SHALL handle the
cloud's firmware operation by downloading the image at the given URL into the
bootloader's secondary slot, requesting a test boot and resetting the device
through the platform reset. It SHALL mark the operation as executing before
the download starts and SHALL name the expected time the device will be
offline while the bootloader swaps.

#### Scenario: An operator installs a new version

- **WHEN** an operator installs a firmware version on the device
- **THEN** the device downloads it, reboots into it, and the operation ends
  successfully once the new image is running and connected

#### Scenario: The download fails

- **WHEN** the image cannot be downloaded (the host is unreachable, or the
  transfer breaks)
- **THEN** the operation fails with the reason, the running image is
  untouched, and the device stays connected

### Requirement: A new image is confirmed only after it proves itself

A newly installed image SHALL be confirmed only after it has connected to the
cloud and the application's firmware-check hook, if any, has passed. An image
that does not confirm SHALL be reverted by the bootloader on the next reset.
After a revert, the client SHALL report the operation as failed, naming the
version that did not come up.

#### Scenario: The new image cannot connect

- **WHEN** the new image runs but never reaches the cloud
- **THEN** it stays unconfirmed, the bootloader restores the previous image
  on the next reset, and that image reports the failure with the version

#### Scenario: The application refuses the new image

- **WHEN** the new image connects but the application's firmware-check hook
  refuses it
- **THEN** the image is not confirmed and is reverted, and the failure names
  the application's reason

#### Scenario: A corrupt image reaches the slot

- **WHEN** the downloaded image fails the bootloader's signature check
- **THEN** the previous image boots, the slot is discarded, and the operation
  is reported as failed

### Requirement: Downloads follow redirects and keep the token to the tenant

The client SHALL follow HTTP redirects up to a configured limit, including to
another host or scheme, and SHALL handle a redirect target of at least one
kilobyte. It SHALL send its cloud token only to hosts within the tenant's own
domain, and SHALL drop it when a redirect leaves that domain. It SHALL write
the image as the parser reports each body segment, so that a chunked transfer
is stored exactly as sent.

#### Scenario: A release asset on another host

- **WHEN** the firmware URL redirects to a download host outside the tenant
- **THEN** the client follows the redirect, sends no token to that host, and
  stores the image correctly

#### Scenario: A chunked transfer

- **WHEN** the cloud sends the image with chunked transfer encoding
- **THEN** the stored image matches what was sent, and the bootloader accepts
  it

### Requirement: The running firmware version is visible in the cloud

The client SHALL report the name and version of the image that is actually
running, on every connect, so that the cloud shows what the device runs
rather than what was last installed.

#### Scenario: After an update

- **WHEN** a device has been updated and reconnects
- **THEN** the cloud shows the new version

#### Scenario: After a revert

- **WHEN** an image is reverted and the previous one reconnects
- **THEN** the cloud shows the previous version, not the one that failed
