## 0. Prerequisite

- [x] 0.1 Finish and archive the `firmware-release-workflow` change, so
  `openspec/specs/firmware-release/spec.md` exists for this change's
  MODIFIED delta

## 1. VERSION markers (firmware source, no device behaviour change)

- [x] 1.1 Add `# x-release-please-major/minor/patch` trailers to the three
  number lines of every `apps/*/VERSION`
- [x] 1.2 Confirm `check-version.sh v0.6.0` and `--apply` still work with the
  trailers, and that `--apply` rewrites `EXTRAVERSION` without touching them
- [x] 1.3 Build one image in the container (e.g.
  `modbus-server-tedge-full-esp32c6-devkitc`) with and without the trailers.
  Confirm the MCUboot header version (`imgtool dumpinfo`) and
  `APP_VERSION_STRING` are identical, and the size report is unchanged
- [x] 1.4 Update `bump.sh` to write the trailers and to set
  `.release-please-manifest.json` to the new version
- [x] 1.5 Add `scripts/release/tests/test_version.py`: every
  `apps/*/VERSION` has all three markers, `bump.sh` round-trips (numbers,
  markers, manifest), and the manifest matches the files

## 2. release-please configuration

- [x] 2.1 Add `release-please-config.json`: root package `.`,
  `release-type: simple`, `include-component-in-tag: false`,
  `include-v-in-tag: true`, `draft: true`, `force-tag-creation: true`,
  `bump-minor-pre-major: false`, `bump-patch-for-minor-pre-major: false`,
  `bootstrap-sha` = the v0.6.0 commit, `extra-files` = the five VERSION
  files as `generic`, and changelog sections (Features, Bug Fixes,
  Performance; hide docs/ci/chore/test)
- [x] 2.2 Add `.release-please-manifest.json` = `{".": "0.6.0"}`
- [x] 2.3 Dry run locally with `npx release-please release-pr --dry-run`
  against the repo, or on a fork. Check that the proposed version, the
  VERSION diffs (only numbers change, markers kept) and the changelog
  contents are right

## 3. Workflows

- [x] 3.1 `release.yml`: no new trigger. The release-please tag is created with
  `COMMUNITY_ACTIONS_PAT`, so it starts the existing `push: tags`. (The
  first implementation added `workflow_call` with a `tag` input; it was
  removed when the PAT was adopted.)
- [x] 3.2 `release.yml` release job: if a release for the tag exists,
  `gh release upload --clobber` the assets, set the body to the existing
  body + `---` + `notes.md`, and `gh release edit --draft=false` (plus
  `--prerelease` for `-pre`). Otherwise keep `gh release create`
- [x] 3.3 Add `.github/workflows/release-please.yml`: on `push: main`, run
  `googleapis/release-please-action@v5` (config + manifest files) with
  `token: ${{ secrets.COMMUNITY_ACTIONS_PAT }}`
- [x] 3.4 Confirm the tag is created with the draft: action v5.0.0 bundles
  release-please 17.6.0, whose `force-tag-creation` creates the tag through
  the API before the draft release
- [x] 3.5 Add `release-please-config.json`, `.release-please-manifest.json`
  and `.github/workflows/release-please.yml` to `release.yml`'s
  `pull_request` paths, so a config change gets the PR build

- [x] 3.6 Add `.github/workflows/pr-title.yml` using
  `amannn/action-semantic-pull-request` (on `pull_request_target`:
  opened, edited, synchronize), allowing the types used in the history:
  feat, fix, perf, docs, ci, chore, test, refactor, build, revert

## 4. Documentation

- [x] 4.1 Rewrite README "Releasing": the release PR is the normal path.
  Cover the commit types and what they bump, `Release-As:`, the draft,
  publish-after-build flow, what to do when a build fails after merge, and
  the manual path (`bump.sh` + tag) for pre-releases
- [x] 4.2 Document in README that PRs are squash-merged, that the PR title
  is the Conventional Commit, and how to mark a breaking change (`!` or a
  `BREAKING CHANGE:` footer in the description)

## 5. Rollout and verification

- [x] 5.1 Confirm the repository can read the organisation secret
  `COMMUNITY_ACTIONS_PAT` (listed under the repository's organisation
  secrets)
- [ ] 5.2 Repository settings: allow squash merging only, with the default
  commit message set to "Pull request title and description"
- [ ] 5.3 Merge the change and confirm release-please opens a release PR (or
  correctly opens none, if only non-releasable commits have landed)
- [ ] 5.4 Merge the first release PR. Confirm the draft and tag are created,
  the called build runs every group, and the release is published with the
  changelog followed by the firmware notes and all assets
- [ ] 5.5 Push a `vX.Y.Z-rc1` tag by hand after `bump.sh` and confirm the
  tag-push path still creates a pre-release
