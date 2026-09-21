#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Put a release's tedge images into a Cumulocity tenant's firmware repository,
# so devices can be updated over the air (Device management > Firmware, or
# `c8y firmware versions install`).
#
#   scripts/release/c8y-upload.sh <tag> [--repo OWNER/REPO] [--dry-run]
#   scripts/release/c8y-upload.sh --from-dir <dir> [--dry-run]
#
# <tag>        a release, e.g. v0.4.0 or v0.4.0-rc1; its *.app.bin, SHA256SUMS
#              and c8y-firmware.json are downloaded with `gh release download`
# --from-dir   use those files from a directory instead (a workflow run's
#              artifacts, say)
# --dry-run    check everything and say what would be created, create nothing
#
# Needs a go-c8y-cli session for the tenant (c8y sessions set), python3, and
# gh for <tag>. Idempotent: a firmware or version that already exists is
# reported and left alone. Every image is checked against SHA256SUMS before
# anything is uploaded.
#
# A device only accepts an image signed with the key its bootloader was
# built with; c8y-firmware.json says which key signed these.
set -euo pipefail

usage() { sed -n '4,25p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

repo=thin-edge/microcontroller-devices
tag="" dir="" dry=0
while [[ $# -gt 0 ]]; do
	case $1 in
	--repo) repo=$2; shift 2 ;;
	--from-dir) dir=$2; shift 2 ;;
	--dry-run) dry=1; shift ;;
	-h|--help) usage 0 ;;
	-*) echo "unknown option: $1" >&2; usage 1 ;;
	*) tag=$1; shift ;;
	esac
done
[[ -n $tag || -n $dir ]] || usage 1
[[ -z $tag || -z $dir ]] || { echo "give a tag or --from-dir, not both" >&2; exit 1; }

if [[ -n $tag ]]; then
	dir=$(mktemp -d)
	trap 'rm -rf "$dir"' EXIT
	echo "downloading $repo $tag"
	gh release download "$tag" --repo "$repo" --dir "$dir" \
		--pattern '*.app.bin' --pattern SHA256SUMS --pattern c8y-firmware.json
fi
[[ -f $dir/c8y-firmware.json ]] || { echo "$dir has no c8y-firmware.json" >&2; exit 1; }

# Name, version, device type, asset and checksum per image, tab-separated.
entries=$(python3 - "$dir/c8y-firmware.json" <<'PY'
import json, sys
m = json.load(open(sys.argv[1]))
for f in m["firmware"]:
    print("\t".join([f["name"], f["version"], f["deviceType"] or "", f["asset"], f["sha256"]]))
PY
)
[[ -n $entries ]] || { echo "c8y-firmware.json lists no tedge images" >&2; exit 1; }

# Every image must match both c8y-firmware.json and SHA256SUMS before
# anything leaves this machine.
sha() { if command -v sha256sum >/dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1; }
bad=0
while IFS=$'\t' read -r name version dtype asset sum; do
	file=$dir/$asset
	if [[ ! -f $file ]]; then
		echo "missing: $asset" >&2; bad=1; continue
	fi
	actual=$(sha "$file")
	listed=$([[ -f $dir/SHA256SUMS ]] && awk -v a="$asset" '$2 == a || $2 == "*"a {print $1}' "$dir/SHA256SUMS")
	if [[ $actual != "$sum" || ( -f $dir/SHA256SUMS && $actual != "$listed" ) ]]; then
		echo "checksum mismatch: $asset" >&2; bad=1
	fi
done <<< "$entries"
[[ $bad -eq 0 ]] || { echo "nothing uploaded" >&2; exit 1; }

c8y() { command c8y "$@" < /dev/null; }
firmware_id() { # exact name match; the name filter is a prefix/wildcard match
	c8y firmware list --name "$1" --pageSize 100 --select id,name -o csv |
		awk -F, -v n="$1" '$2 == n {print $1; exit}'
}

created=0
while IFS=$'\t' read -r name version dtype asset sum; do
	id=$(firmware_id "$name")
	if [[ -z $id ]]; then
		if [[ $dry -eq 1 ]]; then
			echo "would create firmware $name (device type ${dtype:-any})"
		else
			id=$(c8y firmware create --name "$name" ${dtype:+--deviceType "$dtype"} \
				--description "thin-edge.io microcontroller firmware" \
				--force --select id -o csv)
			echo "created firmware $name ($id)"
		fi
	fi
	if [[ -n $id ]] && [[ -n $(c8y firmware versions list --firmware "$id" \
			--version "$version" --select id -o csv) ]]; then
		echo "exists: $name $version"
		continue
	fi
	if [[ $dry -eq 1 ]]; then
		echo "would upload $name $version from $asset"
	else
		c8y firmware versions create --firmware "$id" --version "$version" \
			--file "$dir/$asset" --force --select id -o csv >/dev/null
		echo "uploaded: $name $version"
		created=$((created + 1))
	fi
done <<< "$entries"

first=$(head -1 <<< "$entries" | cut -f1,2)
echo
echo "Install on a device that reports the same firmware name, e.g.:"
echo "  c8y firmware versions install --device <device> --firmware ${first%%$'\t'*} --version ${first##*$'\t'}"
