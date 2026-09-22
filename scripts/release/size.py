#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Size report and OTA-margin gate for one sysbuild build.

    scripts/release/size.py <build-dir> [--json <file>] [--max-slot 95]

Reports the signed application image against slot0 and the use of each
memory region, read from the application's zephyr.elf (allocated sections by
address) and the regions its zephyr.map declares. Fails when the image is
above --max-slot percent of slot0: slot1 has to take an OTA image of the
same size, and an image at the edge of it leaves no room to grow.
"""

import argparse
import json
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import offsets  # noqa: E402

SHF_ALLOC = 0x2
SHT_NOBITS = 8
REGION_RE = re.compile(r"^(\w+)\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)", re.M)
# RAM regions worth reporting; flash-mapped ones (irom/drom/FLASH) are not.
RAM_RE = re.compile(r"^(i?dram\d_\d_seg|iram\d_\d_seg|sram\d_\d_seg|dram\d_seg|lp_ram_seg|RAM|SRAM)$")


def sections(elf):
    """(addr, size) of every allocated section of a 32- or 64-bit LE ELF."""
    data = Path(elf).read_bytes()
    is64 = data[4] == 2
    if is64:
        shoff, = struct.unpack_from("<Q", data, 0x28)
        shentsize, shnum = struct.unpack_from("<HH", data, 0x3A)
    else:
        shoff, = struct.unpack_from("<I", data, 0x20)
        shentsize, shnum = struct.unpack_from("<HH", data, 0x2E)
    out = []
    for i in range(shnum):
        base = shoff + i * shentsize
        if is64:
            _, _, flags, addr, _, size = struct.unpack_from("<IIQQQQ", data, base)
        else:
            _, _, flags, addr, _, size = struct.unpack_from("<IIIIII", data, base)
        if flags & SHF_ALLOC and size:
            out.append((addr, size))
    return out


def regions(map_file):
    text = Path(map_file).read_text(errors="replace")
    block = text.split("Memory Configuration", 1)[1].split("Linker script", 1)[0]
    return [(n, int(o, 16), int(l, 16)) for n, o, l in REGION_RE.findall(block)
            if n != "*default*" and int(l, 16) > 0]


def report(build):
    info = offsets.read(build)
    app = Path(build) / info["app"] / "zephyr"
    image = Path(info["images"]["app"]["file"]).stat().st_size
    slot = info["partitions"]["slot0"]["size"]
    used = {}
    regs = regions(app / "zephyr.map")
    for addr, size in sections(app / "zephyr.elf"):
        for name, origin, length in regs:
            if origin <= addr < origin + length:
                used[name] = used.get(name, 0) + size
                break
    ram = [{"region": n, "used": used.get(n, 0), "size": l,
            "percent": round(100.0 * used.get(n, 0) / l, 2)}
           for n, _, l in regs if RAM_RE.match(n) and used.get(n)]
    return {
        "app": info["app"],
        "image": image,
        "slot0": slot,
        "slot0_percent": round(100.0 * image / slot, 2),
        "ram": ram,
    }


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("build")
    ap.add_argument("--json", help="also write the report here")
    ap.add_argument("--max-slot", type=float, default=95.0,
                    help="fail above this percent of slot0 (default 95)")
    args = ap.parse_args(argv)
    r = report(args.build)
    print(f"{r['app']}: image {r['image']} B of slot0 {r['slot0']} B "
          f"({r['slot0_percent']}%)")
    for reg in r["ram"]:
        print(f"  {reg['region']}: {reg['used']} B of {reg['size']} B ({reg['percent']}%)")
    if args.json:
        Path(args.json).write_text(json.dumps(r, indent=2) + "\n")
    if r["slot0_percent"] > args.max_slot:
        print(f"error: {r['app']} image is {r['image']} B, {r['slot0_percent']}% "
              f"of the {r['slot0']} B slot0 (limit {args.max_slot}%): slot1 must "
              "hold an update of the same size", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
