#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Package one `west build --sysbuild` build as release assets.

    scripts/release/package.py <build-dir> --device <id> --app <app> \\
        --variant <variant> --out <dir>

Writes, with <stem> = <app>-<variant>-<device>-<version>:

  <stem>.factory.bin  MCUboot + application + provisioner merged into one
                      image that is written at 0x0 (esptool, or a browser
                      flasher such as ESPHome Web)
  <stem>.app.bin      the signed application alone: OTA and --app-only
  <stem>.zip          <stem>/ with the three images, flash.json and README.txt
                      (scripts/flash.sh <stem>/ flashes it)
  <stem>.meta.json    what the release notes and c8y-firmware.json are made
                      from; not a release asset

The version is the application's own (APP_VERSION_STRING, set through
scripts/release/check-version.sh). Needs esptool on PATH (merge-bin), and
openssl for the signing key's fingerprint.
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
import offsets  # noqa: E402

ROOT = matrix.ROOT
DEV_KEY_NAME = "root-ec-p256.pem"


def kconfig(app_dir):
    """The build's .config as a dict (strings unquoted)."""
    out = {}
    for line in (app_dir / "zephyr" / ".config").read_text().splitlines():
        m = re.match(r"^(CONFIG_\w+)=(.*)$", line)
        if m:
            v = m.group(2)
            out[m.group(1)] = v[1:-1] if v.startswith('"') else v
    return out


def app_version(app_dir):
    header = (app_dir / "zephyr" / "include" / "generated" / "zephyr" /
              "app_version.h")
    if not header.is_file():  # older layouts
        header = app_dir / "zephyr" / "include" / "generated" / "app_version.h"
    m = re.search(r'#define APP_VERSION_STRING\s+"([^"]+)"', header.read_text())
    return m.group(1)


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def signing(cfg):
    key = cfg.get("CONFIG_MCUBOOT_SIGNATURE_KEY_FILE", "")
    dev = Path(key).name == DEV_KEY_NAME
    fingerprint = None
    try:
        der = subprocess.run(["openssl", "pkey", "-in", key, "-pubout",
                              "-outform", "DER"], check=True,
                             capture_output=True).stdout
        fingerprint = hashlib.sha256(der).hexdigest()
    except (OSError, subprocess.CalledProcessError):
        pass
    if dev:
        statement = ("Signed with MCUboot's public development key "
                     f"({DEV_KEY_NAME}): anyone can sign an image these "
                     "bootloaders accept. Not for production devices.")
    else:
        statement = ("Signed with the repository's release key, SHA-256 "
                     f"fingerprint of the public key {fingerprint}.")
    return {"key": "mcuboot-development" if dev else "release",
            "fingerprint": fingerprint, "statement": statement}


def device_type(app):
    glue = ROOT / "apps" / app / "src" / "tedge_glue.c"
    if not glue.is_file():
        return None
    m = re.search(r'\.type\s*=\s*"([^"]+)"', glue.read_text())
    return m.group(1) if m else None


def features(cfg):
    """tedge-zephyr features compiled in, e.g. ["firmware-update", ...]."""
    skip = {"CONFIG_TEDGE", "CONFIG_TEDGE_TRANSPORT_C8Y",
            "CONFIG_TEDGE_C8Y_MQTT_SERVICE", "CONFIG_TEDGE_AUTH_C8Y_CA"}
    names = []
    for k in ("TELEMETRY", "HEALTH", "RESTART", "FIRMWARE_UPDATE",
              "REMOTE_ACCESS", "SHELL_COMMAND", "LOG_UPLOAD", "COREDUMP",
              "PARAMETERS", "CERT_RENEWAL"):
        key = f"CONFIG_TEDGE_{k}"
        if key not in skip and cfg.get(key) == "y":
            names.append(k.lower().replace("_", "-"))
    return names


