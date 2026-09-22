#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Set the one version every application shares.
#
#   scripts/release/bump.sh X.Y.Z
#
# Rewrites MAJOR/MINOR/PATCHLEVEL in every apps/*/VERSION (the protocol apps,
# tedge-agent and the Wi-Fi provisioner) and clears VERSION_TWEAK and
# EXTRAVERSION. Commit the result, then tag that commit vX.Y.Z; the release
# workflow checks the tag against these files (check-version.sh).
# tedge-zephyr/VERSION is the module's own and is not touched.
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
VERSION_MAJOR = $major
VERSION_MINOR = $minor
PATCHLEVEL = $patch
VERSION_TWEAK = 0
EXTRAVERSION =
VERSION
	echo "${f#"$root"/}: $1"
done
