#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Drive one soak run: open the serial console (on the ESP32-CAM that resets the
# board, so the run starts at boot), poll the device's protocol endpoint and
# ping, record traps, detect outages and write a summary JSON.
#
#   scripts/soak/run.sh --board esp32-cam --app snmp --host 192.168.68.74 \
#       --port /dev/cu.usbserial-210 --variant baseline --duration 3600
#
# Options:
#   --board NAME        free-form board label (recorded in the summary)
#   --app snmp|modbus|opcua|mqtt   (mqtt: pass -- --mqtt-topic TOPIC, see poll.py)
#   --host IP           device IP (literal)
#   --port DEV          serial port to log (omit for console-less boards)
#   --variant LABEL     config variant label (e.g. baseline, diag, pools-40-32)
#   --duration SECONDS  planned run length (default 3600); extended while an
#                       outage is in progress, up to the recovery window
#   --out-dir DIR       where run files go (default scripts/soak/runs, git-ignored)
#   --liveness-s N      liveness timeout used for the recovery window (default 30)
#   --last-resort-s N   last-resort reboot timeout (default 300)
#   --rejoin-s N        allowance for bring-up after a reset (default 120)
#   --stop-after-outages N   end early once N outages have finished
#   --sha SHA           firmware revision to record (default: this checkout's HEAD)
#   -- ARGS...          passed through to poll.py (e.g. --trap-port 1162)
#
# Output: <out-dir>/<stamp>-<board>-<app>-<variant>.{console,polls,traps}.log
# and .summary.json.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"

board="" app="" host="" port="" variant="baseline" duration=3600 sha=""
out_dir="$here/runs" liveness_s=30 last_resort_s=300 rejoin_s=120 stop_after=0
extra=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --board) board="$2"; shift 2 ;;
        --app) app="$2"; shift 2 ;;
        --host) host="$2"; shift 2 ;;
        --port) port="$2"; shift 2 ;;
        --variant) variant="$2"; shift 2 ;;
        --duration) duration="$2"; shift 2 ;;
        --out-dir) out_dir="$2"; shift 2 ;;
        --liveness-s) liveness_s="$2"; shift 2 ;;
        --last-resort-s) last_resort_s="$2"; shift 2 ;;
        --rejoin-s) rejoin_s="$2"; shift 2 ;;
        --stop-after-outages) stop_after="$2"; shift 2 ;;
        --sha) sha="$2"; shift 2 ;;
        --) shift; extra=("$@"); break ;;
        -h|--help) sed -n '3,30p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

[[ -n "$board" && -n "$app" && -n "$host" ]] || {
    echo "need --board, --app and --host (see --help)" >&2; exit 2; }

# The macOS host venv used for flashing has pyserial.
py="${SOAK_PYTHON:-}"
for cand in /opt/soakenv/bin/python /tmp/flashenv/bin/python "$HOME/flashenv/bin/python" \
    "$(command -v python3)"; do
    [[ -n "$py" ]] && break
    [[ -x "$cand" ]] && py="$cand"
done

mkdir -p "$out_dir"
stamp="$(date +%Y%m%dT%H%M%S)"
prefix="$out_dir/${stamp}-${board}-${app}-${variant}"
if [[ -z "$sha" ]]; then
    sha="$(git -C "$repo" rev-parse --short HEAD 2>/dev/null || echo unknown)"
    git -C "$repo" diff --quiet HEAD -- lib apps 2>/dev/null || sha="${sha}-dirty"
fi
window=$((liveness_s + last_resort_s + rejoin_s))

console_pid=""
cleanup() {
    if [[ -n "$console_pid" ]] && kill -0 "$console_pid" 2>/dev/null; then
        kill "$console_pid" 2>/dev/null || true
        wait "$console_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

console_arg=()
if [[ -n "$port" ]]; then
    "$py" "$here/console_log.py" --port "$port" --out "$prefix.console.log" &
    console_pid=$!
    console_arg=(--console "$prefix.console.log")
    sleep 1   # let the open (and the reset it causes) happen before polling
fi

meta="$(printf '{"board":"%s","sha":"%s","variant":"%s","planned_duration_s":%s,"serial_port":"%s"}' \
    "$board" "$sha" "$variant" "$duration" "$port")"

echo "run: $prefix (sha $sha, recovery window ${window}s)"
"$py" "$here/poll.py" --app "$app" --host "$host" --out "$prefix" \
    --duration "$duration" --recovery-window "$window" \
    --stop-after-outages "$stop_after" --meta "$meta" \
    ${console_arg[@]+"${console_arg[@]}"} ${extra[@]+"${extra[@]}"}

cleanup
console_pid=""
# Re-scan now that the console log is complete.
"$py" - "$prefix.summary.json" "$prefix.console.log" "$here" <<'EOF'
import json, sys
sys.path.insert(0, sys.argv[3])
from poll import scan_console
p = sys.argv[1]
s = json.load(open(p))
s["console"] = scan_console(sys.argv[2])
json.dump(s, open(p, "w"), indent=2)
EOF
echo "summary: $prefix.summary.json"
