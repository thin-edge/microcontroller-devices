#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Build, size-check and package one release build, exactly as the release
# workflow does. Run it inside the Zephyr build container:
#
#   scripts/release/build.sh <firmware-name> [--build-dir DIR] [--out DIR]
#
#   docker exec -w /ws/app -e ZEPHYR_SDK_INSTALL_DIR=$SDK zephyr-dev \
#     scripts/release/build.sh modbus-server-tedge-full-esp32c6-devkitc
#
# <firmware-name> is <app>-<variant>-<device> from release/devices.yml
# (scripts/release/matrix.py lists them). The version is whatever the
# apps/*/VERSION files say; run scripts/release/check-version.sh --apply
# first to set a pre-release or dev suffix. KEY_FILE, when set, signs with that
# key instead of MCUboot's development key.
set -euo pipefail

[[ $# -ge 1 ]] || { sed -n '4,17p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
name=$1; shift
root=$(cd "$(dirname "$0")/../.." && pwd)
build=$root/build-release/$name
out=$root/dist
while [[ $# -gt 0 ]]; do
	case $1 in
	--build-dir) build=$2; shift 2 ;;
	--out) out=$2; shift 2 ;;
	*) echo "unknown option: $1" >&2; exit 1 ;;
	esac
done

eval "$(python3 "$root/scripts/release/matrix.py" --entry "$name")"

abs() { # "a;b" repository-relative -> "/root/a;/root/b"
	[[ -n $1 ]] || return 0
	tr ';' '\n' <<< "$1" | sed "s#^#$root/#" | paste -sd';' -
}

args=("-DCONFIG_APP_FIRMWARE_NAME=\"$FIRMWARE_NAME\"")
conf=$(abs "$CONF_FILES")
[[ -z $conf ]] || args+=("-DEXTRA_CONF_FILE=$conf")
dtc=$(abs "$DTC_OVERLAYS")
[[ -z $dtc ]] || args+=("-DEXTRA_DTC_OVERLAY_FILE=$dtc"
			"-Dwifi-provisioner_EXTRA_DTC_OVERLAY_FILE=$dtc")
prov=$(abs "$PROVISIONER_CONF")
[[ -z $prov ]] || args+=("-Dwifi-provisioner_EXTRA_CONF_FILE=$prov")
[[ -z ${KEY_FILE:-} ]] || args+=("-DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE=\"$KEY_FILE\"")

(cd "$root" && west build --sysbuild -b "$BOARD" "apps/$APP" -d "$build" \
	--pristine -- "${args[@]}")

tmp=$(mktemp)
python3 "$root/scripts/release/size.py" "$build" --json "$tmp"
mkdir -p "$out"
python3 "$root/scripts/release/package.py" "$build" --device "$DEVICE" \
	--app "$APP" --variant "$VARIANT" --out "$out"
version=$(sed -n 's/.*"version": "\(.*\)",/\1/p' "$out/$FIRMWARE_NAME-"*.meta.json | head -1)
mv "$tmp" "$out/$FIRMWARE_NAME-$version.size.json"
