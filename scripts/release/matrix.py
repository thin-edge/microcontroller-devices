#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Validate release/devices.yml and turn it into the release build matrix.

    scripts/release/matrix.py                 # GitHub matrix JSON on stdout
    scripts/release/matrix.py --github-output # "matrix=<json>" for $GITHUB_OUTPUT
    scripts/release/matrix.py --markdown      # supported-devices table
    scripts/release/matrix.py --entry NAME    # one build, as shell variables

Every problem found is reported, each naming the device and build it is
in, and the exit status is non-zero; nothing is printed on stdout then, so a
workflow step cannot build from a half-valid manifest.

Also imported by the other release scripts (load(), builds()).
"""

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MANIFEST = ROOT / "release" / "devices.yml"

# tedge_identity's firmware_name buffer in tedge-zephyr is 48 bytes.
FIRMWARE_NAME_MAX = 47
ID_RE = re.compile(r"^[a-z0-9]+(-[a-z0-9]+)*$")
FLASH_RE = re.compile(r"^(1|2|4|8|16|32)MB$")
ESPTOOL_BEFORE = {"default-reset", "usb-reset", "no-reset"}


class ManifestError(Exception):
    def __init__(self, problems):
        super().__init__("\n".join(problems))
        self.problems = problems


@dataclass
class Build:
    device: dict
    app: str
    variant: str
    variant_def: dict
    extra_conf: list = field(default_factory=list)
    measured: str = ""

    @property
    def tedge(self):
        return self.variant != "standalone"

    @property
    def firmware_name(self):
        return f"{self.app}-{self.variant}-{self.device['id']}"

    def conf_files(self):
        """EXTRA_CONF_FILE for the application, repository-relative."""
        files = []
        if self.variant_def.get("profile"):
            files.append(self.variant_def["profile"])
        if self.tedge:
            files.append(self.device["tedge_board_conf"])
        files += self.extra_conf
        return files

    def matrix_entry(self, manifest):
        prov = manifest["provisioners"][self.variant_def["provisioner"]]
        return {
            "device": self.device["id"],
            "device_name": self.device["name"],
            "board": self.device["board"],
            "chip": self.device["chip"],
            "flash_size": self.device["flash_size"],
            "esptool_before": self.device.get("esptool_before", "default-reset"),
            "app": self.app,
            "variant": self.variant,
            "firmware_name": self.firmware_name,
            "conf_files": ";".join(self.conf_files()),
            "dtc_overlays": ";".join(self.device.get("dtc_overlays", [])),
            "provisioner_conf": prov or "",
            "measured": self.measured,
        }


def board_dir_name(board):
    """esp32c6_devkitc/esp32c6/hpcore -> esp32c6_devkitc_esp32c6_hpcore"""
    return board.replace("/", "_")


def slug(heading):
    """GitHub's anchor for a Markdown heading."""
    s = heading.strip().lower()
    s = re.sub(r"[^\w\- ]", "", s)
    return s.replace(" ", "-")


def anchors(path):
    return {
        slug(m.group(1))
        for m in re.finditer(r"^#{1,6}\s+(.*)$", path.read_text(), re.M)
    }


