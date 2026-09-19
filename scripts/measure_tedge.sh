#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Footprint of the device-management client (c8y-direct-spikes, task 8.1).
# Builds apps/c8y-spike with --sysbuild, adding one spike feature at a time,
# and reports for each step:
#   image_B    the signed application image, and its share of slot0. The
#              image grows in steps when a segment crosses an MMU page
#              boundary, so compare features by text_B instead
#   text_B     code + read-only data (the feature's real flash cost)
#   libc_B     the libc heap left for the application (every static RAM cost
#              shows up as a drop here; open62541 and friends allocate from it)
#   tlsheap_B  the static mbedTLS heap in that build (the spike sizes it
#              generously for measuring; subtract it and use the measured
#              TLS peaks from design.md for a production estimate)
# plus the delta of each step against the previous one.
#
# Runs inside the zephyr-dev container:
#   docker exec zephyr-dev bash -lc 'cd /ws/app && scripts/measure_tedge.sh'
# BOARDS overrides the boards. TLS_HEAP (bytes) and TLS_RECORD (bytes)
# override CONFIG_MBEDTLS_HEAP_SIZE / CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN for
# every step, to see what fits with production-like sizes instead of the
# spike's 96 KB measuring heap. EXTRA_BASE (';'-separated overlays) is added
# to every step, e.g. apps/c8y-spike/overlay-psram-s3.conf. Needs overlay-wifi-credentials.conf and
# c8y-spike.local.conf at the repo root (git-ignored).
set -u
export ZEPHYR_SDK_INSTALL_DIR=${ZEPHYR_SDK_INSTALL_DIR:-/opt/toolchains/zephyr-sdk-1.0.1}
BOARDS=${BOARDS:-"esp32c6_devkitc/esp32c6/hpcore esp32s3_devkitc/esp32s3/procpu esp32_devkitc/esp32/procpu"}
OUT=${OUT:-build_measure_tedge}
A=/ws/app/apps/c8y-spike
mkdir -p "$OUT"
BASE="/ws/app/overlay-wifi-credentials.conf${EXTRA_BASE:+;$EXTRA_BASE}"

# step name : overlays added on top of the previous step
STEPS=(
	"base:"
	"+A tls-mqtt:$A/overlay-spike-a.conf;/ws/app/c8y-spike.local.conf"
	"+B ota:$A/overlay-spike-b.conf"
	"+C enroll:$A/overlay-spike-c.conf"
	"+F remote:$A/overlay-spike-f.conf"
)

size_for() { echo "$(nm_for "$1" | sed 's/-nm$/-size/')"; }

nm_for() {
	case "$1" in
	esp32c6*) echo "$ZEPHYR_SDK_INSTALL_DIR/gnu/riscv64-zephyr-elf/bin/riscv64-zephyr-elf-nm" ;;
	esp32s3*) echo "$ZEPHYR_SDK_INSTALL_DIR/gnu/xtensa-espressif_esp32s3_zephyr-elf/bin/xtensa-espressif_esp32s3_zephyr-elf-nm" ;;
	*) echo "$ZEPHYR_SDK_INSTALL_DIR/gnu/xtensa-espressif_esp32_zephyr-elf/bin/xtensa-espressif_esp32_zephyr-elf-nm" ;;
	esac
}

part_size() {
	awk -v lbl="$2_partition:" '
		$1 == lbl { found = 1 }
		found && /reg = </ { gsub(/[<>;]/, ""); print strtonum($4); exit }' \
		"$1/zephyr/zephyr.dts"
}

row() { printf "| %-30s | %-12s | %9s | %5s | %9s | %9s | %9s | %9s | %9s |\n" "$@"; }

echo "| board | step | image_B | slot | text_B | d_text | libc_B | d_libc | tlsheap_B |"
echo "|---|---|---|---|---|---|---|---|---|"
for b in $BOARDS; do
	overlays="$BASE"
	prev_img=""; prev_libc=""; prev_text=""
	for s in "${STEPS[@]}"; do
		name=${s%%:*}; add=${s#*:}
		[ -n "$add" ] && overlays="$overlays;$add"
		d="$OUT/$(echo "$b" | tr / _)_$(echo "$name" | tr -c 'A-Za-z0-9\n' '_')"
		extra=()
		[ -n "${TLS_HEAP:-}" ] && [ -n "$add$prev_img" ] && [ "$name" != base ] && \
			extra+=("-Dc8y-spike_CONFIG_MBEDTLS_HEAP_SIZE=$TLS_HEAP")
		[ -n "${TLS_RECORD:-}" ] && [ "$name" != base ] && \
			extra+=("-Dc8y-spike_CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN=$TLS_RECORD")
		if ! west build --sysbuild -b "$b" apps/c8y-spike -d "$d" --pristine \
			-- -DEXTRA_CONF_FILE="$overlays" "${extra[@]}" >"$d.log" 2>&1; then
			reason=$(grep -aoE "region \`[a-z0-9_]+' overflowed by [0-9]+ bytes" "$d.log" | head -1)
			row "$b" "$name" FAIL "" "" "" "${reason:-see $d.log}" "" ""
			continue
		fi
		app="$d/c8y-spike"
		img=$(stat -c %s "$app/zephyr/zephyr.signed.bin")
		slot=$(part_size "$app" slot0)
		libc=$($(nm_for "$b") "$app/zephyr/zephyr.elf" | awk '$3=="_libc_heap_size"{print strtonum("0x"$1)}')
		tls=$(awk -F= '$1=="CONFIG_MBEDTLS_HEAP_SIZE"{print $2}' "$app/zephyr/.config")
		text=$($(size_for "$b") "$app/zephyr/zephyr.elf" | awk 'NR==2{print $1}')
		dt=""; dl=""
		[ -n "$prev_text" ] && dt=$(printf "%+d" $((text - prev_text)))
		[ -n "$prev_libc" ] && [ -n "$libc" ] && dl=$(printf "%+d" $((libc - prev_libc)))
		row "$b" "$name" "$img" "$((img * 100 / slot))%" "$text" "$dt" "${libc:-?}" "$dl" "${tls:-0}"
		prev_img=$img; prev_libc=$libc; prev_text=$text
	done
done
