#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Rebuild release builds and record their sizes in release/size-baseline.json.
# Run it inside the Zephyr build container, like scripts/release/build.sh:
#
#   scripts/release/baseline.sh                 # every build in release/devices.yml
#   scripts/release/baseline.sh NAME [NAME...]  # only these builds
#
# Each build goes through scripts/release/build.sh --write-baseline, so the
# entry it writes is exactly what the release workflow later compares
# against. The builds share one build directory (build-release/baseline,
# pristine each time; ccache keeps it quick) so a full run does not need
# gigabytes; their packaged output and size.json land in dist/ as usual. A build that fails to build, or that is over its flash budget
# without a `budget_exceptions` note in the baseline, is reported at the end
# and the exit status is non-zero; the other builds are still recorded.
# Commit the updated baseline with the change that moved the sizes.
set -uo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
if [[ $# -gt 0 ]]; then
	names=("$@")
else
	mapfile -t names < <(python3 - "$root" <<'PY'
import sys
sys.path.insert(0, sys.argv[1] + "/scripts/release")
import matrix
for b in matrix.load()[1]:
    print(b.firmware_name)
PY
	)
fi

failed=()
for name in "${names[@]}"; do
	echo "=== $name"
	if ! "$root/scripts/release/build.sh" "$name" --write-baseline \
		--build-dir "$root/build-release/baseline"; then
		failed+=("$name")
	fi
done
echo
echo "recorded ${#names[@]} builds in $root/release/size-baseline.json"
if [[ ${#failed[@]} -gt 0 ]]; then
	echo "failed (build error, or over budget without a baseline exception):" >&2
	printf '  %s\n' "${failed[@]}" >&2
	exit 1
fi
