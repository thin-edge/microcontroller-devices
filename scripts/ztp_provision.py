#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Relay a ZTP enrollment between a device and a lab-ztp-provisioner server.

A headless stand-in for the web or desktop relay, for the bench and for tests,
talking to a device whose provisioner is built with CONFIG_APP_PROV_ZTP. It
does what the stock relays do: write the time, kick the device, forward its
signed envelope to the server, and write the server's response back.
Needs `bleak` (pip install bleak); works on macOS and Linux.

  # list devices advertising the ZTP service
  ztp_provision.py scan

  # enroll the first ZTP device found
  ztp_provision.py enroll --server https://ztp.local:8443 --insecure

  # a specific device, and keep retrying while the server says "pending"
  ztp_provision.py enroll --server https://ztp.local:8443 \\
      --address 7C:0C:5F:5A:6E:BA --wait 600

  # only fetch and print the device's envelope (nothing is sent anywhere)
  ztp_provision.py envelope

Exit status: 0 when the device reports it is provisioned, 1 when the server
or the device refuses, 2 on a transport problem (not found, timeout).
"""
import argparse
import asyncio
import datetime
import json
import ssl
import struct
import sys
import urllib.error
import urllib.request

from bleak import BleakClient, BleakScanner

BASE = "6e4000{:02x}-b5a3-f393-e0a9-e50e24dcca9e"
SVC = BASE.format(0x01)
CH_REQUEST = BASE.format(0x02)
CH_RESPONSE = BASE.format(0x03)
CH_STATUS = BASE.format(0x04)
CH_TIMESYNC = BASE.format(0x05)

STATUS = {0: "idle", 1: "relaying", 2: "done", 3: "error"}
FRAG = 180  # the stock relays' fragment size


def framed(payload: bytes) -> list[bytes]:
    """[u16 BE length][payload] fragments, then the zero-length terminator."""
    out = [struct.pack(">H", len(payload[i:i + FRAG])) + payload[i:i + FRAG]
           for i in range(0, len(payload), FRAG)]
    return out + [b"\x00\x00"]


class Reader:
    """Reassembles framed notifications from the response characteristic."""

    def __init__(self):
        self.buf = bytearray()
        self.done = asyncio.Event()

    def on_notify(self, _sender, data: bytearray):
        if len(data) < 2:
            return
        n = struct.unpack(">H", data[:2])[0]
        if n == 0:
            self.done.set()
        else:
            self.buf += data[2:2 + n]

    def reset(self):
        self.buf = bytearray()
        self.done.clear()


def text_records(body: str) -> dict:
    """The key=value records of a text enroll response."""
    return dict(l.split("=", 1) for l in body.splitlines() if "=" in l)


async def find(address: str | None, timeout: float):
    if address:
        dev = await BleakScanner.find_device_by_address(address, timeout=timeout)
    else:
        dev = await BleakScanner.find_device_by_filter(
            lambda d, ad: SVC in [u.lower() for u in ad.service_uuids],
            timeout=timeout)
    if dev is None:
        print("no ZTP device found (is it in provisioning mode?)", file=sys.stderr)
    return dev


async def cmd_scan(args) -> int:
    found = await BleakScanner.discover(timeout=args.timeout, return_adv=True)
    n = 0
    for dev, ad in found.values():
        if SVC in [u.lower() for u in ad.service_uuids]:
            print(f"{dev.address}  {ad.local_name or dev.name or '?'}  rssi {ad.rssi}")
            n += 1
    return 0 if n else 2


async def get_envelope(client: BleakClient, reader: Reader, timeout: float) -> bytes:
    """Write the time and the kick; return the envelope the device sends."""
    now = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    await client.write_gatt_char(CH_TIMESYNC, now.encode(), response=False)
    reader.reset()
    await client.write_gatt_char(CH_REQUEST, b"\x00\x00", response=False)
    await asyncio.wait_for(reader.done.wait(), timeout)
    return bytes(reader.buf)


def submit(server: str, envelope: bytes, insecure: bool) -> tuple[int, str]:
    """POST the envelope; the device asked for the text rendering itself."""
    req = urllib.request.Request(
        server.rstrip("/") + "/v1/enroll", data=envelope, method="POST",
        headers={"Content-Type": "application/json"})
    ctx = ssl._create_unverified_context() if insecure else None
    try:
        with urllib.request.urlopen(req, context=ctx, timeout=30) as r:
            return r.status, r.read().decode()
    except urllib.error.HTTPError as e:  # 403 carries a rejection body
        return e.code, e.read().decode()


async def cmd_envelope(args) -> int:
    dev = await find(args.address, args.timeout)
    if dev is None:
        return 2
    reader = Reader()
    async with BleakClient(dev, timeout=args.timeout) as client:
        await client.start_notify(CH_RESPONSE, reader.on_notify)
        env = await get_envelope(client, reader, 60)
    print(json.dumps(json.loads(env), indent=2))
    return 0


async def cmd_enroll(args) -> int:
    dev = await find(args.address, args.timeout)
    if dev is None:
        return 2
    reader = Reader()
    status = asyncio.Queue()
    loop = asyncio.get_running_loop()
    deadline = loop.time() + args.wait

    async with BleakClient(dev, timeout=args.timeout) as client:
        print(f"connected to {dev.address} (MTU {client.mtu_size})")
        await client.start_notify(CH_RESPONSE, reader.on_notify)
        await client.start_notify(
            CH_STATUS, lambda _s, d: status.put_nowait(d[0] if d else None))

        while True:
            env = await get_envelope(client, reader, 60)
            print(f"envelope: {len(env)} bytes; submitting to {args.server}")
            code, body = await loop.run_in_executor(
                None, submit, args.server, env, args.insecure)
            rec = text_records(body)
            st = rec.get("status", f"HTTP {code}")
            print(f"server: {st}" + (f" ({rec['reason']})" if rec.get("reason") else "")
                  + f", {len(body)} bytes")

            # The device reads status and server_time from any response, so
            # write it back even when it is not an acceptance.
            while not status.empty():
                status.get_nowait()
            for frag in framed(body.encode()):
                await client.write_gatt_char(CH_REQUEST, frag, response=True)

            if st == "pending" and loop.time() < deadline:
                wait = int(rec.get("retry_after", "10") or 10)
                print(f"pending: retrying in {wait} s (approve the device on the server)")
                await asyncio.sleep(wait)
                continue
            if st != "accepted":
                return 1
            break

        # The device joins the network before it answers: allow for it.
        # It also reported "done" after sending the envelope, and that
        # notification can arrive late; only a result after "relaying" (set
        # when it starts on the bundle) is about the bundle.
        started = False
        try:
            while True:
                s = await asyncio.wait_for(status.get(), args.apply_timeout)
                if s == 1:
                    started = True
                if not started:
                    continue
                print(f"device: {STATUS.get(s, s)}")
                if s == 2:
                    print("provisioned; the device reboots into its application")
                    return 0
                if s == 3:
                    return 1
        except asyncio.TimeoutError:
            print("no answer from the device", file=sys.stderr)
            return 2


async def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--address", help="the device's BLE address (default: first found)")
    ap.add_argument("--timeout", type=float, default=15.0, help="scan/connect timeout (s)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("scan", help="list ZTP devices in provisioning mode")
    sub.add_parser("envelope", help="print the device's signed envelope")
    e = sub.add_parser("enroll", help="relay a full enrollment")
    e.add_argument("--server", required=True, help="ZTP server base URL")
    e.add_argument("--insecure", action="store_true",
                   help="accept a self-signed server certificate (lab only)")
    e.add_argument("--wait", type=float, default=0,
                   help="keep retrying while pending, for up to this many seconds")
    e.add_argument("--apply-timeout", type=float, default=90.0,
                   help="how long the device may take to test Wi-Fi and apply (s)")
    args = ap.parse_args()
    try:
        return await {"scan": cmd_scan, "envelope": cmd_envelope,
                      "enroll": cmd_enroll}[args.cmd](args)
    except asyncio.TimeoutError:
        print("timed out", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
