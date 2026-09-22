#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Footprint of the tedge-zephyr client: each application built with and
# without CONFIG_TEDGE, so the table shows what the client costs a host
# application. Reports for each build:
#   image_B    the signed application image
#   text_B     code + read-only data
#   libc_B     the libc heap left for the application
#   tlsheap_B  the mbedTLS heap that build configures
#
# Runs inside the zephyr-dev container:
#   docker exec zephyr-dev bash -lc 'cd /ws/app && MODE=module scripts/measure_tedge.sh'
# CASES overrides the builds (see below). Needs overlay-wifi-credentials.conf
# and tedge.local.conf at the repo root (both git-ignored).
set -u
export ZEPHYR_SDK_INSTALL_DIR=${ZEPHYR_SDK_INSTALL_DIR:-/opt/toolchains/zephyr-sdk-1.0.1}
OUT=${OUT:-build_measure_tedge}
mkdir -p "$OUT"
BASE="/ws/app/overlay-wifi-credentials.conf${EXTRA_BASE:+;$EXTRA_BASE}"

size_for() { echo "$(nm_for "$1" | sed 's/-nm$/-size/')"; }

nm_for() {
	case "$1" in
	esp32c6*) echo "$ZEPHYR_SDK_INSTALL_DIR/gnu/riscv64-zephyr-elf/bin/riscv64-zephyr-elf-nm" ;;
	esp32s3*) echo "$ZEPHYR_SDK_INSTALL_DIR/gnu/xtensa-espressif_esp32s3_zephyr-elf/bin/xtensa-espressif_esp32s3_zephyr-elf-nm" ;;
	*) echo "$ZEPHYR_SDK_INSTALL_DIR/gnu/xtensa-espressif_esp32_zephyr-elf/bin/xtensa-espressif_esp32_zephyr-elf-nm" ;;
	esac
}

# --- MODE=module: the client's cost in a host application -------------------
# The client at the minimal profile, with each board's tedge-zephyr settings.
TB=/ws/app/lib/common/tedge-boards
MIN=/ws/app/tedge-zephyr/profiles/minimal.conf
# label : board : app : overlays (on top of the Wi-Fi credentials)
CASES=${CASES:-"
c6 modbus, no client:esp32c6_devkitc/esp32c6/hpcore:apps/modbus-server:
c6 modbus + client:esp32c6_devkitc/esp32c6/hpcore:apps/modbus-server:$MIN;$TB/esp32c6-devkitc.conf;/ws/app/tedge.local.conf
s3 modbus, no client:esp32s3_devkitc/esp32s3/procpu:apps/modbus-server:
s3 modbus + client (PSRAM):esp32s3_devkitc/esp32s3/procpu:apps/modbus-server:$MIN;$TB/esp32s3-devkitc.conf;/ws/app/tedge.local.conf
c6 modbus + client + remote access:esp32c6_devkitc/esp32c6/hpcore:apps/modbus-server:$MIN;$TB/esp32c6-devkitc.conf;/ws/app/tedge.local.conf;/ws/app/build_measure_extra/ra.conf
c6 samples/minimal:esp32c6_devkitc/esp32c6/hpcore:tedge-zephyr/samples/minimal:!/ws/app/tedge-zephyr/samples/minimal/overlay-c8y.conf;/ws/app/tedge.local.conf
"}
echo "| case | image_B | text_B | libc_B | tlsheap_B |"
echo "|---|---|---|---|---|"
prev_text=""; prev_libc=""
echo "$CASES" | while IFS=: read -r label board app add; do
	[ -z "$label" ] && continue
	d="$OUT/mod_$(echo "$label" | tr -c 'A-Za-z0-9\n' '_')"
	# A case whose overlays start with "!" does not take the
	# repository's Wi-Fi overlay (it has its own options).
	case "$add" in
	"!"*) overlays="${add#!}" ;;
	*) overlays="$BASE${add:+;$add}" ;;
	esac
	# Only the applications with a sysbuild.cmake build with MCUboot.
	sb=--sysbuild
	[ -f "/ws/app/$app/sysbuild.cmake" ] || sb=
	if ! west build $sb -b "$board" "$app" -d "$d" --pristine \
		-- -DEXTRA_CONF_FILE="$overlays" >"$d.log" 2>&1; then
		reason=$(grep -aoE "region \`[a-z0-9_]+' overflowed by [0-9]+ bytes" "$d.log" | head -1)
		printf "| %-28s | %s |\n" "$label" "FAIL ${reason:-see $d.log}"
		continue
	fi
	img_dir="$d/$(basename "$app")"
	[ -d "$img_dir" ] || img_dir="$d"
	elf="$img_dir/zephyr/zephyr.elf"
	img=$(stat -c %s "$img_dir/zephyr/zephyr.signed.bin" 2>/dev/null ||
	      stat -c %s "$img_dir/zephyr/zephyr.bin")
	text=$($(size_for "$board") "$elf" | awk 'NR==2{print $1}')
	libc=$($(nm_for "$board") "$elf" | awk '$3=="_libc_heap_size"{print strtonum("0x"$1)}')
	tls=$(awk -F= '$1=="CONFIG_MBEDTLS_HEAP_SIZE"{print $2}' "$img_dir/zephyr/.config")
	printf "| %-28s | %9s | %9s | %9s | %9s |\n" "$label" "$img" "$text" \
		"${libc:-?}" "${tls:-0}"
done
