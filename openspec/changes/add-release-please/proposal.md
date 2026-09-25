## Why

A release is done by hand today. A maintainer picks the next number from the
commit log, runs `scripts/release/bump.sh`, commits, tags and pushes. Nothing
records what changed between releases except the log itself. The commits
already follow Conventional Commits (`feat:`, `fix:`, `chore(release):`), so
release-please can do all of this: it keeps a release pull request open with
the next version and a changelog, and when that PR is merged it tags the
commit and creates the release. The v0.6.0 release showed the cost of doing it
by hand: every step, and the choice of version, happened manually.

## What Changes

- Add a `release-please` workflow that runs on every push to `main`. It keeps
  one open release PR. That PR bumps every `apps/*/VERSION` to the next
  version and adds the new entries to `CHANGELOG.md`.
- Add `release-please-config.json` and `.release-please-manifest.json`. They
  set a single root package at `0.6.0` with `v`-prefixed tags and no
  component name, and they point release-please's generic updater at the five
  `apps/*/VERSION` files.
- Add `x-release-please-major/minor/patch` markers to the `VERSION_MAJOR`,
  `VERSION_MINOR` and `PATCHLEVEL` lines of each `apps/*/VERSION`. The generic
  updater needs these markers to find the numbers, because Zephyr's VERSION
  format is not a format release-please knows. `bump.sh` keeps the markers
  when it rewrites the files.
- Change how the firmware release is published. Merging the release PR
  creates a **draft** GitHub Release and its tag. release-please runs as the
  organisation secret `COMMUNITY_ACTIONS_PAT`, so the tag starts the
  existing build workflow like a hand-pushed tag. That workflow uploads the images,
  `c8y-firmware.json` and `SHA256SUMS`, and adds the firmware notes from
  `notes.py` under release-please's changelog. It publishes the draft only if
  every build succeeded.
- Keep pushing a tag by hand for pre-releases (`v0.7.0-rc1`) and for a manual
  re-run. The workflow creates the release itself when none exists, as it
  does today.
- Map commit types to version bumps: `fix`/`perf` bump PATCH, `feat` bumps
  MINOR, and a breaking change (`!` or a `BREAKING CHANGE:` footer) bumps
  MAJOR. This follows the README rule that MAJOR means "reflash by cable".
  `Release-As: X.Y.Z` in a commit body overrides the choice.
- Squash-merge PRs, with the PR title as the commit message (the merge
  style release-please recommends), and add a check that PR titles are
  Conventional Commits.
- Update README "Releasing": merging the release PR is now the normal path,
  and `bump.sh` plus a manual tag remain the fallback.

## Capabilities

### New Capabilities

- `release-automation`: a release PR keeps the next version and the
  changelog up to date from Conventional Commits. Merging it tags the release
  and starts the firmware build and publish.

### Modified Capabilities

- `firmware-release`: "Releases are built from a version tag" now covers a
  release PR merge, where the draft is published only after every build
  succeeds, alongside a pushed tag. "One version for all applications" now
  lets release-please change the VERSION files in its release PR, as well as
  the bump script.

## Impact

- New files: `.github/workflows/release-please.yml`,
  `release-please-config.json`, `.release-please-manifest.json` and
  `CHANGELOG.md` (created by the first release PR).
- Changed files: `.github/workflows/release.yml` (publishes an existing
  draft instead of always creating a release), `apps/*/VERSION` (markers only),
  `scripts/release/bump.sh`, `scripts/release/check-version.sh` (must still
  parse lines that carry a trailing marker) and README "Releasing".
- New workflow `.github/workflows/pr-title.yml`.
- Repository settings: squash merges only, with the PR title and description
  as the default message.
- Secret: the existing organisation secret `COMMUNITY_ACTIONS_PAT`, which
  the repository can already see. Tags and PRs created with it start
  workflows, so the tag builds and the release PR gets checks.
- Firmware: nothing changes on the device. Zephyr's `version.cmake` reads
  `VERSION_MAJOR = <digits>` and ignores the trailing `#` marker. The images'
  MCUboot header and `APP_VERSION_STRING` must be checked to be unchanged.
  There is no flash or RAM cost.
- Phase: tooling that serves every phase. No board, protocol, Kconfig option
  or `tedge_*` API changes. `tedge-zephyr/VERSION` stays out of
  release-please and is still versioned by hand.

## Non-goals

- Versioning `tedge-zephyr` separately with release-please (a second
  package). That can come later, when the module moves to its own repository.
- Automating pre-releases through release-please's prerelease mode. `rc`
  tags stay manual.
- Uploading to Cumulocity (`c8y-upload.sh`) from CI.
- Rewriting past history into the changelog. It starts after v0.6.0.
