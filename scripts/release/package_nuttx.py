#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Package one NuttX build (tiny-edge's ports/nuttx/scripts/build.sh) as
release assets.

    scripts/release/package_nuttx.py <build-out> --config <nuttx/.config> \\
        --device <id> --app <app> --variant <variant> --version <x.y.z> \\
        --tiny-edge <dir> --out <dir> [--max-slot 95]

The NuttX counterpart of package.py and package_idf.py, with the same
outputs, so the release notes, c8y-firmware.json and SHA256SUMS treat every
framework alike. <build-out> is what tiny-edge's build.sh wrote: MCUboot,
nuttx.bin (padded and confirmed, for flashing) and nuttx-update.bin (for a
firmware update). With <stem> = <app>-<os>-<set>-<device>-<version>:

  <stem>.factory.bin  MCUboot and the confirmed image, merged into one image
                      that is written at 0x0
  <stem>.app.bin      the firmware update: unpadded and not confirmed, so
                      MCUboot boots it once on trial and the agent confirms it
  <stem>.zip          <stem>/ with MCUboot, the confirmed image, flash.json
                      and README.txt (scripts/flash.sh <stem>/ flashes it)
  <stem>.meta.json    what the release notes and c8y-firmware.json are made
                      from; not a release asset
  <stem>.size.json    the update image against its slot

The slots, the data partition, the firmware name, the device type and the
features come from the build's NuttX configuration. The build fails when the
update image takes more than --max-slot percent of its slot. Needs esptool
on PATH.
"""

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import matrix  # noqa: E402

SIGNING = {
    "key": "none",
    "fingerprint": None,
    "statement": ("NuttX images (the NuttX agent) are not signed: MCUboot "
                  "checks each image's SHA-256 hash, not who built it. Use "
                  "MCUboot with a signing key for production devices."),
}

FEATURES = ("TELEMETRY", "HEALTH", "RESTART", "FIRMWARE_UPDATE",
            "REMOTE_ACCESS", "SHELL_COMMAND", "LOG_UPLOAD", "COREDUMP",
            "PARAMETERS", "CERT_RENEWAL")

README = """{name}
{rule}

{device_name} — {app} ({variant}, tiny-edge's NuttX agent), version {version}

Flash everything (first flash; erases the board, including the stored Wi-Fi
network and Cumulocity certificate):

  esptool --chip {chip} --before {before} erase-flash
  esptool --chip {chip} --before {before} write-flash {writes}

or, from a checkout of the repository:

  scripts/flash.sh {name} --erase-all

or, without any tools, the single image {stem}.factory.bin at 0x0 from a
browser flasher such as https://web.esphome.io/ (Chrome or Edge).

Replace only the application (keeps the Wi-Fi network and the certificate):

  esptool --chip {chip} --before {before} write-flash {app_off} app.signed.bin

Then set it up over USB, with the board's USB port (on a DevKitC, the
connector labelled USB) plugged into a computer. On that computer, install
tedge-onboard as a go-c8y-cli extension and run it:

  c8y extensions install thin-edge/tiny-edge
  c8y tinyedge onboard

or download it from https://github.com/thin-edge/tiny-edge/releases and run
./tedge-onboard. It gives the device the computer's Wi-Fi network, registers
it in your Cumulocity tenant, and passes it the tenant and a one-time
password.

Updates from Cumulocity install {stem}.app.bin, which MCUboot boots once on
trial: the agent confirms it once it has connected, or MCUboot goes back to
the image before.

