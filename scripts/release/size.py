#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Size report, flash budget and RAM/flash baseline gate for one sysbuild build.

    scripts/release/size.py <build-dir> [--json <file>]
        [--max-slot 80] [--max-prov 92] [--ota-ceiling 95]
        [--baseline release/size-baseline.json [--tolerance 256] [--write-baseline]]

Reports, for the application and for the Wi-Fi provisioner built beside it:

- the signed image against its partition (the application against `slot0`,
  the provisioner against `prov`);
- the static use of each RAM region, from the allocated sections of
  zephyr.elf and the regions zephyr.map declares;
- the libc malloc arena that is left: `_heap_sentry - _end`, which is what
  Zephyr's common libc gives malloc when COMMON_LIBC_MALLOC_ARENA_SIZE is -1
  (lib/libc/common/source/stdlib/malloc.c). open62541 and the shell allocate
  from it, so an image that links can still fail here.

Flash figures are image files, not section sums: an ESP image pads its
RAM-loaded segments (IRAM code, .data) to a 64 KB boundary before the
flash-mapped code and rodata, so bytes moved from .data to rodata can grow
the image although nothing was added, and small .data changes may not show.

Two flash checks apply. The *budget* (--max-slot, --max-prov) keeps room to
grow: an image above it fails unless the baseline lists it as a known
exception (`budget_exceptions`), in which case it must not grow. The *OTA
ceiling* (--ota-ceiling, applied to the application) is the hard limit:
slot1 has to take an update of the same size.

With --baseline, every RAM region and image size is compared with the
committed figure for this build (keyed by CONFIG_APP_FIRMWARE_NAME); growth
past --tolerance fails, naming the build, the region and the delta. A change
that legitimately grows or shrinks a build updates the baseline in the same
pull request with --write-baseline (scripts/release/baseline.sh does it for
every build).
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
REGION_RE = re.compile(r"^(\w+)\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)", re.M)
# RAM regions worth reporting; flash-mapped ones (irom/drom/FLASH) are not.
RAM_RE = re.compile(r"^(i?dram\d_\d_seg|iram\d_\d_seg|sram\d_\d_seg|dram\d_seg|lp_ram_seg|RAM|SRAM)$")
SYMBOL_RE = re.compile(r"^\s+(0x[0-9a-f]+)\s+(_end|_heap_sentry)\s*(=|$)", re.M)
NAME_RE = re.compile(r'^CONFIG_APP_FIRMWARE_NAME="([^"]+)"', re.M)

PROVISIONER = "wifi-provisioner"


def sections(elf, names=False):
    """(addr, size) of every allocated section of a 32- or 64-bit LE ELF;
    with names=True, (name, addr, size)."""
    data = Path(elf).read_bytes()
    is64 = data[4] == 2
    if is64:
        shoff, = struct.unpack_from("<Q", data, 0x28)
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    else:
        shoff, = struct.unpack_from("<I", data, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)

    def header(i):
        base = shoff + i * shentsize
        if is64:
            name, _, flags, addr, off, size = struct.unpack_from("<IIQQQQ", data, base)
        else:
            name, _, flags, addr, off, size = struct.unpack_from("<IIIIII", data, base)
        return name, flags, addr, off, size

    strtab = None
    if names and shstrndx < shnum:
        _, _, _, off, size = header(shstrndx)
        strtab = data[off:off + size]
    out = []
    for i in range(shnum):
        name, flags, addr, _, size = header(i)
        if flags & SHF_ALLOC and size:
            if names:
                label = strtab[name:strtab.index(b"\0", name)].decode() if strtab else ""
                out.append((label, addr, size))
            else:
                out.append((addr, size))
    return out


def padding(named_sections):
    """Bytes of alignment padding inside the image. An ESP image pads its
    RAM-loaded segments to a 64 KB boundary before the flash-mapped code and
    rodata (.flash.align_* sections), so the image file moves in steps: code
    removed below a boundary does not show until enough goes to cross it."""
    return sum(size for name, _, size in named_sections if name.startswith(".flash.align"))


def regions(map_text):
    """[(name, origin, length)] from the map's Memory Configuration block."""
    block = map_text.split("Memory Configuration", 1)[1].split("Linker script", 1)[0]
    return [(n, int(o, 16), int(l, 16)) for n, o, l in REGION_RE.findall(block)
            if n != "*default*" and int(l, 16) > 0]


def heap_symbols(map_text):
    """{'_end': addr, '_heap_sentry': addr} as the linker resolved them."""
    found = {}
    for addr, name, _ in SYMBOL_RE.findall(map_text):
        found.setdefault(name, int(addr, 16))
    return found


def ram_use(elf_sections, regs):
    """Per-region static use ([{region, used, size, percent}]) and the
    region each section landed in."""
    used = {}
    for addr, size in elf_sections:
        for name, origin, length in regs:
            if origin <= addr < origin + length:
                used[name] = used.get(name, 0) + size
                break
    return [{"region": n, "used": used.get(n, 0), "size": l,
             "percent": round(100.0 * used.get(n, 0) / l, 2)}
            for n, _, l in regs if RAM_RE.match(n) and used.get(n)]


