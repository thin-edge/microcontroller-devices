## ADDED Requirements

### Requirement: A release PR tracks the next version

On every push to `main`, the release-please workflow SHALL open, or update, a
single release pull request. That PR SHALL set every `apps/*/VERSION` to the
next version, update `.release-please-manifest.json`, and add an entry for
that version to `CHANGELOG.md` listing the Conventional Commits since the
last release. It SHALL NOT change `tedge-zephyr/VERSION`.

#### Scenario: A fix lands on main

- **WHEN** a `fix:` commit is pushed to `main` after release `v0.6.0`
- **THEN** a release PR proposes `0.6.1`, and all five `apps/*/VERSION`
  files in it read `0.6.1`

#### Scenario: A feature lands after a fix

- **WHEN** a `feat:` commit is pushed to `main` while the release PR
  proposes `0.6.1`
- **THEN** the same PR is updated to propose `0.7.0`, and its changelog
  lists both commits

#### Scenario: Breaking change

- **WHEN** a commit with `!` or a `BREAKING CHANGE:` footer lands on `main`
- **THEN** the release PR proposes the next MAJOR version

#### Scenario: Chosen version

- **WHEN** a commit body carries `Release-As: 1.0.0`
- **THEN** the release PR proposes `1.0.0`

#### Scenario: Only non-releasable commits

- **WHEN** only `docs:`, `ci:` or `chore:` commits have landed since the last
  release
- **THEN** no release PR is opened

### Requirement: Merging the release PR releases the firmware

When the release PR is merged, the workflow SHALL create the tag `vX.Y.Z`
and a draft GitHub Release carrying the changelog entry. The tag SHALL be
created with a token whose events start workflows (`COMMUNITY_ACTIONS_PAT`),
so it starts the firmware build exactly as a hand-pushed tag does. The draft
SHALL be published, with the images, `c8y-firmware.json`, `SHA256SUMS` and
the firmware notes after the changelog, only after every build has
succeeded.

#### Scenario: Release PR merged

- **WHEN** the release PR for `0.7.0` is merged and every build succeeds
- **THEN** release `v0.7.0` is published, not as a draft, with its body
  starting with the changelog entry and every asset attached

#### Scenario: A build fails after the merge

- **WHEN** the release PR is merged and one board's build fails
- **THEN** the release stays a draft with no assets, and the run names the
  failing job

#### Scenario: Release PR is checked

- **WHEN** release-please opens or updates the release PR
- **THEN** the PR's checks (`pr-title`, and the PR build, since VERSION files
  change) run on it

#### Scenario: Tag names the version

- **WHEN** the release PR for `0.7.0` is merged
- **THEN** the tag is `v0.7.0`, with no component prefix, and it points to
  the merge commit whose VERSION files read `0.7.0`

### Requirement: VERSION files carry release-please markers

Every `apps/*/VERSION` SHALL carry an `x-release-please-major`,
`x-release-please-minor` and `x-release-please-patch` marker on its
`VERSION_MAJOR`, `VERSION_MINOR` and `PATCHLEVEL` lines. The markers SHALL
NOT change the version the build reads. The bump script SHALL write them and
SHALL keep `.release-please-manifest.json` at the same version.

#### Scenario: Marker missing

- **WHEN** a pull request removes a marker from `apps/opcua-server/VERSION`
- **THEN** the release scripts' unit tests fail and name the file

#### Scenario: Build reads the version

- **WHEN** an image is built from VERSION files carrying the markers
- **THEN** its MCUboot header and `APP_VERSION_STRING` are the same as for
  the same numbers without markers

#### Scenario: Manual bump

- **WHEN** a maintainer runs `scripts/release/bump.sh 0.7.0`
- **THEN** every `apps/*/VERSION` reads `0.7.0` with its markers, and
  `.release-please-manifest.json` says `0.7.0`
