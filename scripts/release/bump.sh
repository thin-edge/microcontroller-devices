#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Set the one version every application shares.
#
#   scripts/release/bump.sh X.Y.Z
#
# Rewrites MAJOR/MINOR/PATCHLEVEL in every apps/*/VERSION (the protocol apps,
# tedge-agent and the Wi-Fi provisioner), clears VERSION_TWEAK and
# EXTRAVERSION, and sets .release-please-manifest.json to the same version.
# Commit the result, then tag that commit vX.Y.Z; the release workflow checks
# the tag against these files (check-version.sh).
# tedge-zephyr/VERSION is the module's own and is not touched.
#
# Normally release-please's release PR does this (README, "Releasing"); this
# script is for a pre-release or a release cut by hand. The
# x-release-please-* trailers tell release-please's generic updater which
# number is which; Zephyr reads only the digits before them.
set -euo pipefail

[[ $# -eq 1 ]] || { echo "usage: $0 X.Y.Z" >&2; exit 1; }
[[ $1 =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)$ ]] || {
	echo "not a MAJOR.MINOR.PATCH version: $1" >&2
	exit 1
}
major=${BASH_REMATCH[1]} minor=${BASH_REMATCH[2]} patch=${BASH_REMATCH[3]}

root=$(cd "$(dirname "$0")/../.." && pwd)
for f in "$root"/apps/*/VERSION; do
	cat > "$f" <<VERSION
VERSION_MAJOR = $major # x-release-please-major
VERSION_MINOR = $minor # x-release-please-minor
PATCHLEVEL = $patch # x-release-please-patch
VERSION_TWEAK = 0
EXTRAVERSION =
VERSION
	echo "${f#"$root"/}: $1"
done
printf '{\n  ".": "%s"\n}\n' "$1" > "$root/.release-please-manifest.json"
echo ".release-please-manifest.json: $1"