def arena(symbols, regs):
    """The libc malloc arena: from `_end` to `_heap_sentry`, and the RAM
    region it lives in. None when the map does not define both symbols."""
    if "_end" not in symbols or "_heap_sentry" not in symbols:
        return None
    start, end = symbols["_end"], symbols["_heap_sentry"]
    region = next((n for n, o, l in regs if o <= start < o + l), None)
    return {"region": region, "start": start, "end": end,
            "size": max(0, end - start)}


def image_report(zephyr_dir, image_file, partition, part_size):
    """Image size against its partition, RAM regions and the arena of one
    Zephyr image (an <image>/zephyr directory of the sysbuild build)."""
    zephyr_dir = Path(zephyr_dir)
    map_text = (zephyr_dir / "zephyr.map").read_text(errors="replace")
    regs = regions(map_text)
    image = Path(image_file).stat().st_size
    named = sections(zephyr_dir / "zephyr.elf", names=True)
    pad = padding(named)
    return {
        "image": image,
        partition: part_size,
        f"{partition}_percent": round(100.0 * image / part_size, 2),
        # What the image carries besides alignment padding: the figure to
        # watch when trimming flash, since the image itself moves in 64 KB steps.
        "padding": pad,
        "content": image - pad,
        "ram": ram_use([(a, sz) for _, a, sz in named], regs),
        "arena": arena(heap_symbols(map_text), regs),
    }


def firmware_name(app_zephyr_dir, fallback):
    config = Path(app_zephyr_dir) / ".config"
    if config.is_file():
        m = NAME_RE.search(config.read_text(errors="replace"))
        if m:
            return m.group(1)
    return fallback


def report(build):
    info = offsets.read(build)
    build = Path(build)
    app = build / info["app"] / "zephyr"
    r = {"name": firmware_name(app, info["app"]), "app": info["app"]}
    r.update(image_report(app, info["images"]["app"]["file"], "slot0",
                          info["partitions"]["slot0"]["size"]))
    prov = build / PROVISIONER / "zephyr"
    if (prov / "zephyr.map").is_file():
        r["provisioner"] = image_report(prov, info["images"]["provisioner"]["file"],
                                        "prov", info["partitions"]["prov"]["size"])
    return r


# --- baseline ---------------------------------------------------------------

def baseline_entry(r):
    """The figures the baseline keeps for one build: every image size and RAM
    region that must not grow, plus the arena for the record."""
    entry = {
        "image": r["image"],
        "content": r.get("content"),
        "ram": {reg["region"]: reg["used"] for reg in r["ram"]},
        "arena": r["arena"]["size"] if r.get("arena") else None,
    }
    if "provisioner" in r:
        p = r["provisioner"]
        entry["provisioner"] = {
            "image": p["image"],
            "content": p.get("content"),
            "ram": {reg["region"]: reg["used"] for reg in p["ram"]},
            "arena": p["arena"]["size"] if p.get("arena") else None,
        }
    return entry


def load_baseline(path):
    path = Path(path)
    return json.loads(path.read_text()) if path.is_file() else {}


def write_baseline(path, r):
    """Merge this build's entry into the baseline file, keeping every other
    build's entry and this build's `budget_exceptions` note."""
    path = Path(path)
    data = load_baseline(path)
    entry = baseline_entry(r)
    old = data.get(r["name"], {})
    if "budget_exceptions" in old:
        entry["budget_exceptions"] = old["budget_exceptions"]
    data[r["name"]] = entry
    path.write_text(json.dumps(dict(sorted(data.items())), indent=2) + "\n")


def _growth(name, what, now, before, tolerance, problems, notes):
    delta = now - before
    sign = "+" if delta >= 0 else ""
    notes.append(f"{what}: {now} B (baseline {before} B, {sign}{delta})")
    if delta > tolerance:
        problems.append(f"{name}: {what} grew by {delta} B past the baseline "
                        f"({before} -> {now} B, tolerance {tolerance} B)")


def compare(r, baseline, tolerance):
    """Compare a report with its baseline entry.

    Returns (problems, notes): problems fail the gate, notes are the deltas
    worth printing either way."""
    name = r["name"]
    problems, notes = [], []
    entry = baseline.get(name)
    if entry is None:
        return ([f"{name}: not in the baseline; add it with --write-baseline"], notes)

    def image(prefix, rep, base):
        _growth(name, f"{prefix}image", rep["image"], base["image"],
                tolerance, problems, notes)
        base_ram = base.get("ram", {})
        for reg in rep["ram"]:
            what = f"{prefix}{reg['region']}"
            if reg["region"] not in base_ram:
                problems.append(f"{name}: {what}: region not in the baseline "
                                f"({reg['used']} B used); update it with --write-baseline")
                continue
            _growth(name, what, reg["used"], base_ram[reg["region"]], tolerance,
                    problems, notes)
        if rep.get("content") is not None and base.get("content") is not None:
            delta = rep["content"] - base["content"]
            sign = "+" if delta >= 0 else ""
            notes.append(f"{prefix}image content: {rep['content']} B "
                         f"(baseline {base['content']} B, {sign}{delta})")
        if rep.get("arena") and base.get("arena") is not None:
            delta = rep["arena"]["size"] - base["arena"]
            sign = "+" if delta >= 0 else ""
            notes.append(f"{prefix}malloc arena: {rep['arena']['size']} B "
                         f"(baseline {base['arena']} B, {sign}{delta})")

    image("", r, entry)
    if "provisioner" in r and "provisioner" in entry:
        image("provisioner ", r["provisioner"], entry["provisioner"])
    elif "provisioner" in r:
        problems.append(f"{name}: provisioner not in the baseline; "
                        "update it with --write-baseline")
    return problems, notes


