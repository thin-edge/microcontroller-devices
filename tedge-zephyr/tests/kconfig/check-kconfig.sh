#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Check the tedge-zephyr Kconfig dependency rules.
#
# Zephyr treats an assignment that a dependency overrides ("was assigned the
# value 'y' but got the value 'n'") as a warning, not an error, so these rules
# can't fail a build on their own. This script configures samples/minimal once
# per case in cases/*.conf and checks each case's expectations, written as
# comments in the case file:
#
#   # expect: CONFIG_TEDGE_X=y         resulting value (n = unset or absent)
#   # expect-warning: <regex>          a Kconfig warning matching the regex
#
# Run it from a Zephyr workspace (west on PATH, ZEPHYR_BASE set or found by
# west). Usage: check-kconfig.sh [build-root] [board]

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
module="$(cd "$here/../.." && pwd)"
build_root="${1:-$PWD/build-tedge-kconfig}"
board="${2:-native_sim/native/64}"

pass=0
fail=0

for case_file in "$here"/cases/*.conf; do
	name="$(basename "$case_file" .conf)"
	build_dir="$build_root/$name"
	log="$build_dir.log"
	mkdir -p "$build_root"

	if ! west build -b "$board" "$module/samples/minimal" --pristine \
		-d "$build_dir" --cmake-only -- \
		-DEXTRA_CONF_FILE="$case_file" >"$log" 2>&1; then
		echo "FAIL $name: configure failed (see $log)"
		fail=$((fail + 1))
		continue
	fi

	config="$build_dir/zephyr/.config"
	# Kconfig wraps its warnings over several lines; match them joined.
	output="$(tr '\n' ' ' <"$log")"
	case_ok=1

	while IFS= read -r line; do
		case "$line" in
		"# expect: "*)
			expect="${line#\# expect: }"
			sym="${expect%%=*}"
			want="${expect#*=}"
			got="$(grep -E "^${sym}=" "$config" | head -n1 | cut -d= -f2- || true)"
			[ -z "$got" ] && got=n
			if [ "$got" != "$want" ]; then
				echo "FAIL $name: $sym is '$got', expected '$want'"
				case_ok=0
			fi
			;;
		"# expect-warning: "*)
			regex="${line#\# expect-warning: }"
			if ! grep -qE "$regex" <<<"$output"; then
				echo "FAIL $name: no Kconfig warning matching '$regex'"
				case_ok=0
			fi
			;;
		esac
	done <"$case_file"

	if [ "$case_ok" = 1 ]; then
		echo "ok   $name"
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
	fi
done

echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
