#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Capture a board's console with host timestamps.

Opening the ESP32-C6's USB-Serial-JTAG port resets the board, so a capture
starts at boot. Lines are written to stdout and to --log, prefixed with the
seconds since the capture started.

Usage: console.py PORT [--seconds N] [--until REGEX] [--log FILE]
"""

import argparse
import re
import sys
import time

import serial


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("--seconds", type=float, default=60)
    ap.add_argument("--until", help="stop after a line matching this regex")
    ap.add_argument("--log")
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()

    until = re.compile(args.until) if args.until else None
    log = open(args.log, "w") if args.log else None
    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = args.baud
    ser.timeout = 0.2
    ser.dtr = False
    ser.rts = False
    ser.open()

    t0 = time.monotonic()
    buf = b""
    try:
        while time.monotonic() - t0 < args.seconds:
            try:
                buf += ser.read(4096)
            except serial.SerialException:
                # The C6's port re-enumerates when it resets.
                ser.close()
                time.sleep(0.5)
                try:
                    ser.open()
                except serial.SerialException:
                    pass
                continue
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = re.sub(r"\x1b\[[0-9;]*m", "",
                              raw.decode(errors="replace").rstrip("\r"))
                stamped = f"[{time.monotonic() - t0:7.2f}] {line}"
                print(stamped, flush=True)
                if log:
                    log.write(stamped + "\n")
                    log.flush()
                if until and until.search(line):
                    return 0
    finally:
        ser.close()
        if log:
            log.close()
    return 0 if not until else 1


if __name__ == "__main__":
    sys.exit(main())
