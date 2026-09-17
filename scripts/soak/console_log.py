#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Timestamped serial console logger for soak runs.

Opens the board's serial port exactly once, at the start of the run, and never
re-opens it. On the ESP32-CAM (USB-serial bridge without auto-reset gating)
opening the port *is* the reset, so the whole boot log is captured. Each line is
stamped with host wall-clock time (the same clock poll.py uses), ANSI colour
codes are stripped, and output is flushed line by line so nothing is lost if
the logger is killed.

    console_log.py --port /dev/cu.usbserial-210 --out runs/x.console.log \
                   [--duration 3600] [--baud 115200]

Stops after --duration seconds (0 = until killed / SIGTERM).
"""
import argparse
import datetime
import re
import signal
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial missing: use the /tmp/flashenv Python (see README)")

ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


def stamp():
    return datetime.datetime.now().isoformat(timespec="milliseconds")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--duration", type=float, default=0,
                    help="seconds to log (0 = until terminated)")
    args = ap.parse_args()

    stop = False

    def on_signal(signum, frame):
        nonlocal stop
        stop = True

    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)

    port = serial.Serial()
    port.port = args.port
    port.baudrate = args.baud
    port.timeout = 0.5
    # Keep the lines de-asserted; on boards whose bridge resets on open anyway
    # this is harmless, on auto-reset DevKitC boards it avoids a bootloader entry.
    port.dtr = False
    port.rts = False

    with open(args.out, "a", buffering=1, encoding="utf-8") as out:
        out.write(f"{stamp()} ##### HOST: console open {args.port} @ {args.baud}\n")
        port.open()
        t_end = time.monotonic() + args.duration if args.duration > 0 else None
        pending = ""
        try:
            while not stop and (t_end is None or time.monotonic() < t_end):
                chunk = port.read(4096)
                if not chunk:
                    continue
                pending += ANSI.sub("", chunk.decode("utf-8", "replace"))
                *lines, pending = pending.split("\n")
                now = stamp()
                for line in lines:
                    out.write(f"{now} {line.rstrip()}\n")
        finally:
            if pending.strip():
                out.write(f"{stamp()} {pending.rstrip()}\n")
            out.write(f"{stamp()} ##### HOST: console closed\n")
            port.close()


if __name__ == "__main__":
    main()
