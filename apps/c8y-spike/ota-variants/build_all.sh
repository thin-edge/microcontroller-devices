#!/usr/bin/env bash
# Build the Spike B test images A-E (c8y-direct-spikes, section 4). Run from
# the repo root: apps/c8y-spike/ota-variants/build_all.sh [A B C D E]
set -u
mkdir -p build_logs
OV="/ws/app/overlay-wifi-credentials.conf;/ws/app/apps/c8y-spike/overlay-spike-a.conf;/ws/app/c8y-spike.local.conf;/ws/app/apps/c8y-spike/overlay-spike-b.conf;/ws/app/apps/c8y-spike/overlay-github.conf"
for v in ${@:-A B C D E}; do
  docker exec -w /ws/app -e ZEPHYR_SDK_INSTALL_DIR=/opt/toolchains/zephyr-sdk-1.0.1 zephyr-dev \
    west build -b esp32c6_devkitc/esp32c6/hpcore apps/c8y-spike --sysbuild --pristine \
    -d build_spike_b_$v -- -DEXTRA_CONF_FILE="$OV;/ws/app/apps/c8y-spike/ota-variants/$v.conf" \
    > build_logs/spike_b_$v.log 2>&1
  echo "$v exit=$? $(ls -l build_spike_b_$v/c8y-spike/zephyr/zephyr.signed.bin 2>/dev/null | awk '{print $5}')"
done