def load(path=DEFAULT_MANIFEST, root=ROOT):
    """Parse and validate; return (manifest, [Build]) or raise ManifestError."""
    path, root = Path(path), Path(root)
    try:
        manifest = yaml.safe_load(path.read_text())
    except (OSError, yaml.YAMLError) as e:
        raise ManifestError([f"{path}: {e}"])
    problems = []

    def need_file(where, rel):
        if not (root / rel).is_file():
            problems.append(f"{where}: {rel} does not exist")

    variants = manifest.get("variants") or {}
    provisioners = manifest.get("provisioners") or {}
    for name, v in variants.items():
        if v.get("provisioner") not in provisioners:
            problems.append(f"variant {name}: unknown provisioner {v.get('provisioner')!r}")
        if v.get("profile"):
            need_file(f"variant {name}", v["profile"])
        if name != "standalone" and not v.get("profile"):
            problems.append(f"variant {name}: a tedge variant needs a profile")
    for name, conf in provisioners.items():
        if conf:
            need_file(f"provisioner {name}", conf)

    layout_boards = set(
        re.findall(r'_board STREQUAL "([^"]+)"',
                   (root / "sysbuild" / "provisioning.cmake").read_text()))

    builds, seen_ids, seen_builds = [], set(), set()
    doc_anchors = {}
    for dev in manifest.get("devices") or []:
        did = dev.get("id", "?")
        where = f"device {did}"
        if not ID_RE.match(did):
            problems.append(f"{where}: id must be lowercase letters, digits and dashes")
        if did in seen_ids:
            problems.append(f"{where}: duplicate device id")
        seen_ids.add(did)
        for key in ("name", "board", "chip", "flash_size"):
            if not dev.get(key):
                problems.append(f"{where}: missing {key}")
        board = dev.get("board", "")
        if board and board not in layout_boards:
            problems.append(f"{where}: board {board} has no provisioner layout "
                            "in sysbuild/provisioning.cmake")
        if dev.get("flash_size") and not FLASH_RE.match(dev["flash_size"]):
            problems.append(f"{where}: flash_size must look like 4MB")
        if dev.get("esptool_before", "default-reset") not in ESPTOOL_BEFORE:
            problems.append(f"{where}: esptool_before must be one of "
                            f"{', '.join(sorted(ESPTOOL_BEFORE))}")
        for ov in dev.get("dtc_overlays", []):
            need_file(where, ov)
        if dev.get("tedge_board_conf"):
            need_file(where, dev["tedge_board_conf"])

        for b in dev.get("builds") or []:
            app, variant = b.get("app", "?"), b.get("variant", "?")
            bwhere = f"{where}, {app} {variant}"
            if (did, app, variant) in seen_builds:
                problems.append(f"{bwhere}: listed twice")
            seen_builds.add((did, app, variant))
            if not (root / "apps" / app / "CMakeLists.txt").is_file():
                problems.append(f"{bwhere}: there is no apps/{app}")
            elif board and not (root / "apps" / app / "boards" /
                                f"{board_dir_name(board)}.conf").is_file():
                problems.append(f"{bwhere}: apps/{app}/boards/"
                                f"{board_dir_name(board)}.conf does not exist")
            if variant not in variants:
                problems.append(f"{bwhere}: unknown variant")
                continue
            for conf in b.get("extra_conf", []):
                need_file(bwhere, conf)
            build = Build(dev, app, variant, variants[variant],
                          list(b.get("extra_conf", [])), b.get("measured", ""))
            if build.tedge:
                if not dev.get("tedge_board_conf"):
                    problems.append(f"{bwhere}: the device has no tedge_board_conf")
                if not build.measured:
                    problems.append(f"{bwhere}: a tedge build needs `measured`, "
                                    "the DEVICES.md entry recording its run on the board")
                else:
                    doc, _, anchor = build.measured.partition("#")
                    if not (root / doc).is_file():
                        problems.append(f"{bwhere}: measured: {doc} does not exist")
                    elif anchor:
                        if doc not in doc_anchors:
                            doc_anchors[doc] = anchors(root / doc)
                        if anchor not in doc_anchors[doc]:
                            problems.append(f"{bwhere}: measured: {doc} has no "
                                            f"heading #{anchor}")
            if len(build.firmware_name) > FIRMWARE_NAME_MAX:
                problems.append(f"{bwhere}: firmware name {build.firmware_name} is "
                                f"{len(build.firmware_name)} characters "
                                f"(at most {FIRMWARE_NAME_MAX})")
            builds.append(build)

    if not builds and not problems:
        problems.append(f"{path}: no builds")
    if problems:
        raise ManifestError(problems)
    return manifest, builds


def markdown(manifest, builds):
    rows = ["| Device | Zephyr board | standalone | tedge-full | tedge-ota |",
            "|---|---|---|---|---|"]
    for dev in manifest["devices"]:
        cells = []
        for variant in ("standalone", "tedge-full", "tedge-ota"):
            apps = [b.app for b in builds
                    if b.device is dev and b.variant == variant]
            cells.append(", ".join(f"`{a}`" for a in apps) or "—")
        rows.append(f"| {dev['name']} | `{dev['board']}` | " + " | ".join(cells) + " |")
    return "\n".join(rows)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--manifest", default=DEFAULT_MANIFEST)
    ap.add_argument("--root", default=ROOT)
    out = ap.add_mutually_exclusive_group()
    out.add_argument("--github-output", action="store_true")
    out.add_argument("--markdown", action="store_true")
    out.add_argument("--entry", metavar="FIRMWARE_NAME",
                     help="print one build's matrix entry as shell variables")
    args = ap.parse_args(argv)
    try:
        manifest, builds = load(args.manifest, args.root)
    except ManifestError as e:
        for p in e.problems:
            print(f"error: {p}", file=sys.stderr)
        return 1
    if args.markdown:
        print(markdown(manifest, builds))
        return 0
    if args.entry:
        build = next((b for b in builds if b.firmware_name == args.entry), None)
        if build is None:
            print(f"error: no build named {args.entry}; one of:", file=sys.stderr)
            for b in builds:
                print(f"  {b.firmware_name}", file=sys.stderr)
            return 1
        for k, v in build.matrix_entry(manifest).items():
            print(f"{k.upper()}='{v}'")
        return 0
    matrix = json.dumps({"include": [b.matrix_entry(manifest) for b in builds]})
    print(f"matrix={matrix}" if args.github_output else matrix)
    return 0


if __name__ == "__main__":
    sys.exit(main())
