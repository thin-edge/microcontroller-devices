## MODIFIED Requirements

### Requirement: Releases are built from a version tag

The workflow SHALL build every manifest entry and publish a GitHub Release
with all assets attached, for a version tag of the form `vMAJOR.MINOR.PATCH`
or `vMAJOR.MINOR.PATCH-<pre>`. The tag can be pushed, or created by merging
the release PR, in which case the workflow is called with the tag as input.
A pre-release tag SHALL publish a pre-release. The release SHALL NOT be
published unless every build succeeded. If a draft release for the tag
already exists, the workflow SHALL attach the assets to it, add the firmware
notes after its body, and publish it. Otherwise it SHALL create the release.
A tag of any other form SHALL fail without building.

#### Scenario: Successful release

- **WHEN** `v0.4.0` is pushed and every build succeeds
- **THEN** a release `v0.4.0` exists with, for every manifest build, a
  factory image, an application image and a bundle, plus
  `c8y-firmware.json` and `SHA256SUMS`

#### Scenario: One board fails

- **WHEN** one build in the matrix fails
- **THEN** the other builds still run to completion and report, and no
  release is published

#### Scenario: Pre-release

- **WHEN** `v0.4.0-rc1` is pushed
- **THEN** the release is marked as a pre-release

#### Scenario: Draft from the release PR

- **WHEN** the workflow is called for `v0.7.0` and a draft release `v0.7.0`
  exists
- **THEN** after every build succeeds the draft gets the assets and the
  firmware notes after its changelog, and is published; no second release is
  created

### Requirement: One version for all applications

All `apps/*/VERSION` files SHALL hold the same `MAJOR.MINOR.PATCH`. It SHALL
be changed only in a commit, either by the release bump script or by
release-please's release PR. A release tag `vX.Y.Z[-pre]` SHALL only build
if every file holds `X.Y.Z`. The build SHALL NOT change the version numbers;
it only sets `EXTRAVERSION` in its own checkout. `<pre>` SHALL be lowercase
letters, digits and dots, which Zephyr accepts as `EXTRAVERSION`. Every image
SHALL carry that version in its MCUboot header and its
`APP_VERSION_STRING`, with `-<pre>` appended to the latter for a
pre-release tag and `-dev` for a non-tag build. `tedge-zephyr/VERSION` SHALL
be versioned independently, and the release notes SHALL state it for every
`tedge` image.

#### Scenario: Bumping for a release

- **WHEN** a maintainer runs the bump script with `0.4.0`
- **THEN** all five application VERSION files read `0.4.0`, the
  release-please manifest says `0.4.0`, and nothing else changes

#### Scenario: Bumped by the release PR

- **WHEN** release-please's release PR proposes `0.7.0`
- **THEN** all five application VERSION files in it read `0.7.0`, and
  `tedge-zephyr/VERSION` is unchanged

#### Scenario: Tag does not match the files

- **WHEN** `v0.5.0` is pushed on a commit whose VERSION files say `0.4.0`
- **THEN** the workflow fails before building and names the mismatch

#### Scenario: Files disagree

- **WHEN** a pull request leaves `apps/snmp-agent/VERSION` at a different
  version from the other apps
- **THEN** the release-matrix check fails and names the file

#### Scenario: Version reported in the cloud

- **WHEN** a `tedge` image from release `v0.4.0` connects to Cumulocity
- **THEN** the device's firmware version is shown as `0.4.0`

#### Scenario: Update from the previous release

- **WHEN** a device running release `v0.4.0` is offered the `v0.4.1`
  application image through Cumulocity firmware update
- **THEN** the update is accepted, since the versions differ

#### Scenario: CI artifact

- **WHEN** a build runs for a pull request
- **THEN** its `APP_VERSION_STRING` ends in `-dev` and its artifact names
  say `dev`
