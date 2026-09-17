#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Build one app in the zephyr-dev container (see README "Building").
#   scripts/soak/build.sh <build-dir> <app> <board> [extra.conf ...]
# Extra conf paths are relative to the repo root; the git-ignored Wi-Fi
# credentials overlay is always added first.
set -euo pipefail
dir="$1" app="$2" board="$3"; shift 3
confs="/ws/app/overlay-wifi-credentials.conf"
for c in "$@"; do confs="$confs;/ws/app/$c"; done
docker exec zephyr-dev bash -lc "export ZEPHYR_SDK_INSTALL_DIR=/opt/toolchains/zephyr-sdk-1.0.1; \
  cd /ws/app && west build -b $board -d $dir apps/$app --pristine -- \
  -DEXTRA_CONF_FILE='$confs' 2>&1 | grep -E 'warning|error|Memory region|FLASH|RAM|iram|dram|irom|drom|^\\[[0-9]+/[0-9]+\\] Linking' | grep -v '^--' ; exit \${PIPESTATUS[0]}"
