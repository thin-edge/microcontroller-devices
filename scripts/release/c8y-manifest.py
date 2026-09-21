#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Write c8y-firmware.json: what a release offers Cumulocity's firmware repository.

    scripts/release/c8y-manifest.py <dir-with-meta.json-files> > c8y-firmware.json

One entry per tedge image (standalone images have no cloud client). The
version is exactly what the image reports once installed — the application's
own version string — so an install is recognised as done rather than as a
rollback. The name is unique per (app, variant, device), so a firmware
repository entry only ever holds images for one kind of hardware.
scripts/release/c8y-upload.sh reads it.
"""

import json
import sys
from pathlib import Path


def build(meta_dir):
    entries, versions = [], set()
    for f in sorted(Path(meta_dir).glob("*.meta.json")):
        m = json.loads(f.read_text())
        versions.add(m["version"])
        if not m.get("tedge"):
            continue
        asset = f"{m['stem']}.app.bin"
        entries.append({
            "name": m["firmware_name"],
            "version": m["version"],
            "deviceType": m["device_type"],
            "asset": asset,
            "sha256": m["assets"][asset],
            "device": m["device"],
            "deviceName": m["device_name"],
            "app": m["app"],
            "variant": m["variant"],
            "features": m["features"],
            "tedgeZephyrVersion": m["tedge_zephyr_version"],
            "signing": m["signing"],
        })
    if len(versions) > 1:
        raise SystemExit(f"images of different versions in one release: {sorted(versions)}")
    return {"schema": 1, "version": versions.pop() if versions else None,
            "firmware": entries}


def main(argv):
    if len(argv) != 2:
        print(__doc__.split("\n\n")[1], file=sys.stderr)
        return 1
    print(json.dumps(build(argv[1]), indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
