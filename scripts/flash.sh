#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Flash a `west build --sysbuild` build of a BLE-provisioning board: MCUboot,
# the signed application into slot0 and the signed Wi-Fi provisioner into
# prov_partition, each at the offset its build was linked for. Runs on the host
# (esptool needs the USB port; Docker on macOS cannot reach it).
#
#   scripts/flash.sh <build-dir> [options]
#
# Options:
#   --port PORT       serial port (default: esptool auto-detects)
#   --erase-storage   also erase storage (credentials, identity) and bootreq
#   --erase-all       erase the whole flash first (first flash over simple boot)
#   --app-only        write only the application (slot0)
#   --before MODE     esptool reset mode (default-reset; usb-reset for some
#                     native-USB boards, e.g. the QT Py S3)
#   --dry-run         print the esptool commands without running them
#
# ESPTOOL overrides the esptool command (default: `esptool`).
#
# The images are signed with MCUboot's public development key: fine for
# development, never for production devices.
set -euo pipefail

usage() { sed -n '4,22p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

[[ $# -ge 1 ]] || usage 1
build=$1; shift
port=() ; erase_storage=0 ; erase_all=0 ; app_only=0 ; dry=0 ; before=default-reset
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

[[ -d $build/mcuboot && -d $build/wifi-provisioner ]] || {
	echo "$build is not a sysbuild build with MCUboot and the provisioner" \
	     "(build with: west build --sysbuild ...)" >&2
	exit 1
}

# The application image: the one domain that is neither MCUboot nor the
# provisioner.
app=""
for d in "$build"/*/; do
	d=${d%/}; n=${d##*/}
	[[ $n == mcuboot || $n == wifi-provisioner ]] && continue
	[[ -f $d/zephyr/zephyr.signed.bin ]] && app=$d
done
[[ -n $app ]] || { echo "no signed application image in $build" >&2; exit 1; }

dts=$app/zephyr/zephyr.dts
soc=$(sed -n 's/^CONFIG_SOC="\(.*\)"/\1/p' "$app/zephyr/.config")

# Offset and size of a partition by node label, from the app's devicetree.
part() {
	awk -v lbl="$1_partition:" '
		$1 == lbl { found = 1 }
		found && /reg = </ {
			gsub(/[<>;]/, ""); print $3, $4; exit
		}' "$dts"
}
read -r boot_off _ < <(part boot)
read -r slot0_off _ < <(part slot0)
read -r prov_off _ < <(part prov)
read -r bootreq_off bootreq_size < <(part bootreq)
read -r storage_off storage_size < <(part storage)
# The classic ESP32 ROM loads the second-stage bootloader from 0x1000.
[[ $soc == esp32 ]] && boot_off=0x1000

run() {
	echo "+ $*"
	[[ $dry -eq 1 ]] || "$@"
}
tool=("$esptool" --chip "$soc" "${port[@]}" --before "$before")

if [[ $erase_all -eq 1 ]]; then
	run "${tool[@]}" --after no-reset erase-flash
elif [[ $erase_storage -eq 1 ]]; then
	run "${tool[@]}" --after no-reset erase-region "$storage_off" "$storage_size"
	run "${tool[@]}" --after no-reset erase-region "$bootreq_off" "$bootreq_size"
fi

images=("$slot0_off" "$app/zephyr/zephyr.signed.bin")
if [[ $app_only -eq 0 ]]; then
	images=("$boot_off" "$build/mcuboot/zephyr/zephyr.bin" "${images[@]}"
		"$prov_off" "$build/wifi-provisioner/zephyr/zephyr.signed.bin")
fi
run "${tool[@]}" --after hard-reset write-flash "${images[@]}"
