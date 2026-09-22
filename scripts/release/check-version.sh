#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Check the applications' shared version against the ref being built, and
# say which EXTRAVERSION the build should use.
#
#   scripts/release/check-version.sh [--apply] <ref>
#
#   <ref> vX.Y.Z        every apps/*/VERSION must be X.Y.Z; EXTRAVERSION empty
#   <ref> vX.Y.Z-<pre>  as above; EXTRAVERSION=<pre>, so the application
#                       reports X.Y.Z-<pre> (the MCUboot header stays X.Y.Z)
#   anything else       the files must agree; EXTRAVERSION=dev
#
# <pre> is lowercase letters, digits and dots (rc1, beta.2): what Zephyr's
# EXTRAVERSION accepts. Prints "version=<X.Y.Z[-pre|-dev]>" and
# "extraversion=<...>" lines (the format of $GITHUB_OUTPUT). Exits non-zero,
# naming the file, on a mismatch.
#
# --apply also writes EXTRAVERSION into every apps/*/VERSION of this working
# copy, which is how the build picks it up. The version numbers are never
# changed; run it in CI's checkout, not in a tree you commit from.
set -euo pipefail

apply=0
[[ ${1:-} == --apply ]] && { apply=1; shift; }
[[ $# -eq 1 ]] || { echo "usage: $0 [--apply] <git ref or tag>" >&2; exit 1; }
ref=${1#refs/tags/}
root=$(cd "$(dirname "$0")/../.." && pwd)

field() { sed -n "s/^$1 *= *\([0-9]*\).*/\1/p" "$2"; }

base=""
for f in "$root"/apps/*/VERSION; do
	v="$(field VERSION_MAJOR "$f").$(field VERSION_MINOR "$f").$(field PATCHLEVEL "$f")"
	if [[ -z $base ]]; then
		base=$v first=${f#"$root"/}
	elif [[ $v != "$base" ]]; then
		echo "${f#"$root"/} says $v but $first says $base:" \
		     "every application shares one version (scripts/release/bump.sh)" >&2
		exit 1
	fi
done

if [[ $ref =~ ^v([0-9]+\.[0-9]+\.[0-9]+)(-([0-9a-z.]+))?$ ]]; then
	tag_version=${BASH_REMATCH[1]} pre=${BASH_REMATCH[3]}
	if [[ $tag_version != "$base" ]]; then
		echo "tag $ref is $tag_version but apps/*/VERSION say $base:" \
		     "run scripts/release/bump.sh $tag_version and tag that commit" >&2
		exit 1
	fi
	extra=$pre
elif [[ $ref == v* ]]; then
	echo "tag $ref is not vMAJOR.MINOR.PATCH[-pre] (pre: lowercase, digits, dots)" >&2
	exit 1
else
	extra=dev
fi

if [[ $apply -eq 1 ]]; then
	for f in "$root"/apps/*/VERSION; do
		sed -i.bak "s/^EXTRAVERSION *=.*/EXTRAVERSION = $extra/" "$f" && rm -f "$f.bak"
	done
fi

echo "version=${base}${extra:+-$extra}"
echo "extraversion=$extra"