{signing}
"""


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def read_config(path):
    """CONFIG_X=value -> {X: value}; strings unquoted, y -> True."""
    cfg = {}
    for line in Path(path).read_text().splitlines():
        m = re.match(r"^CONFIG_(\w+)=(.*)$", line)
        if not m:
            continue
        v = m.group(2)
        if v == "y":
            v = True
        elif len(v) >= 2 and v[0] == v[-1] == '"':
            v = v[1:-1]
        cfg[m.group(1)] = v
    return cfg


def option(cfg, name):
    """An MCUboot layout option, named CONFIG_ESPRESSIF_* on the RISC-V ESP
    chips and CONFIG_ESP32S3_* on the ESP32-S3."""
    for prefix in ("ESPRESSIF_", "ESP32S3_"):
        if prefix + name in cfg:
            return int(str(cfg[prefix + name]), 0)
    raise SystemExit(f"the NuttX configuration has no {name} (built without "
                     "MCUboot?)")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("build")
    ap.add_argument("--config", required=True, help="the build's NuttX .config")
    ap.add_argument("--device", required=True)
    ap.add_argument("--app", required=True)
    ap.add_argument("--variant", required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--tiny-edge", required=True, help="the tiny-edge checkout")
    ap.add_argument("--out", required=True)
    ap.add_argument("--manifest", default=matrix.DEFAULT_MANIFEST)
    ap.add_argument("--esptool", default="esptool")
    # The agent with every feature takes about 92 % of its 1.5 MB slot; the
    # slot is as large as the 4 MB layout allows (tiny-edge's NuttX README).
    ap.add_argument("--max-slot", type=float, default=95.0,
                    help="largest share of the slot the update image may take")
    args = ap.parse_args(argv)

    manifest, builds = matrix.load(args.manifest)
    build = next((b for b in builds if b.device["id"] == args.device and
                  b.app == args.app and b.variant == args.variant), None)
    if build is None or build.framework != "nuttx":
        print(f"{args.device} {args.app} {args.variant} is not a NuttX build "
              "in the manifest", file=sys.stderr)
        return 1
    dev = build.device
    bdir = Path(args.build)
    cfg = read_config(args.config)

    chip = cfg.get("ARCH_CHIP", "")
    if chip != dev["chip"]:
        print(f"the build is for {chip!r}, the device is {dev['chip']}",
              file=sys.stderr)
        return 1
    if cfg.get("TINY_EDGE_FIRMWARE_NAME") != build.firmware_name:
        print(f"the image reports firmware name "
              f"{cfg.get('TINY_EDGE_FIRMWARE_NAME')!r}, expected "
              f"{build.firmware_name!r}", file=sys.stderr)
        return 1
    if cfg.get("TINY_EDGE_FIRMWARE_VERSION") != args.version:
        print(f"the image reports version "
              f"{cfg.get('TINY_EDGE_FIRMWARE_VERSION')!r}, expected "
              f"{args.version!r}", file=sys.stderr)
        return 1
    # No release image carries site settings.
    if cfg.get("TEDGE_C8Y_URL"):
        print("CONFIG_TEDGE_C8Y_URL is set in this build: release images carry "
              "no tenant", file=sys.stderr)
        return 1

    slot0 = option(cfg, "OTA_PRIMARY_SLOT_OFFSET")
    slot1 = option(cfg, "OTA_SECONDARY_SLOT_OFFSET")
    slot = option(cfg, "OTA_SLOT_SIZE")
    scratch = option(cfg, "OTA_SCRATCH_OFFSET")
    parts = {
        "boot": {"offset": 0, "size": slot0},
        "slot0": {"offset": slot0, "size": slot},
        "slot1": {"offset": slot1, "size": slot},
        "scratch": {"offset": scratch, "size": option(cfg, "OTA_SCRATCH_SIZE")},
        "data": {"offset": option(cfg, "STORAGE_MTD_OFFSET"),
                 "size": option(cfg, "STORAGE_MTD_SIZE")},
    }

    mcuboot = bdir / f"mcuboot-{chip}.bin"
    confirmed = bdir / "nuttx.bin"
    update = bdir / "nuttx-update.bin"
    for f in (mcuboot, confirmed, update):
        if not f.is_file():
            print(f"{f} is missing: not a tiny-edge NuttX build with MCUboot",
                  file=sys.stderr)
            return 1
    update_size = update.stat().st_size
    percent = round(100.0 * update_size / slot, 1)
    if percent > args.max_slot:
        print(f"the update image is {update_size} B, {percent}% of its "
              f"{slot} B slot (at most {args.max_slot}%)", file=sys.stderr)
        return 1

    stem = f"{build.firmware_name}-{args.version}"
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    images = [("mcuboot", "mcuboot.bin", mcuboot, 0),
              ("app", "app.signed.bin", confirmed, slot0)]

    factory = out / f"{stem}.factory.bin"
    cmd = [args.esptool, "--chip", chip, "merge-bin", "--output", str(factory),
           "--format", "raw"]
    for _, _, src, off in images:
        cmd += [f"{off:#x}", str(src)]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)

    app_bin = out / f"{stem}.app.bin"
    shutil.copyfile(update, app_bin)

    before = dev.get("esptool_before", "default-reset")
    flash = {
        "schema": 1,
        "framework": "nuttx",
        "device": dev["id"],
        "device_name": dev["name"],
        "board": dev["nuttx_board"],
        "app": build.name,
        "variant": args.variant,
        "version": args.version,
        "firmware_name": build.firmware_name,
        "chip": chip,
        "flash_size": dev["flash_size"],
        "esptool_before": before,
        "flash_args": [],
        "images": [{"name": n, "file": f, "offset": f"{off:#x}"}
                   for n, f, _, off in images],
        "partitions": {n: {"offset": f"{p['offset']:#x}", "size": f"{p['size']:#x}"}
                       for n, p in parts.items()},
        "signing": SIGNING,
    }
    readme = README.format(
        name=stem, rule="=" * len(stem), stem=stem, device_name=dev["name"],
        app=build.name, variant=args.variant, version=args.version, chip=chip,
        before=before, app_off=f"{slot0:#x}",
        writes=" ".join(f"{off:#x} {f}" for _, f, _, off in images),
        signing=SIGNING["statement"])

    bundle = out / f"{stem}.zip"
    with zipfile.ZipFile(bundle, "w", zipfile.ZIP_DEFLATED) as z:
        for _, f, src, _ in images:
            z.write(src, f"{stem}/{f}")
        z.writestr(f"{stem}/flash.json", json.dumps(flash, indent=2) + "\n")
        z.writestr(f"{stem}/README.txt", readme)

    tiny_edge = Path(args.tiny_edge) / "VERSION"
    meta = {
        **{k: flash[k] for k in ("device", "device_name", "board", "app",
                                 "variant", "version", "firmware_name", "chip",
                                 "flash_size")},
        "framework": "nuttx",
        "stem": stem,
        "tedge": True,
        # tedge-onboard registers the device with the type the image reports,
        # so the firmware repository entry reaches exactly these devices.
        "device_type": cfg.get("TEDGE_DEVICE_TYPE") or None,
        "features": [k.lower().replace("_", "-") for k in FEATURES
                     if cfg.get(f"TEDGE_{k}") is True],
        "tedge_zephyr_version": (tiny_edge.read_text().strip()
                                 if tiny_edge.is_file() else None),
        "measured": build.measured,
        "signing": SIGNING,
        "assets": {p.name: sha256(p) for p in (factory, app_bin, bundle)},
    }
    (out / f"{stem}.meta.json").write_text(json.dumps(meta, indent=2) + "\n")
    size = {"image": update_size, "slot0": slot, "slot0_percent": percent,
            "ram": []}
    (out / f"{stem}.size.json").write_text(json.dumps(size, indent=2) + "\n")
    for p in (factory, app_bin, bundle):
        print(p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
