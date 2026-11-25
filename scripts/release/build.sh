#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Build, size-check and package one release build, exactly as the release
# workflow does. Run it inside the Zephyr build container:
#
#   scripts/release/build.sh <firmware-name> [--build-dir DIR] [--out DIR]
#                            [--write-baseline]
#
#   docker exec -w /ws/app -e ZEPHYR_SDK_INSTALL_DIR=$SDK zephyr-dev \
#     scripts/release/build.sh modbus-server-zephyr-full-esp32c6-devkitc
#
# <firmware-name> is <app>-<os>-<set>-<device> from release/devices.yml
# (scripts/release/matrix.py lists them). The version is whatever the
# apps/*/VERSION files say; run scripts/release/check-version.sh --apply
# first to set a pre-release or dev suffix. KEY_FILE, when set, signs with that
# key instead of MCUboot's development key.
#
# An ESP-IDF build (`framework: esp-idf` in release/devices.yml) runs idf.py
# in apps/<app> instead, inside Espressif's ESP-IDF container with ESP-IDF's
# environment exported; scripts/release/package_idf.py packages it and fails
# it when the application takes more than 80 % of its OTA slot.
#
# A NuttX build (`framework: nuttx`) is tiny-edge's NuttX agent: it runs
# tiny-edge's ports/nuttx/scripts/build.sh for the device's `nuttx_board`,
# inside tiny-edge's NuttX container (ports/nuttx/docker), with the release's
# firmware name and apps/<app>/VERSION, and the build's `extra_conf` as
# NuttX fragments. TINY_EDGE_DIR names the tiny-edge checkout where west is
# not installed; NUTTX_WS, NuttX's workspace (default: in the build
# directory). scripts/release/package_nuttx.py packages it.
#
# The size step (scripts/release/size.py) fails the build when an image is
# over its flash budget or when any RAM region or image has grown past the
# build's entry in release/size-baseline.json. A change that moves a build's
# size updates that entry with --write-baseline (scripts/release/baseline.sh
# does every build) and commits it in the same pull request.
set -euo pipefail

[[ $# -ge 1 ]] || { sed -n '4,32p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
name=$1; shift
root=$(cd "$(dirname "$0")/../.." && pwd)
build=$root/build-release/$name
out=$root/dist
baseline=$root/release/size-baseline.json
size_args=()
while [[ $# -gt 0 ]]; do
	case $1 in
	--build-dir) build=$2; shift 2 ;;
	--out) out=$2; shift 2 ;;
	--write-baseline) size_args+=(--write-baseline); shift ;;
	*) echo "unknown option: $1" >&2; exit 1 ;;
	esac
done

# Relative to where the script was started: the ESP-IDF build runs from the
# application's directory.
case $build in /*) ;; *) build=$PWD/$build ;; esac
case $out in /*) ;; *) out=$PWD/$out ;; esac

eval "$(python3 "$root/scripts/release/matrix.py" --entry "$name")"

if [[ $FRAMEWORK == esp-idf ]]; then
	# The release's firmware name and the device's flash size, on top of
	# the application's own sdkconfig.defaults. A fresh build directory, as
	# --pristine gives the Zephyr builds.
	rm -rf "$build"
	mkdir -p "$build" "$out"
	printf 'CONFIG_APP_FIRMWARE_NAME="%s"\nCONFIG_ESPTOOLPY_FLASHSIZE_%s=y\n' \
		"$FIRMWARE_NAME" "$FLASH_SIZE" > "$build/release.defaults"
	(cd "$root/apps/$APP" && idf.py -B "$build" -D IDF_TARGET="$CHIP" \
		-D SDKCONFIG="$build/sdkconfig" \
		-D SDKCONFIG_DEFAULTS="sdkconfig.defaults;$build/release.defaults" build)
	python3 "$root/scripts/release/package_idf.py" "$build" --device "$DEVICE" \
		--app "$APP" --variant "$VARIANT" --out "$out"
	exit 0
fi

if [[ $FRAMEWORK == nuttx ]]; then
	te=$(python3 - "$root" <<'PY'
import sys
sys.path.insert(0, sys.argv[1] + "/scripts/release")
import matrix
print(matrix.west_project_dir("tiny-edge") or "")
PY
	)
	[[ -x $te/ports/nuttx/scripts/build.sh ]] || {
		echo "tiny-edge's NuttX port not found (set TINY_EDGE_DIR)" >&2; exit 1; }
	v=$root/apps/$APP/VERSION
	field() { sed -n "s/^$1 *= *\([^ #]*\).*/\1/p" "$v"; }
	version="$(field VERSION_MAJOR).$(field VERSION_MINOR).$(field PATCHLEVEL)"
	extra=$(field EXTRAVERSION)
	[[ -z $extra ]] || version="$version-$extra"
	rm -rf "$build"
	mkdir -p "$build" "$out"
	# The release's firmware name, which an update must carry.
	printf 'CONFIG_TINY_EDGE_FIRMWARE_NAME="%s"\n' "$FIRMWARE_NAME" \
		> "$build/release.conf"
	args=(--agent --version "$version")
	while IFS= read -r frag; do
		case $frag in "") continue ;; /*) ;; *) frag=$root/$frag ;; esac
		args+=(--extra "$frag")
	done < <(tr ';' '\n' <<< "$CONF_FILES")
	ws=${NUTTX_WS:-$build/nuttx}
	NUTTX_WS=$ws "$te/ports/nuttx/scripts/build.sh" \
		"$NUTTX_BOARD" "${args[@]}" --extra "$build/release.conf" \
		--out "$build/out"
	python3 "$root/scripts/release/package_nuttx.py" "$build/out" \
		--config "$ws/nuttx/.config" \
		--device "$DEVICE" --app "$APP" --variant "$VARIANT" --out "$out" \
		--version "$version" --tiny-edge "$te"
	exit 0
fi

abs() { # "a;/x/b" repository-relative or absolute -> "/root/a;/x/b"
	[[ -n $1 ]] || return 0
	tr ';' '\n' <<< "$1" | sed "s#^\([^/]\)#$root/\1#" | paste -sd';' -
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
mkdir -p "$out"
if ! python3 "$root/scripts/release/size.py" "$build" --json "$tmp" \
	--baseline "$baseline" ${size_args[@]+"${size_args[@]}"}; then
	# Keep the report of a build that failed the size gate, so the figures a
	# baseline update needs come out of the CI run (its firmware artifact).
	[[ -s $tmp ]] && mv "$tmp" "$out/$FIRMWARE_NAME.size.json"
	exit 1
fi
python3 "$root/scripts/release/package.py" "$build" --device "$DEVICE" \
	--app "$APP" --variant "$VARIANT" --out "$out"
version=$(sed -n 's/.*"version": "\(.*\)",/\1/p' "$out/$FIRMWARE_NAME-"*.meta.json | head -1)
mv "$tmp" "$out/$FIRMWARE_NAME-$version.size.json"
