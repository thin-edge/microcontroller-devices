#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Size check for BLE provisioning: build every app with --sysbuild on every
# board that has a provisioner layout, and report each signed image against its
# partition, plus the libc heap left for the application's frontend (the "libc
# heap size" printed at boot; open62541 allocates from it). Runs inside the
# zephyr-dev container:
#
#   docker exec zephyr-dev bash -lc 'cd /ws/app && scripts/measure_prov.sh'
set -u
export ZEPHYR_SDK_INSTALL_DIR=${ZEPHYR_SDK_INSTALL_DIR:-/opt/toolchains/zephyr-sdk-1.0.1}
BOARDS=${BOARDS:-"esp32s3_devkitc/esp32s3/procpu esp32c6_devkitc/esp32c6/hpcore"}
APPS=${APPS:-"opcua-server modbus-server snmp-agent"}
OUT=${OUT:-build_measure}

nm_for() {
	case "$1" in
	esp32c6*) echo "$ZEPHYR_SDK_INSTALL_DIR/gnu/riscv64-zephyr-elf/bin/riscv64-zephyr-elf-nm" ;;
	esp32s3*|adafruit_qt_py_esp32s3*) echo "$ZEPHYR_SDK_INSTALL_DIR/gnu/xtensa-espressif_esp32s3_zephyr-elf/bin/xtensa-espressif_esp32s3_zephyr-elf-nm" ;;
	*) echo "$ZEPHYR_SDK_INSTALL_DIR/gnu/xtensa-espressif_esp32_zephyr-elf/bin/xtensa-espressif_esp32_zephyr-elf-nm" ;;
	esac
}

# Size of a partition (by node label) from an image's devicetree.
part_size() {
	awk -v lbl="$2_partition:" '
		$1 == lbl { found = 1 }
		found && /reg = </ { gsub(/[<>;]/, ""); print strtonum($4); exit }' \
		"$1/zephyr/zephyr.dts"
}

row() { printf "%-32s %-16s %10s %10s %5s %10s\n" "$@"; }

row board image image_B part_B used heap_B
for b in $BOARDS; do
	for a in $APPS; do
		d="$OUT/$(echo "$b" | tr / _)_$a"
		if ! west build --sysbuild -b "$b" "apps/$a" -d "$d" --pristine >"$d.log" 2>&1; then
			row "$b" "$a" FAIL "(see $d.log)" "" ""
			continue
		fi
		for img in "$a:slot0" "wifi-provisioner:prov"; do
			n=${img%%:*}; p=${img##*:}
			# The provisioner is the same image for every app: report it once.
			[ "$n" = wifi-provisioner ] && [ "$a" != "${APPS%% *}" ] && continue
			size=$(stat -c %s "$d/$n/zephyr/zephyr.signed.bin")
			psize=$(part_size "$d/$n" "$p")
			heap=$($(nm_for "$b") "$d/$n/zephyr/zephyr.elf" | awk '$3=="_libc_heap_size"{print strtonum("0x"$1)}')
			row "$b" "$n" "$size" "$psize" "$((size * 100 / psize))%" "$heap"
		done
	done
done
