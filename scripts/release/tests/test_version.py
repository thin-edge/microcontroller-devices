# SPDX-License-Identifier: Apache-2.0
"""Tests for the shared application version: apps/*/VERSION, bump.sh and the
release-please manifest.

    python3 -m unittest discover -s scripts/release/tests
"""

import json
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
MANIFEST = ".release-please-manifest.json"
CONFIG = "release-please-config.json"

# The line and the marker release-please's generic updater looks for on it.
MARKED = {
    "VERSION_MAJOR": "x-release-please-major",
    "VERSION_MINOR": "x-release-please-minor",
    "PATCHLEVEL": "x-release-please-patch",
}


def version_of(path):
    text = path.read_text()
    nums = [re.search(rf"^{k} = ([0-9]+)", text, re.M) for k in MARKED]
    return ".".join(m.group(1) for m in nums if m)


class VersionTest(unittest.TestCase):
    def test_every_version_file_carries_the_markers(self):
        files = sorted(ROOT.glob("apps/*/VERSION"))
        self.assertTrue(files)
        for f in files:
            text = f.read_text()
            for key, marker in MARKED.items():
                with self.subTest(file=str(f.relative_to(ROOT)), key=key):
                    self.assertRegex(text, rf"(?m)^{key} = [0-9]+ # {marker}$")

    def test_manifest_matches_the_files(self):
        want = json.loads((ROOT / MANIFEST).read_text())["."]
        for f in sorted(ROOT.glob("apps/*/VERSION")):
            with self.subTest(file=str(f.relative_to(ROOT))):
                self.assertEqual(version_of(f), want)

    def test_config_updates_every_version_file(self):
        config = json.loads((ROOT / CONFIG).read_text())
        extra = config["packages"]["."]["extra-files"]
        paths = {e["path"] for e in extra if e.get("type") == "generic"}
        want = {str(f.relative_to(ROOT)) for f in ROOT.glob("apps/*/VERSION")}
        self.assertEqual(paths, want)

    def test_bump_round_trips(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            for f in ROOT.glob("apps/*/VERSION"):
                dst = tmp / f.relative_to(ROOT)
                dst.parent.mkdir(parents=True)
                shutil.copy(f, dst)
            (tmp / "scripts/release").mkdir(parents=True)
            shutil.copy(ROOT / "scripts/release/bump.sh", tmp / "scripts/release")
            subprocess.run([tmp / "scripts/release/bump.sh", "1.2.3"],
                           check=True, capture_output=True)
            for f in sorted(tmp.glob("apps/*/VERSION")):
                text = f.read_text()
                self.assertEqual(version_of(f), "1.2.3")
                for key, marker in MARKED.items():
                    self.assertRegex(text, rf"(?m)^{key} = [0-9]+ # {marker}$")
                self.assertRegex(text, r"(?m)^EXTRAVERSION =$")
            self.assertEqual(json.loads((tmp / MANIFEST).read_text()), {".": "1.2.3"})


if __name__ == "__main__":
    unittest.main()
