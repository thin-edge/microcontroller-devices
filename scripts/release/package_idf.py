#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Package one ESP-IDF build (`idf.py build`) as release assets.

    scripts/release/package_idf.py <build-dir> --device <id> --app <app> \\
        --variant <variant> --out <dir> [--max-slot 80]

The ESP-IDF counterpart of package.py, with the same outputs, so the release
notes, c8y-firmware.json and SHA256SUMS treat both alike. With
<stem> = <app>-<variant>-<device>-<version>:

  <stem>.factory.bin  bootloader, partition table, OTA data and application
                      merged into one image that is written at 0x0
  <stem>.app.bin      the application alone: OTA and --app-only
  <stem>.zip          <stem>/ with the images, flash.json and README.txt
                      (scripts/flash.sh <stem>/ flashes it)
  <stem>.meta.json    what the release notes and c8y-firmware.json are made
                      from; not a release asset
  <stem>.size.json    the application image against its OTA slot

The offsets come from the build's flasher_args.json, the settings from
config/sdkconfig.json, and the version from project_description.json. The
build fails when the application takes more than --max-slot percent of its
OTA slot, so an update always has room to land. Needs esptool on PATH.
"""

import argparse
import csv
import hashlib
import json
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import matrix  # noqa: E402

ROOT = matrix.ROOT

SIGNING = {
    "key": "none",
    "fingerprint": None,
    "statement": ("ESP-IDF images (tedge-onboard) are not signed: the "
                  "bootloader checks each image's checksum, not who built it. "
                  "Turn on Secure Boot for production devices."),
}

FEATURES = ("TELEMETRY", "HEALTH", "RESTART", "FIRMWARE_UPDATE",
            "REMOTE_ACCESS", "SHELL_COMMAND", "LOG_UPLOAD", "COREDUMP",
            "PARAMETERS", "CERT_RENEWAL")

README = """{name}
{rule}

{device_name} — {app} ({variant}), version {version}

Flash everything (first flash; erases the board, including the stored Wi-Fi
network and Cumulocity certificate):

  esptool --chip {chip} --before {before} erase-flash
  esptool --chip {chip} --before {before} write-flash {writes}

or, from a checkout of the repository:

  scripts/flash.sh {name} --erase-all

or, without any tools, the single image {stem}.factory.bin at 0x0 from a
browser flasher such as https://web.esphome.io/ (Chrome or Edge).

Update only the application (keeps the Wi-Fi network and the certificate):

  esptool --chip {chip} --before {before} write-flash {app_off} app.bin