README = """{name}
{rule}

{device_name} — {app} ({variant}), version {version}

Flash everything (first flash; erases the board, including stored Wi-Fi and
Cumulocity credentials):

  esptool --chip {chip} --before {before} erase-flash
  esptool --chip {chip} --before {before} write-flash {writes}

or, from a checkout of the repository:

  scripts/flash.sh {name} --erase-all

or, without any tools, the single image {stem}.factory.bin at 0x0 from a
browser flasher such as https://web.esphome.io/ (Chrome or Edge).

Update only the application (keeps credentials and the provisioner):

  esptool --chip {chip} --before {before} write-flash {app_off:#x} app.signed.bin

Then provision it over Bluetooth: {provisioning}

{signing}
"""


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("build")
    ap.add_argument("--device", required=True)
    ap.add_argument("--app", required=True)
    ap.add_argument("--variant", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--manifest", default=matrix.DEFAULT_MANIFEST)
    ap.add_argument("--esptool", default="esptool")
    args = ap.parse_args(argv)

    manifest, builds = matrix.load(args.manifest)
    build = next((b for b in builds if b.device["id"] == args.device and
                  b.app == args.app and b.variant == args.variant), None)
    if build is None:
        print(f"{args.device} {args.app} {args.variant} is not in the manifest",
              file=sys.stderr)
        return 1
    dev = build.device

    info = offsets.read(args.build)
    app_dir = Path(args.build) / info["app"]
    cfg = kconfig(app_dir)
    version = app_version(app_dir)
    if cfg.get("CONFIG_APP_FIRMWARE_NAME") != build.firmware_name:
        print(f"the image reports firmware name "
              f"{cfg.get('CONFIG_APP_FIRMWARE_NAME')!r}, expected "
              f"{build.firmware_name!r} (pass -DCONFIG_APP_FIRMWARE_NAME)",
              file=sys.stderr)
        return 1
    # No release image carries site settings.
    for key in ("CONFIG_APP_WIFI_SSID", "CONFIG_APP_WIFI_PSK", "CONFIG_TEDGE_C8Y_URL"):
        if cfg.get(key):
            print(f"{key} is set in this build: release images carry no "
                  "credentials or tenant", file=sys.stderr)
            return 1

    stem = f"{build.firmware_name}-{version}"
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    imgs = info["images"]
    order = sorted(imgs.items(), key=lambda kv: kv[1]["offset"])
    names = {"mcuboot": "mcuboot.bin", "app": "app.signed.bin",
             "provisioner": "provisioner.signed.bin"}

    # The factory image: every part at its absolute offset from 0x0, gaps
    # filled with 0xFF, nothing past the provisioner (so storage is left
    # alone). Bootloader headers stay as built — what flash.sh writes too.
    factory = out / f"{stem}.factory.bin"
    cmd = [args.esptool, "--chip", info["chip"], "merge-bin",
           "--output", str(factory), "--format", "raw",
           "--flash-mode", "keep", "--flash-freq", "keep", "--flash-size", "keep"]
    for _, img in order:
        cmd += [f"{img['offset']:#x}", img["file"]]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    storage = info["partitions"]["storage"]["offset"]
    if factory.stat().st_size > storage:
        print(f"{factory.name} reaches into storage ({factory.stat().st_size:#x} "
              f"> {storage:#x})", file=sys.stderr)
        return 1

    app_bin = out / f"{stem}.app.bin"
    shutil.copyfile(imgs["app"]["file"], app_bin)

    sign = signing(cfg)
    before = dev.get("esptool_before", "default-reset")
    flash = {
        "schema": 1,
        "device": dev["id"],
        "device_name": dev["name"],
        "board": dev["board"],
        "app": args.app,
        "variant": args.variant,
        "version": version,
        "firmware_name": build.firmware_name,
        "chip": info["chip"],
        "flash_size": dev["flash_size"],
        "esptool_before": before,
        "images": [{"name": n, "file": names[n], "offset": f"{i['offset']:#x}"}
                   for n, i in order],
        "partitions": {n: {"offset": f"{p['offset']:#x}", "size": f"{p['size']:#x}"}
                       for n, p in info["partitions"].items()},
        "signing": sign,
    }
    provisioning = ("Improv Wi-Fi (https://www.improv-wifi.com/)."
                    if not build.tedge else
                    "lab-ztp-provisioner, which also delivers the Cumulocity "
                    "tenant.")
    readme = README.format(
        name=stem, rule="=" * len(stem), stem=stem, device_name=dev["name"],
        app=args.app, variant=args.variant, version=version, chip=info["chip"],
        before=before, app_off=imgs["app"]["offset"],
        writes=" ".join(f"{i['offset']:#x} {names[n]}" for n, i in order),
        provisioning=provisioning, signing=sign["statement"])

    bundle = out / f"{stem}.zip"
    with zipfile.ZipFile(bundle, "w", zipfile.ZIP_DEFLATED) as z:
        for n, i in order:
            z.write(i["file"], f"{stem}/{names[n]}")
        z.writestr(f"{stem}/flash.json", json.dumps(flash, indent=2) + "\n")
        z.writestr(f"{stem}/README.txt", readme)

    meta = {
        **{k: flash[k] for k in ("device", "device_name", "board", "app",
                                 "variant", "version", "firmware_name", "chip",
                                 "flash_size")},
        "stem": stem,
        "tedge": build.tedge,
        "device_type": device_type(args.app) if build.tedge else None,
        "features": features(cfg) if build.tedge else [],
        "tedge_zephyr_version": ((ROOT / "tedge-zephyr" / "VERSION").read_text().strip()
                                 if build.tedge else None),
        "measured": build.measured,
        "signing": sign,
        "assets": {p.name: sha256(p) for p in (factory, app_bin, bundle)},
    }
    (out / f"{stem}.meta.json").write_text(json.dumps(meta, indent=2) + "\n")
    for p in (factory, app_bin, bundle):
        print(p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
