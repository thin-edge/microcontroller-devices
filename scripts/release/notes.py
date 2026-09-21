#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Release notes for the images in a directory.

    scripts/release/notes.py <dir> --tag v0.4.0 [--repo OWNER/REPO] > notes.md

<dir> holds each build's <stem>.meta.json (package.py) and <stem>.size.json
(size.py). The device list and variants come from release/devices.yml.
"""

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import matrix  # noqa: E402

README = "https://github.com/{repo}/blob/{tag}/README.md"


def load(d):
    metas = {}
    for f in sorted(Path(d).glob("*.meta.json")):
        m = json.loads(f.read_text())
        size = Path(d) / f"{m['stem']}.size.json"
        m["size"] = json.loads(size.read_text()) if size.is_file() else None
        metas[(m["device"], m["app"], m["variant"])] = m
    return metas


def notes(manifest, metas, tag, repo):
    readme = README.format(repo=repo, tag=tag)
    signing = {json.dumps(m["signing"], sort_keys=True): m["signing"] for m in metas.values()}
    out = [f"Prebuilt firmware for {len({m['device'] for m in metas.values()})} "
           f"devices, version `{tag.lstrip('v')}`. Pick your device and a variant, "
           "flash the `.factory.bin`, then provision the device over Bluetooth.",
           "",
           "## Variants",
           ""]
    for name, v in manifest["variants"].items():
        out.append(f"- **{name}**: {v['description']}.")
    out += ["",
            "## Images",
            "",
            "| Device | App | Variant | Flash this first | Update / OTA | Bundle |",
            "|---|---|---|---|---|---|"]
    for dev in manifest["devices"]:
        for (did, app, variant), m in metas.items():
            if did != dev["id"]:
                continue
            s = m["stem"]
            out.append(f"| {dev['name']} | `{app}` | {variant} | `{s}.factory.bin` | "
                       f"`{s}.app.bin` | `{s}.zip` |")
    out += ["",
            "## Flashing",
            "",
            "**From a browser** (Chrome or Edge, no tools): open "
            "[ESPHome Web](https://web.esphome.io/), connect the board, choose "
            "*Install*, tick *Erase device*, and pick the `.factory.bin`. "
            "When it offers to set up Wi-Fi afterwards, skip it: these images are "
            "provisioned over Bluetooth.",
            "",
            "**With esptool**:",
            "",
            "```sh",
            "esptool --chip <chip> erase-flash",
            "esptool --chip <chip> write-flash 0x0 <image>.factory.bin",
            "```",
            "",
            "The QT Py ESP32-S3 and ESP32-C6 need `--before usb-reset`; each "
            "bundle's `README.txt` has the exact commands, and "
            "`scripts/flash.sh <bundle-dir>` flashes a bundle from a checkout.",
            "",
            f"Then provision it: `standalone` images with Improv Wi-Fi, `tedge-*` "
            f"images with lab-ztp-provisioner. See [Provisioning Wi-Fi over BLE]"
            f"({readme}#provisioning-wi-fi-over-ble), including its security notes.",
            ""]
    tedge = [m for m in metas.values() if m["tedge"]]
    if tedge:
        versions = sorted({m["tedge_zephyr_version"] for m in tedge})
        out += ["## Updating over the air (tedge images)",
                "",
                "Put the application images into your tenant's firmware repository, "
                "then install from the device's *Firmware* tab:",
                "",
                "```sh",
                f"scripts/release/c8y-upload.sh {tag}",
                "```",
                "",
                f"Each image reports a firmware name unique to its device and app, and "
                f"the version `{tag.lstrip('v')}`; `c8y-firmware.json` lists them.",
                "",
                f"tedge-zephyr {', '.join(versions)}. Features per image:",
                "",
                "| Image | Features |",
                "|---|---|"]
        for m in tedge:
            out.append(f"| `{m['firmware_name']}` | {', '.join(m['features'])} |")
        no_renew = [m["firmware_name"] for m in tedge
                    if "cert-renewal" not in m["features"]]
        if no_renew:
            out += ["",
                    "Without certificate renewal (" +
                    ", ".join(f"`{n}`" for n in no_renew) +
                    "): these devices must be onboarded again before their "
                    "certificate expires, a year after enrolment."]
        out += ["",
                "Measurements are published on `te/device/<id>///m/<type>` over the "
                "Cumulocity MQTT Service; the tenant has to map that topic to see them.",
                ""]
    out += ["## Signing", ""]
    for s in signing.values():
        out.append(f"- {s['statement']}")
    out += ["",
            "A device's bootloader only accepts images signed with its own key: a "
            "device flashed from one key cannot be updated over the air to images "
            "signed with another.",
            "",
            "## Sizes",
            "",
            "| Image | App image / slot0 | RAM (fullest region) |",
            "|---|---|---|"]
    for m in metas.values():
        s = m["size"]
        if not s:
            continue
        ram = max(s["ram"], key=lambda r: r["percent"]) if s["ram"] else None
        ram_txt = (f"{ram['region']} {ram['used']} / {ram['size']} B "
                   f"({ram['percent']}%)") if ram else "—"
        out.append(f"| `{m['firmware_name']}` | {s['image']} / {s['slot0']} B "
                   f"({s['slot0_percent']}%) | {ram_txt} |")
    out += ["",
            "Verify downloads with `sha256sum -c SHA256SUMS`.",
            ""]
    return "\n".join(out)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("dir")
    ap.add_argument("--tag", required=True)
    ap.add_argument("--repo", default="thin-edge/microcontroller-devices")
    ap.add_argument("--manifest", default=matrix.DEFAULT_MANIFEST)
    args = ap.parse_args(argv)
    manifest, _ = matrix.load(args.manifest)
    print(notes(manifest, load(args.dir), args.tag, args.repo))
    return 0


if __name__ == "__main__":
    sys.exit(main())