# --- flash budget -----------------------------------------------------------

def budget(r, max_slot, max_prov, ota_ceiling, baseline):
    """Flash checks. Returns (problems, warnings)."""
    name = r["name"]
    problems, warnings = [], []
    exceptions = baseline.get(name, {}).get("budget_exceptions", {})

    def check(what, rep, partition, limit):
        pct = rep[f"{partition}_percent"]
        if pct <= limit:
            return
        msg = (f"{name}: {what} image is {rep['image']} B, {pct}% of the "
               f"{rep[partition]} B {partition} partition (budget {limit:g}%)")
        if what in exceptions:
            warnings.append(f"{msg}; known exception: {exceptions[what]}")
        else:
            problems.append(msg)

    check("app", r, "slot0", max_slot)
    if r["slot0_percent"] > ota_ceiling:
        problems.append(f"{name}: app image is {r['image']} B, {r['slot0_percent']}% "
                        f"of the {r['slot0']} B slot0 (OTA ceiling {ota_ceiling:g}%): "
                        "slot1 must hold an update of the same size")
    if "provisioner" in r:
        check("provisioner", r["provisioner"], "prov", max_prov)
    return problems, warnings


# --- output -----------------------------------------------------------------

def print_report(r, max_slot, max_prov):
    print(r["name"])
    print(f"  app image {r['image']} B of slot0 {r['slot0']} B "
          f"({r['slot0_percent']}%, budget {max_slot:g}%); "
          f"{r['content']} B content + {r['padding']} B alignment padding")
    for reg in r["ram"]:
        print(f"  {reg['region']}: {reg['used']} B of {reg['size']} B ({reg['percent']}%)")
    if r.get("arena"):
        print(f"  malloc arena: {r['arena']['size']} B left in {r['arena']['region']}")
    if "provisioner" in r:
        p = r["provisioner"]
        print(f"  provisioner image {p['image']} B of prov {p['prov']} B "
              f"({p['prov_percent']}%, budget {max_prov:g}%); "
              f"{p['content']} B content + {p['padding']} B alignment padding")
        for reg in p["ram"]:
            print(f"  provisioner {reg['region']}: {reg['used']} B of {reg['size']} B "
                  f"({reg['percent']}%)")
        if p.get("arena"):
            print(f"  provisioner malloc arena: {p['arena']['size']} B left "
                  f"in {p['arena']['region']}")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("build")
    ap.add_argument("--json", help="also write the report here")
    ap.add_argument("--max-slot", type=float, default=80.0,
                    help="application flash budget, percent of slot0 (default 80)")
    ap.add_argument("--max-prov", type=float, default=92.0,
                    help="provisioner flash budget, percent of prov (default 92)")
    ap.add_argument("--ota-ceiling", type=float, default=95.0,
                    help="hard limit for the application, percent of slot0 "
                         "(default 95): slot1 must take an update of the same size")
    ap.add_argument("--baseline", help="compare with this per-build baseline file")
    ap.add_argument("--tolerance", type=int, default=256,
                    help="bytes a RAM region or image may grow past the "
                         "baseline (default 256)")
    ap.add_argument("--write-baseline", action="store_true",
                    help="record this build in --baseline instead of comparing")
    args = ap.parse_args(argv)
    if args.write_baseline and not args.baseline:
        ap.error("--write-baseline needs --baseline")

    r = report(args.build)
    print_report(r, args.max_slot, args.max_prov)
    if args.json:
        Path(args.json).write_text(json.dumps(r, indent=2) + "\n")

    baseline = load_baseline(args.baseline) if args.baseline else {}
    problems, warnings = budget(r, args.max_slot, args.max_prov, args.ota_ceiling,
                                baseline)
    if args.baseline:
        if args.write_baseline:
            write_baseline(args.baseline, r)
            print(f"  baseline: recorded in {args.baseline}")
        else:
            more, notes = compare(r, baseline, args.tolerance)
            for n in notes:
                print(f"  vs baseline: {n}")
            problems += more
    for w in warnings:
        print(f"warning: {w}", file=sys.stderr)
    for p in problems:
        print(f"error: {p}", file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
