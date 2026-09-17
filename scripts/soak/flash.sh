#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Flash a build to a classic ESP32 (esp32_devkitc target) from the macOS host.
#   scripts/soak/flash.sh <build-dir> <port>
# For the QT Py S3 use offset 0x0 and --before usb_reset (see README).
set -euo pipefail
dir="$1" port="$2"
py="${SOAK_PYTHON:-}"
for cand in /tmp/flashenv/bin/python "$HOME/flashenv/bin/python"; do
    [[ -n "$py" ]] && break
    [[ -x "$cand" ]] && py="$cand"
done
[[ -n "$py" ]] || { echo "no esptool venv; set SOAK_PYTHON" >&2; exit 2; }
"$py" -m esptool --chip esp32 -p "$port" -b 460800 write_flash 0x1000 "$dir/zephyr/zephyr.bin"
