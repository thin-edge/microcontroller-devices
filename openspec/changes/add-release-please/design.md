## Context

There are two ways a release starts today:

1. **By hand.** `scripts/release/bump.sh X.Y.Z` rewrites the five
   `apps/*/VERSION` files. The maintainer commits
   `chore(release): vX.Y.Z` and pushes a `vX.Y.Z` tag.
2. **From the tag push.** Pushing that tag triggers
   `.github/workflows/release.yml`. The workflow checks the tag against the
   files (`check-version.sh`), builds every entry in `release/devices.yml`,
   and only then runs `gh release create` with the assets and the notes from
   `notes.py`.

The workflow publishes only if every build succeeds. That guarantee has to
survive this change.

The commit history already follows Conventional Commits. `tedge-zephyr/VERSION`
is the module's own version and is not part of the release version.

One constraint of GitHub Actions shapes the design:

- Anything the default `GITHUB_TOKEN` does (pushing a tag, opening a PR,
  publishing a release) does **not** start other workflows. A tag that
  release-please created with it would never trigger `release.yml`'s
  `push: tags`, and its release PR would get no checks. The organisation
  provides `COMMUNITY_ACTIONS_PAT`, which does start workflows.

## Goals / Non-Goals

**Goals:**
- Merging one PR performs the whole release: version bump, changelog, tag,
  build and publish.
- The next version comes from the commit types, and a maintainer can
  override it.
- Nothing is published unless every build succeeds, as today.
- The manual path (`bump.sh` + tag push) still works, for pre-releases and
  for recovery.
- No new secrets: `COMMUNITY_ACTIONS_PAT` is an existing organisation
  secret that the repository can already see.

**Non-Goals:**
- A separate release-please package for `tedge-zephyr`.
- Pre-release automation, for-each-board release notes in the changelog, or
  uploading to Cumulocity.

## Decisions

### 1. Single root package, `simple` release type, generic updater for VERSION

`release-please-config.json` defines one package at `.`:
`release-type: simple`, `include-component-in-tag: false`,
`include-v-in-tag: true`. The `extra-files` list holds the five
`apps/*/VERSION` paths with `type: generic`.
`.release-please-manifest.json` starts at `{".": "0.6.0"}`.

- **Why generic with markers**: Zephyr VERSION is `KEY = value` lines with
  the version split across three keys. release-please has no updater for
  this format. Its generic updater, however, replaces the number on any line
  marked `x-release-please-major`, `-minor` or `-patch`. The files become:

  ```
  VERSION_MAJOR = 0 # x-release-please-major
  VERSION_MINOR = 6 # x-release-please-minor
  PATCHLEVEL = 0 # x-release-please-patch
  VERSION_TWEAK = 0
  EXTRAVERSION =
  ```

  Zephyr's `cmake/modules/version.cmake` matches
  `VERSION_MAJOR = ([0-9]*)`, so it ignores the trailer. `check-version.sh`'s
  `field` sed also stops at the first non-digit. Both must be checked (see
  tasks).
- **Alternatives considered**: a `version.txt` as the source of truth, with
  a script regenerating the VERSION files. That needs a second step inside
  the release PR, which release-please cannot run, so the PR would be wrong
  until CI fixed it. A custom updater (a TypeScript plugin) is too much to
  maintain for five files.
- **`simple`'s own `version.txt`**: the `simple` type also writes a
  `version.txt`. Setting `version-file` to one of the VERSION files would
  make it overwrite that file wholesale, so it must not be set. The
  `version.txt` is kept. It is harmless and gives scripts a plain version to
  read, but nothing in the build depends on it.

### 2. Version bump rules

`bump-minor-pre-major: false`, so a breaking change goes to `1.0.0` while
the version is below 1.0. This follows the README rule that MAJOR means
"needs a reflash by cable". `bump-patch-for-minor-pre-major: false`, so
`feat` bumps MINOR, which is what v0.5.0 and v0.6.0 did by hand.
`Release-As: X.Y.Z` in a commit body overrides the number.

Both settings are release-please's defaults, so this rule costs no extra
config. It is spelled out in the config anyway, so a reader doesn't need to
know the defaults.

- **Alternative**: `bump-minor-pre-major: true` (the common choice for 0.x).
  Rejected because it contradicts the documented rule.

### 3. release-please runs as `COMMUNITY_ACTIONS_PAT`, and the tag starts the build

`.github/workflows/release-please.yml` runs on `push: main` and uses
`googleapis/release-please-action@v5` with
`token: ${{ secrets.COMMUNITY_ACTIONS_PAT }}`. Because the tag is created
with a PAT, it triggers `release.yml`'s existing `push: tags`, exactly like a
tag pushed by hand. So the build needs no new trigger and no inputs, and
both paths run the same code. Because the release PR is opened with the
PAT, it also gets the `pr-title` check and the PR build.

