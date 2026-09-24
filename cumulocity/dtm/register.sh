#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Register the tedge-zephyr parameter sets in a tenant's Digital Twin
# Manager, so the device's Parameters tab can show and edit them.
#
#   cumulocity/dtm/register.sh [--replace] [set...]   # default: every *.json here
#
# Needs a go-c8y-cli session, the dtm and device-parameter microservices, and
# ROLE_DIGITAL_TWIN_DEFINITIONS_CREATE/_ADMIN. An identifier that already
# exists is left alone; --replace deletes and re-registers it (DTM cannot
# update one in place). The files were printed by the firmware itself
# (`tedge params schema <set>`, CONFIG_TEDGE_PARAMETERS_SCHEMA), so they match
# what the devices validate against; regenerate them when a declaration changes.
set -euo pipefail
cd "$(dirname "$0")"
replace=0
failed=0
[[ ${1:-} == --replace ]] && { replace=1; shift; }
sets=("$@")
[[ ${#sets[@]} -gt 0 ]] || sets=($(ls *.json | sed 's/\.json$//'))
# The listing is paged whatever pageSize asks for (a tenant with ~20
# definitions returned them over several pages), so read every page: a check
# that saw only the first made --replace skip the delete and the POST fail.
exists() {
	local page=1 found
	while :; do
		found=$(c8y api GET "/service/dtm/definitions/properties?pageSize=100&currentPage=$page" --raw < /dev/null |
			python3 -c 'import json,sys; d=json.load(sys.stdin); d=d.get("definitions", []) if isinstance(d, dict) else d; print("yes" if any(i.get("identifier")==sys.argv[1] for i in d) else ("end" if not d else "no"))' "$1")
		case $found in
		yes) return 0 ;;
		end) return 1 ;;
		esac
		page=$((page + 1))
	done
}
for s in "${sets[@]}"; do
	if exists "$s"; then
		if [[ $replace -eq 0 ]]; then
			echo "exists: $s (use --replace to re-register)"
			continue
		fi
		c8y api DELETE "/service/dtm/definitions/properties/$s?contexts=asset,event,operation" --force < /dev/null >/dev/null
		echo "deleted: $s"
	fi
	if out=$(c8y api POST /service/dtm/definitions/properties --data "$(cat "$s.json")" --force --raw < /dev/null 2>&1); then
		echo "registered: $s"
	else
		# DTM refuses a duplicate identifier *or* title (HTTP 409).
		echo "failed: $s: $out" >&2
		failed=1
	fi
done
exit $failed
