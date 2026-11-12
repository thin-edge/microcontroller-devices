#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Flash a BLE-provisioning board: MCUboot, the signed application into slot0
# and the signed Wi-Fi provisioner into prov_partition, each at the offset its
# build was linked for. Takes either a `west build --sysbuild` build directory
# or an extracted release bundle (a directory with flash.json). Runs on the
# host (esptool needs the USB port; Docker on macOS cannot reach it).
#
#   scripts/flash.sh <build-dir|bundle-dir> [options]
#
# Options:
#   --port PORT       serial port (default: esptool auto-detects)
#   --erase-storage   also erase storage (credentials, identity) and bootreq
#   --erase-all       erase the whole flash first (first flash over simple boot)
#   --app-only        write only the application (slot0)
#   --before MODE     esptool reset mode (default-reset; usb-reset for some
#                     native-USB boards, e.g. the QT Py S3; a bundle names
#                     its board's)
#   --dry-run         print the esptool commands without running them
#
# ESPTOOL overrides the esptool command (default: `esptool`). Offsets come from
# scripts/release/offsets.py, which needs python3 (as esptool does).
#
# A bundle built for more flash than the connected chip has is refused.
#
# An ESP-IDF bundle (a `tedge-onboard` image: flash.json says
# "framework": "esp-idf") is flashed the same way: everything, or with
# --app-only the application into ota_0; --erase-storage erases its NVS
# partition (the Wi-Fi network and the Cumulocity certificate).
#
# Local builds are signed with MCUboot's public development key: fine for
# development, never for production devices. A bundle's flash.json says which
# key signed it.
set -euo pipefail

usage() { sed -n '4,29p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

[[ $# -ge 1 ]] || usage 1
build=$1; shift
port=() ; erase_storage=0 ; erase_all=0 ; app_only=0 ; dry=0 ; before=""
while [[ $# -gt 0 ]]; do
	case $1 in
	--port) port=(--port "$2"); shift 2 ;;
	--erase-storage) erase_storage=1; shift ;;
	--erase-all) erase_all=1; shift ;;
	--app-only) app_only=1; shift ;;
	--before) before=$2; shift 2 ;;
	--dry-run) dry=1; shift ;;
	-h|--help) usage 0 ;;
	*) echo "unknown option: $1" >&2; usage 1 ;;
	esac
done
esptool=${ESPTOOL:-esptool}

run() {
	echo "+ $*"
	[[ $dry -eq 1 ]] || "$@"
}

if [[ -f $build/flash.json ]] && grep -q '"framework": "esp-idf"' "$build/flash.json"; then
	vars=$(python3 - "$build" <<'PY'
import json, shlex, sys
from pathlib import Path
d = Path(sys.argv[1])
f = json.loads((d / "flash.json").read_text())
imgs = [(i["offset"], str(d / i["file"])) for i in f["images"]]
app = next(i for i in f["images"] if i["name"] == "app")
nvs = f["partitions"].get("nvs", {})
for k, v in {"CHIP": f["chip"], "ESPTOOL_BEFORE": f["esptool_before"],
             "FLASH_ARGS": " ".join(f["flash_args"]),
             "ALL_IMAGES": " ".join(f"{o} {shlex.quote(p)}" for o, p in imgs),
             "APP_IMAGE": f"{app['offset']} {shlex.quote(str(d / app['file']))}",
             "NVS_OFF": nvs.get("offset", ""), "NVS_SIZE": nvs.get("size", "")}.items():
    print(f"{k}={shlex.quote(v)}")
PY
	) || exit 1
	eval "$vars"
	before=${before:-$ESPTOOL_BEFORE}
	tool=("$esptool" --chip "$CHIP" "${port[@]}" --before "$before")
	if [[ $erase_all -eq 1 ]]; then
		run "${tool[@]}" --after no-reset erase-flash
	elif [[ $erase_storage -eq 1 && -n $NVS_OFF ]]; then
		run "${tool[@]}" --after no-reset erase-region "$NVS_OFF" "$NVS_SIZE"
	fi
	images=$ALL_IMAGES
	[[ $app_only -eq 0 ]] || images=$APP_IMAGE
	# shellcheck disable=SC2086 # offset/file pairs, quoted by the parser above
	eval "run \"\${tool[@]}\" --after hard-reset write-flash $FLASH_ARGS $images"
	exit 0
fi

vars=$(python3 "$(dirname "$0")/release/offsets.py" "$build" --shell) || exit 1
eval "$vars"
before=${before:-${ESPTOOL_BEFORE:-default-reset}}

tool=("$esptool" --chip "$CHIP" "${port[@]}" --before "$before")

# A bundle states the flash size it was laid out for; a smaller chip would
# put the provisioner, storage or the scratch area past its end.
if [[ -n ${FLASH_SIZE:-} ]]; then
	if [[ $dry -eq 1 ]]; then
		echo "# would check the chip has at least $FLASH_SIZE of flash"
	else
		detected=$("${tool[@]}" --after no-reset flash-id 2>&1 |
			sed -n 's/.*[Dd]etected flash size: *\([0-9]*MB\).*/\1/p' | head -1)
		if [[ -z $detected ]]; then
			echo "could not read the flash size from the chip" >&2
			exit 1
		fi
		if (( ${detected%MB} < ${FLASH_SIZE%MB} )); then
			echo "this bundle is laid out for $FLASH_SIZE of flash but the" \
			     "chip has $detected: it is for a different board" >&2
			exit 1
		fi
	fi
fi

if [[ $erase_all -eq 1 ]]; then
	run "${tool[@]}" --after no-reset erase-flash
elif [[ $erase_storage -eq 1 ]]; then
	run "${tool[@]}" --after no-reset erase-region "$STORAGE_PART_OFF" "$STORAGE_PART_SIZE"
	run "${tool[@]}" --after no-reset erase-region "$BOOTREQ_PART_OFF" "$BOOTREQ_PART_SIZE"
fi

images=("$APP_OFF" "$APP_FILE")
if [[ $app_only -eq 0 ]]; then
	images=("$MCUBOOT_OFF" "$MCUBOOT_FILE" "${images[@]}"
		"$PROVISIONER_OFF" "$PROVISIONER_FILE")
fi
run "${tool[@]}" --after hard-reset write-flash "${images[@]}"