- **Alternative (first draft of this design)**: keep `GITHUB_TOKEN` and have
  release-please.yml call `release.yml` through `workflow_call` with the
  tag. That needs no secret, but it means a `tag` input threaded through every
  ref in `release.yml`, and the release PR gets no checks. With a PAT, adding
  the call would build every image twice.

### 4. Draft first, publish only after every build

The config sets `draft: true` and `force-tag-creation: true`. Merging the
release PR then creates the tag and a draft release whose body is the
changelog entry. The release job in `release.yml`:

- If a release for the tag exists (release-please's draft), it uploads the
  assets (`gh release upload --clobber`), sets the body to the changelog
  followed by a rule and the `notes.py` output, and runs
  `gh release edit --draft=false`, adding `--prerelease` for a `-pre` tag.
- If no release exists (a manual tag push), it runs `gh release create` as
  today.

The job still `needs: build`, so a failing board leaves only an unpublished
draft, and the tag, behind.

- **Why draft**: without it, release-please publishes an empty release as
  soon as the PR is merged, before any image exists. That breaks the "only if
  every build succeeded" guarantee, and watchers would get a release with no
  assets.
- **Why `force-tag-creation`**: a draft release has no tag until it is
  published. The tag has to exist, because it is what starts the build. Action v5.0.0 bundles release-please 17.6.0, which creates the
  tag through the API (`git.createRef` on the release commit) before it
  creates the draft.

### 5. `bump.sh` stays, and keeps the markers

`bump.sh` writes the markers into the heredoc, so a manual bump and a
release-please bump produce identical files. It stays the documented way to
start a pre-release (`bump.sh 0.7.0`, commit, tag `v0.7.0-rc1`). After a
manual bump, `.release-please-manifest.json` must be set to the same version,
so the script updates it too.

### 6. Squash-merge, with the PR title as the commit

release-please recommends squash-merging: each PR becomes one commit on
`main`, which gives one changelog entry per PR and a history that is linear
and easy to revert. The repository is set to allow squash merges only, with
the default commit message set to *Pull request title and description*. The
PR title therefore becomes the Conventional Commit that release-please reads.
A breaking change is marked with `!` in the title or a `BREAKING CHANGE:`
footer in the description.

A `pr-title` workflow (`amannn/action-semantic-pull-request`) checks each
PR title against the allowed types. Nothing else checks the title, and a
title that doesn't parse would silently leave the PR out of the release.

- **Alternative**: keep merge commits. release-please reads every commit in
  a merged branch, so it works too. But the changelog then lists every
  intermediate commit, including fix-ups, and a branch with one stray
  non-conventional commit is harder to spot.

## Risks / Trade-offs

- [The tag starts the build before release-please has created the draft]
  → release-please creates the draft straight after the tag, and the publish
  job runs only after every board has built, minutes later. If the draft
  were somehow missing, the job would create the release itself, without the
  changelog.
- [`COMMUNITY_ACTIONS_PAT` expires or loses access] → release-please fails
  on the next push to `main`, and nothing is released. Renew the secret. The
  manual path (`bump.sh` + tag) does not use it.
- [A failed build leaves a tag and a draft behind] → Fix it on main and
  re-run the failed jobs. Or delete the draft and tag and let release-please
  cut the next patch. README documents this.
- [The generic updater misses a marker (a typo, or a whitespace change)] →
  `check-version.sh` fails the release on mismatched files. A unit test in
  `scripts/release/tests` checks that every `apps/*/VERSION` carries all
  three markers.
- [A squash-merged PR with a non-conventional title drops out of the
  changelog and bumps nothing] → A PR-title check (decision 6) fails the PR
  until its title parses.
- [A maintainer pushes a manual tag and release-please doesn't know] →
  `bump.sh` also updates `.release-please-manifest.json`. release-please
  also uses the latest matching tag or release when it computes the next
  version.

## Migration Plan

1. Land config, manifest (`0.6.0`), markers, workflow changes and README in
   one PR. Set `bootstrap-sha` to the v0.6.0 release commit (`36d3d80`), so
   the first changelog covers only commits after it.
2. After merge, release-please opens `chore(main): release 0.6.1` or
   `0.7.0`, depending on what has landed. Merge it when ready and watch the
   called build publish the draft.
3. Rollback: delete `release-please.yml`, and the manual path is exactly as
   it is today. The markers are harmless and can stay.

## Open Questions

None. Breaking changes below 1.0 bump MAJOR, following the README rule, and
feature PRs are squash-merged (decisions 2 and 6).
