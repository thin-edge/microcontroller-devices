## MODIFIED Requirements

### Requirement: Advertised capabilities match the build

The device SHALL advertise to the cloud or gateway only the operations and
capabilities of features compiled into the running image. This covers supported
operations, log types and configuration types. When it receives an operation for
a feature that is not compiled in, the device SHALL mark that operation as
failed with a reason that names the missing feature. It SHALL NOT ignore the
operation or leave it pending.

#### Scenario: Firmware update advertises itself and the running version

- **WHEN** an image with the firmware-update feature connects
- **THEN** its supported operations include firmware update, and it reports
  the name and version of the image that is running

#### Scenario: Supported operations reflect the image

- **WHEN** an image without the firmware-update feature connects
- **THEN** its supported-operations list does not include firmware update

#### Scenario: Operation for an absent feature

- **WHEN** the device receives an operation whose feature is not compiled in
- **THEN** it marks the operation as failed with a reason naming the feature
