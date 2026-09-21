#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Flash offsets of a `west build --sysbuild` build or of a release bundle.

    scripts/release/offsets.py <build-dir|bundle-dir>          # JSON
    scripts/release/offsets.py <build-dir|bundle-dir> --shell  # NAME_OFF=0x.. ...

A release bundle is a directory with a flash.json (scripts/release/package.py
writes it); its offsets were read from the build's devicetree when it was
packaged.

The application's zephyr.dts is what MCUboot, the application and the
provisioner were all linked for (lib/common/dts/layout-*.dtsi), so it is the
one source for where each image goes. The classic ESP32 ROM loads the
second-stage bootloader from 0x1000 whatever the partition table says.

Used by scripts/flash.sh and scripts/release/package.py.
"""

import argparse
import json
import re
import sys
from pathlib import Path

PARTITION_RE = re.compile(
    r"(\w+)_partition:\s*partition@[0-9a-f]+\s*\{[^}]*?reg\s*=\s*<\s*(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s*>",
    re.S)
SOC_RE = re.compile(r'^CONFIG_SOC="([^"]+)"', re.M)

# Domains a provisioning sysbuild build has besides the application.
NOT_THE_APP = {"mcuboot", "wifi-provisioner"}


def app_dir(build):
    """The application domain: the one with a signed image that is not
    MCUboot or the provisioner."""
    build = Path(build)
    for d in sorted(p for p in build.iterdir() if p.is_dir()):
        if d.name not in NOT_THE_APP and (d / "zephyr" / "zephyr.signed.bin").is_file():
            return d
    raise FileNotFoundError(f"no signed application image in {build}")


def read_bundle(bundle):
    bundle = Path(bundle)
    meta = json.loads((bundle / "flash.json").read_text())
    return {
        "app": meta["app"],
        "chip": meta["chip"],
        "flash_size": meta["flash_size"],
        "esptool_before": meta.get("esptool_before", "default-reset"),
        "images": {i["name"]: {"offset": int(i["offset"], 16),
                               "file": str(bundle / i["file"])}
                   for i in meta["images"]},
        "partitions": {n: {"offset": int(p["offset"], 16), "size": int(p["size"], 16)}
                       for n, p in meta["partitions"].items()},
    }


def read(build):
    build = Path(build)
    if (build / "flash.json").is_file():
        return read_bundle(build)
    if not (build / "mcuboot").is_dir() or not (build / "wifi-provisioner").is_dir():
        raise FileNotFoundError(
            f"{build} is not a sysbuild build with MCUboot and the provisioner "
            "(build with: west build --sysbuild ...)")
    app = app_dir(build)
    dts = (app / "zephyr" / "zephyr.dts").read_text()
    soc = SOC_RE.search((app / "zephyr" / ".config").read_text()).group(1)
    parts = {name: {"offset": int(off, 16), "size": int(size, 16)}
             for name, off, size in PARTITION_RE.findall(dts)}
    for required in ("boot", "slot0", "prov", "bootreq", "storage"):
        if required not in parts:
            raise ValueError(f"{app}/zephyr/zephyr.dts has no {required}_partition")
    boot = 0x1000 if soc == "esp32" else parts["boot"]["offset"]
    return {
        "app": app.name,
        "chip": soc,
        "images": {
            "mcuboot": {"offset": boot,
                        "file": str(build / "mcuboot" / "zephyr" / "zephyr.bin")},
            "app": {"offset": parts["slot0"]["offset"],
                    "file": str(app / "zephyr" / "zephyr.signed.bin")},
            "provisioner": {"offset": parts["prov"]["offset"],
                            "file": str(build / "wifi-provisioner" / "zephyr" /
                                        "zephyr.signed.bin")},
        },
        "partitions": parts,
    }


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("build")
    ap.add_argument("--shell", action="store_true",
                    help="print shell assignments instead of JSON")
    args = ap.parse_args(argv)
    try:
        info = read(args.build)
    except (OSError, ValueError, AttributeError) as e:
        print(e, file=sys.stderr)
        return 1
    if args.shell:
        print(f"CHIP={info['chip']}")
        print(f"FLASH_SIZE={info.get('flash_size', '')}")
        print(f"ESPTOOL_BEFORE={info.get('esptool_before', '')}")
        for name, img in info["images"].items():
            print(f"{name.upper()}_OFF={img['offset']:#x}")
            print(f"{name.upper()}_FILE='{img['file']}'")
        for name, p in info["partitions"].items():
            print(f"{name.upper()}_PART_OFF={p['offset']:#x}")
            print(f"{name.upper()}_PART_SIZE={p['size']:#x}")
    else:
        print(json.dumps(info, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