Then set it up over USB: run tedge-onboard on the computer the board is
plugged into (https://github.com/thin-edge/tiny-edge/tree/main/tools/onboard).
It gives the device the computer's Wi-Fi network, registers it in your
Cumulocity tenant, and passes it the tenant and a one-time password.

{signing}
"""

# The names the images get in the bundle, by flasher_args.json's key.
NAMES = {"bootloader": "bootloader.bin", "partition-table": "partition-table.bin",
         "otadata": "ota_data_initial.bin", "app": "app.bin"}


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def partitions(csv_path):
    """name -> {offset, size} from an ESP-IDF partition table CSV."""
    out = {}
    with open(csv_path, newline="") as f:
        rows = csv.reader(line for line in f
                          if line.strip() and not line.lstrip().startswith("#"))
        for row in rows:
            row = [c.strip() for c in row]
            if len(row) >= 5 and row[3] and row[4]:
                out[row[0]] = {"offset": int(row[3], 0), "size": int(row[4], 0)}
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("build")
    ap.add_argument("--device", required=True)
    ap.add_argument("--app", required=True)
    ap.add_argument("--variant", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--manifest", default=matrix.DEFAULT_MANIFEST)
    ap.add_argument("--esptool", default="esptool")
    ap.add_argument("--max-slot", type=float, default=80.0,
                    help="largest share of the OTA slot the application may take")
    args = ap.parse_args(argv)

    manifest, builds = matrix.load(args.manifest)
    build = next((b for b in builds if b.device["id"] == args.device and
                  b.app == args.app and b.variant == args.variant), None)
    if build is None or build.framework != "esp-idf":
        print(f"{args.device} {args.app} {args.variant} is not an ESP-IDF build "
              "in the manifest", file=sys.stderr)
        return 1
    dev = build.device
    bdir = Path(args.build)

    flasher = json.loads((bdir / "flasher_args.json").read_text())
    project = json.loads((bdir / "project_description.json").read_text())
    cfg = json.loads((bdir / "config" / "sdkconfig.json").read_text())
    version = project["project_version"]
    chip = flasher["extra_esptool_args"]["chip"]
    if chip != dev["chip"]:
        print(f"the build is for {chip}, the device is {dev['chip']}",
              file=sys.stderr)
        return 1
    if cfg.get("APP_FIRMWARE_NAME") != build.firmware_name:
        print(f"the image reports firmware name {cfg.get('APP_FIRMWARE_NAME')!r}, "
              f"expected {build.firmware_name!r}", file=sys.stderr)
        return 1
    # No release image carries site settings.
    for key in ("SAMPLE_WIFI_SSID", "SAMPLE_WIFI_PASSWORD", "TEDGE_C8Y_URL"):
        if cfg.get(key):
            print(f"CONFIG_{key} is set in this build: release images carry "
                  "no credentials or tenant", file=sys.stderr)
            return 1

    parts = partitions(ROOT / "apps" / args.app /
                       cfg["PARTITION_TABLE_CUSTOM_FILENAME"])
    slot = parts["ota_0"]["size"]
    app_file = bdir / flasher["app"]["file"]
    app_size = app_file.stat().st_size
    percent = round(100.0 * app_size / slot, 1)
    if percent > args.max_slot:
        print(f"the application is {app_size} B, {percent}% of its "
              f"{slot} B OTA slot (at most {args.max_slot}%)", file=sys.stderr)
        return 1

    stem = f"{build.firmware_name}-{version}"
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    images = sorted(((k, flasher[k]) for k in NAMES if k in flasher),
                    key=lambda kv: int(kv[1]["offset"], 0))

    factory = out / f"{stem}.factory.bin"
    cmd = [args.esptool, "--chip", chip, "merge-bin", "--output", str(factory),
           "--format", "raw", *flasher["write_flash_args"]]
    for _, img in images:
        cmd += [img["offset"], str(bdir / img["file"])]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)

    app_bin = out / f"{stem}.app.bin"
    shutil.copyfile(app_file, app_bin)

    before = dev.get("esptool_before", "default-reset")
    flash = {
        "schema": 1,
        "framework": "esp-idf",
        "device": dev["id"],
        "device_name": dev["name"],
        "board": dev["board"],
        "app": args.app,
        "variant": args.variant,
        "version": version,
        "firmware_name": build.firmware_name,
        "chip": chip,
        "flash_size": dev["flash_size"],
        "esptool_before": before,
        "flash_args": flasher["write_flash_args"],
        "images": [{"name": n, "file": NAMES[n], "offset": i["offset"]}
                   for n, i in images],
        "partitions": {n: {"offset": f"{p['offset']:#x}", "size": f"{p['size']:#x}"}
                       for n, p in parts.items()},
        "signing": SIGNING,
    }
    readme = README.format(
        name=stem, rule="=" * len(stem), stem=stem, device_name=dev["name"],
        app=args.app, variant=args.variant, version=version, chip=chip,
        before=before, app_off=flasher["app"]["offset"],
        writes=" ".join(f"{i['offset']} {NAMES[n]}" for n, i in images),
        signing=SIGNING["statement"])

    bundle = out / f"{stem}.zip"
    with zipfile.ZipFile(bundle, "w", zipfile.ZIP_DEFLATED) as z:
        for n, i in images:
            z.write(bdir / i["file"], f"{stem}/{NAMES[n]}")
        z.writestr(f"{stem}/flash.json", json.dumps(flash, indent=2) + "\n")
        z.writestr(f"{stem}/README.txt", readme)

    tiny_edge = bdir / "_deps" / "tiny_edge-src" / "VERSION"
    meta = {
        **{k: flash[k] for k in ("device", "device_name", "board", "app",
                                 "variant", "version", "firmware_name", "chip",
                                 "flash_size")},
        "framework": "esp-idf",
        "stem": stem,
        "tedge": True,
        # No device type for the firmware repository's filter: tedge-onboard
        # registers every device as "thin-edge.io", so an entry restricted to
        # the image's own type would not be offered in the devices' Firmware
        # tab. The firmware name already names the app, variant and board.
        "device_type": None,
        "image_device_type": cfg.get("APP_DEVICE_TYPE"),
        "features": [k.lower().replace("_", "-") for k in FEATURES
                     if cfg.get(f"TEDGE_{k}") is True],
        "tedge_zephyr_version": (tiny_edge.read_text().strip()
                                 if tiny_edge.is_file() else None),
        "measured": build.measured,
        "signing": SIGNING,
        "assets": {p.name: sha256(p) for p in (factory, app_bin, bundle)},
    }
    (out / f"{stem}.meta.json").write_text(json.dumps(meta, indent=2) + "\n")
    size = {"image": app_size, "slot0": slot, "slot0_percent": percent,
            "ram": []}
    (out / f"{stem}.size.json").write_text(json.dumps(size, indent=2) + "\n")
    for p in (factory, app_bin, bundle):
        print(p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
