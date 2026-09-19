#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Provision Wi-Fi on a device in BLE provisioning mode (Improv Wi-Fi over BLE).

A headless alternative to the Improv web page, for the bench and for tests.
Needs `bleak` (pip install bleak); works on macOS and Linux.

  # list devices advertising Improv
  improv_provision.py scan

  # send credentials (the password is read from the environment, not argv)
  IMPROV_PSK=secret improv_provision.py provision --ssid my-network
  improv_provision.py provision --ssid my-network --name tedge-modbus

  # blink the device's LED; send a deliberately corrupt frame
  improv_provision.py identify
  improv_provision.py raw 01 02 03 04

Exit status: 0 on success, 1 when the device reports an error, 2 on a
transport problem (not found, timeout).
"""
import argparse
import asyncio
import os
import sys

from bleak import BleakClient, BleakScanner

BASE = "00467768-6228-2272-4663-2774782680{:02x}"
SVC = BASE.format(0x00)
CH_STATE = BASE.format(0x01)
CH_ERROR = BASE.format(0x02)
CH_RPC = BASE.format(0x03)
CH_RESULT = BASE.format(0x04)
CH_CAPS = BASE.format(0x05)

STATES = {1: "authorization required", 2: "authorized", 3: "provisioning",
          4: "provisioned"}
ERRORS = {0: "none", 1: "invalid RPC", 2: "unknown RPC",
          3: "unable to connect", 4: "not authorized", 0xFF: "unknown"}


def frame(cmd: int, payload: bytes) -> bytes:
    body = bytes([cmd, len(payload)]) + payload
    return body + bytes([sum(body) & 0xFF])


def wifi_settings(ssid: str, psk: str) -> bytes:
    s, p = ssid.encode(), psk.encode()
    return frame(0x01, bytes([len(s)]) + s + bytes([len(p)]) + p)


def parse_result(data: bytes) -> list[str]:
    out, i = [], 2
    while i < len(data) - 1:
        n = data[i]
        out.append(data[i + 1:i + 1 + n].decode(errors="replace"))
        i += 1 + n
    return out


async def find(name: str | None, timeout: float):
    devices = await BleakScanner.discover(timeout=timeout, return_adv=True,
                                          service_uuids=[SVC])
    found = [(d, adv) for d, adv in devices.values()
             if SVC in [u.lower() for u in adv.service_uuids]
             # Prefer the advertised name: macOS caches names per address,
             # so d.name can be stale after the device is reflashed.
             and (name is None or (adv.local_name or d.name or "").startswith(name))]
    return found


async def cmd_scan(args) -> int:
    found = await find(args.name, args.timeout)
    for d, adv in found:
        sd = adv.service_data.get("00004677-0000-1000-8000-00805f9b34fb", b"")
        state = STATES.get(sd[0], "?") if sd else "?"
        print(f"{d.address}  {adv.local_name or d.name}  rssi={adv.rssi}  state={state}")
    return 0 if found else 2


async def session(args, payload: bytes | None, wait_for_final: bool) -> int:
    found = await find(args.name, args.timeout)
    if not found:
        print("no Improv device found", file=sys.stderr)
        return 2
    dev, adv = found[0]
    print(f"connecting to {adv.local_name or dev.name} ({dev.address})")

    events: asyncio.Queue = asyncio.Queue()
    async with BleakClient(dev, timeout=args.timeout) as client:
        await client.start_notify(CH_STATE, lambda _c, d: events.put_nowait(("state", d[0])))
        await client.start_notify(CH_ERROR, lambda _c, d: events.put_nowait(("error", d[0])))
        await client.start_notify(CH_RESULT, lambda _c, d: events.put_nowait(("result", bytes(d))))
        state = (await client.read_gatt_char(CH_STATE))[0]
        caps = (await client.read_gatt_char(CH_CAPS))[0]
        print(f"state: {STATES.get(state, state)}, capabilities: 0x{caps:02x}")
        if payload is None:
            return 0
        await client.write_gatt_char(CH_RPC, payload, response=True)
        if not wait_for_final:
            await asyncio.sleep(1.0)
            err = (await client.read_gatt_char(CH_ERROR))[0]
            print(f"error: {ERRORS.get(err, err)}")
            return 0 if err == 0 else 1

        loop = asyncio.get_running_loop()
        deadline = loop.time() + args.connect_timeout
        rc = 2
        while loop.time() < deadline:
            try:
                kind, val = await asyncio.wait_for(events.get(), deadline - loop.time())
            except asyncio.TimeoutError:
                break
            if kind == "state":
                print(f"state: {STATES.get(val, val)}")
                if val == 4:
                    rc = 0
            elif kind == "error":
                print(f"error: {ERRORS.get(val, val)}")
                if val != 0:
                    return 1
            elif kind == "result":
                for url in parse_result(val):
                    print(f"device URL: {url}")
                if rc == 0:
                    return 0
        if rc == 0:
            return 0
        print("timed out waiting for the device", file=sys.stderr)
        return 2


async def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--name", help="only devices whose name starts with this")
    ap.add_argument("--timeout", type=float, default=10.0, help="scan/connect timeout (s)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("scan")
    sub.add_parser("state")
    sub.add_parser("identify")
    p = sub.add_parser("provision")
    p.add_argument("--ssid", required=True)
    p.add_argument("--psk-env", default="IMPROV_PSK",
                   help="environment variable holding the password (default IMPROV_PSK)")
    p.add_argument("--connect-timeout", type=float, default=60.0)
    r = sub.add_parser("raw", help="write raw hex bytes to the RPC characteristic")
    r.add_argument("bytes", nargs="+")
    args = ap.parse_args()

    if args.cmd == "scan":
        return await cmd_scan(args)
    if args.cmd == "state":
        return await session(args, None, False)
    if args.cmd == "identify":
        return await session(args, frame(0x02, b""), False)
    if args.cmd == "raw":
        return await session(args, bytes(int(b, 16) for b in args.bytes), False)
    psk = os.environ.get(args.psk_env, "")
    return await session(args, wifi_settings(args.ssid, psk), True)


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
