#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Run the soak harness on a remote Linux host (e.g. a Raspberry Pi) that has the
# boards on its USB ports. Builds still happen locally in the zephyr-dev
# container; this pushes the image, flashes it there and runs the harness
# detached, so a run survives the laptop going away.
#
#   SOAK_HOST=root@rpi5-d83add9f145a.local scripts/soak/remote.sh <command> ...
#
# Commands:
#   setup                       create /opt/soakenv (esptool, pyserial), copy scripts
#   flash <build-dir> <port> [esp32|esp32s3|esp32c6]
#                               copy <build-dir>/zephyr/zephyr.bin and flash it
#                               (esp32: 0x1000, auto-reset;
#                                esp32s3/esp32c6: 0x0, usb-reset)
#   run <run.sh args...>        start run.sh detached; prints the run stem
#   status                      running soaks and the last poll line of each
#   stop <stem>                 stop a running soak
#   pull                        copy remote run records to scripts/soak/runs/
#
# Ports: use /dev/serial/by-path/... so a board keeps its name across reboots.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
host="${SOAK_HOST:?set SOAK_HOST=user@host}"
rdir="${SOAK_REMOTE_DIR:-/opt/soak}"
renv="${SOAK_REMOTE_ENV:-/opt/soakenv}"

push_scripts() {
    ssh "$host" "mkdir -p $rdir/scripts $rdir/runs $rdir/fw"
    scp -q "$here/run.sh" "$here/poll.py" "$here/console_log.py" "$host:$rdir/scripts/"
}

cmd="${1:-}"; shift || true
case "$cmd" in
setup)
    ssh "$host" "python3 -m venv $renv && $renv/bin/pip install -q esptool pyserial"
    push_scripts
    ;;
flash)
    dir="$1" port="$2" chip="${3:-esp32}"
    name="$(basename "$dir")"
    ssh "$host" "mkdir -p $rdir/fw/$name"
    scp -q "$repo/$dir/zephyr/zephyr.bin" "$host:$rdir/fw/$name/zephyr.bin"
    case "$chip" in
    esp32s3|esp32c6)
        # Native-USB parts (USB-Serial-JTAG): image at 0x0, reset over USB.
        # Applies to the QT Py S3, the ESP32-S3-DevKitC and the ESP32-C6, whose
        # board devicetrees all include a partitions_0x0_* layout.
        opts="--chip $chip --before usb-reset --after hard-reset"; off=0x0
        ;;
    *)
        opts="--chip esp32"; off=0x1000
        ;;
    esac
    ssh "$host" "$renv/bin/python -m esptool -p $port -b 460800 $opts \
        write-flash $off $rdir/fw/$name/zephyr.bin" | tail -2
    ;;
run)
    push_scripts
    sha="$(git -C "$repo" rev-parse --short HEAD)"
    git -C "$repo" diff --quiet HEAD -- lib apps || sha="${sha}-dirty"
    args=""
    for a in "$@"; do args+=" $(printf '%q' "$a")"; done
    tag="$(date +%s)"
    ssh "$host" "cd $rdir && SOAK_PYTHON=$renv/bin/python nohup scripts/run.sh \
        --out-dir $rdir/runs --sha $sha $args > runs/nohup-$tag.out 2>&1 < /dev/null & \
        sleep 3; head -1 runs/nohup-$tag.out"
    ;;
status)
    ssh "$host" "pgrep -af '[s]cripts/run.sh' | sed 's/^/  /'; \
        for f in \$(ls -t $rdir/runs/*.polls.log 2>/dev/null | head -8); do \
            echo \"\$(basename \$f .polls.log): \$(grep -c '^.* OK ' \$f) ok, \
\$(grep -c 'HOST: outage (' \$f) outages; last: \$(tail -1 \$f | cut -c1-90)\"; done"
    ;;
stop)
    stem="$1"
    # Bracketed first characters keep pkill from matching this ssh command.
    ssh "$host" "pkill -f -- '[-]-out $rdir/runs/$stem' || true; sleep 2; \
        pkill -f '[c]onsole_log.py .*$stem' || true"
    ;;
pull)
    mkdir -p "$here/runs"
    rsync -a "$host:$rdir/runs/" "$here/runs/"
    ;;
*)
    sed -n '3,22p' "$0"; exit 2
    ;;
esac
