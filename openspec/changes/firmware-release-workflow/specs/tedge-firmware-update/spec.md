## MODIFIED Requirements

### Requirement: The running firmware version is visible in the cloud

The client SHALL report the name and version of the image that is actually
running, on every connect, so that the cloud shows what the device runs
rather than what was last installed. The version SHALL be the application's
full version string (`tedge_identity.firmware_version`, including any
pre-release suffix such as `-rc1`), falling back to the bootloader image
header's `MAJOR.MINOR.PATCH` only when the application supplies none. The
same string SHALL be used to refuse a request for the version already
running and to decide, after the reboot, whether the requested version came
up or was reverted.

#### Scenario: After an update

- **WHEN** a device has been updated and reconnects
- **THEN** the cloud shows the new version

#### Scenario: After a revert

- **WHEN** an image is reverted and the previous one reconnects
- **THEN** the cloud shows the previous version, not the one that failed

#### Scenario: A pre-release installs as itself

- **WHEN** a device installs version `0.4.0-rc1` and the image comes up
- **THEN** the operation succeeds and the cloud shows `0.4.0-rc1`, not
  `0.4.0` and not a rollback

#### Scenario: Final release over its pre-release

- **WHEN** a device running `0.4.0-rc1` is asked to install `0.4.0`
- **THEN** the request is accepted, since the versions differ

#### Scenario: Application supplies no version

- **WHEN** the application leaves `firmware_version` unset
- **THEN** the client reports the image header's `MAJOR.MINOR.PATCH`
