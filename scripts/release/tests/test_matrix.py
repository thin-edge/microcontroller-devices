# SPDX-License-Identifier: Apache-2.0
"""Tests for scripts/release/matrix.py.

    python3 -m unittest discover -s scripts/release/tests
"""

import json
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import matrix  # noqa: E402

BOARD = "esp32c6_devkitc/esp32c6/hpcore"

VALID = """
variants:
  standalone: {provisioner: improv}
  tedge-ota: {profile: profiles/ota.conf, provisioner: ztp}
provisioners:
  improv: null
  ztp: prov/ztp.conf
devices:
  - id: c6
    name: C6
    board: esp32c6_devkitc/esp32c6/hpcore
    chip: esp32c6
    flash_size: 4MB
    tedge_board_conf: boards/c6.conf
    builds:
      - {app: modbus-server, variant: standalone, pr: true}
      - app: modbus-server
        variant: tedge-ota
        measured: DEVICES.md#measured-on-the-c6
"""


class MatrixTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        for rel in ("profiles/ota.conf", "prov/ztp.conf", "boards/c6.conf",
                    "apps/modbus-server/CMakeLists.txt",
                    f"apps/modbus-server/boards/{matrix.board_dir_name(BOARD)}.conf"):
            (self.root / rel).parent.mkdir(parents=True, exist_ok=True)
            (self.root / rel).write_text("")
        (self.root / "sysbuild").mkdir()
        (self.root / "sysbuild/provisioning.cmake").write_text(
            f'if(_board STREQUAL "{BOARD}")\n')
        (self.root / "DEVICES.md").write_text("# Fleet\n\n## Measured on the C6\n")

    def tearDown(self):
        self.tmp.cleanup()

    def load(self, text):
        path = self.root / "devices.yml"
        path.write_text(textwrap.dedent(text))
        return matrix.load(path, self.root)

    def problems(self, text):
        with self.assertRaises(matrix.ManifestError) as ctx:
            self.load(text)
        return "\n".join(ctx.exception.problems)

    def test_valid_manifest(self):
        manifest, builds = self.load(VALID)
        entries = [b.matrix_entry(manifest) for b in builds]
        self.assertEqual(len(entries), 2)
        std, ota = entries
        self.assertEqual(std["conf_files"], "")
        self.assertEqual(std["provisioner_conf"], "")
        self.assertEqual(ota["conf_files"], "profiles/ota.conf;boards/c6.conf")
        self.assertEqual(ota["provisioner_conf"], "prov/ztp.conf")
        self.assertEqual(ota["firmware_name"], "modbus-server-tedge-ota-c6")
        json.dumps({"include": entries})  # serialisable

    def test_unknown_app(self):
        p = self.problems(VALID.replace("app: modbus-server\n", "app: nope\n"))
        self.assertIn("device c6, nope tedge-ota: there is no apps/nope", p)

    def test_missing_overlay(self):
        p = self.problems(VALID.replace("boards/c6.conf", "boards/gone.conf"))
        self.assertIn("device c6: boards/gone.conf does not exist", p)

    def test_board_without_layout(self):
        p = self.problems(VALID.replace("board: esp32c6_devkitc/esp32c6/hpcore",
                                        "board: esp32s2_saola/esp32s2"))
        self.assertIn("device c6: board esp32s2_saola/esp32s2 has no provisioner layout", p)

    def test_duplicate_device_id(self):
        dev = VALID[VALID.index("  - id: c6"):]
        p = self.problems(VALID + dev)
        self.assertIn("device c6: duplicate device id", p)

    def test_unknown_variant(self):
        p = self.problems(VALID.replace("variant: standalone", "variant: tedge-max"))
        self.assertIn("device c6, modbus-server tedge-max: unknown variant", p)

    def test_tedge_build_must_be_measured(self):
        p = self.problems(VALID.replace("        measured: DEVICES.md#measured-on-the-c6\n", ""))
        self.assertIn("device c6, modbus-server tedge-ota: a tedge build needs `measured`", p)

    def test_measured_anchor_must_exist(self):
        p = self.problems(VALID.replace("#measured-on-the-c6", "#never-measured"))
        self.assertIn("measured: DEVICES.md has no heading #never-measured", p)

    def test_firmware_name_length(self):
        long_id = "c" + "x" * 30
        p = self.problems(VALID.replace("id: c6", f"id: {long_id}"))
        self.assertIn("characters (at most 47)", p)

    def test_every_problem_is_reported(self):
        text = VALID.replace("boards/c6.conf", "boards/gone.conf").replace(
            "variant: standalone", "variant: tedge-max")
        p = self.problems(text)
        self.assertIn("boards/gone.conf does not exist", p)
        self.assertIn("unknown variant", p)

    def test_main_prints_nothing_on_error(self):
        path = self.root / "devices.yml"
        path.write_text(VALID.replace("variant: standalone", "variant: tedge-max"))
        from io import StringIO
        from contextlib import redirect_stdout, redirect_stderr
        out, err = StringIO(), StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            rc = matrix.main(["--manifest", str(path), "--root", str(self.root)])
        self.assertEqual(rc, 1)
        self.assertEqual(out.getvalue(), "")
        self.assertIn("error: device c6", err.getvalue())

    def test_one_job_per_device_and_app(self):
        manifest, builds = self.load(VALID)
        groups = matrix.groups(builds)
        self.assertEqual(len(groups), 1)
        self.assertEqual(groups[0]["group"], "modbus-server-c6")
        self.assertEqual(groups[0]["chip"], "esp32c6")
        self.assertEqual(groups[0]["builds"],
                         "modbus-server-standalone-c6 modbus-server-tedge-ota-c6")

    def test_pr_subset(self):
        path = self.root / "devices.yml"
        path.write_text(textwrap.dedent(VALID))
        from io import StringIO
        from contextlib import redirect_stdout
        out = StringIO()
        with redirect_stdout(out):
            rc = matrix.main(["--manifest", str(path), "--root", str(self.root),
                              "--subset", "pr"])
        self.assertEqual(rc, 0)
        jobs = json.loads(out.getvalue())["include"]
        self.assertEqual([j["builds"] for j in jobs], ["modbus-server-standalone-c6"])

    def test_every_device_needs_a_pr_build(self):
        p = self.problems(VALID.replace(", pr: true", ""))
        self.assertIn("device c6: no build is marked `pr: true`", p)

    def test_pr_must_be_boolean(self):
        p = self.problems(VALID.replace("pr: true", "pr: yes please"))
        self.assertIn("pr must be true or false", p)

    def test_repository_manifest_is_valid(self):
        manifest, builds = matrix.load()
        self.assertTrue(builds)

    def test_readme_table_is_current(self):
        """The README's device table is generated; regenerate it with
        scripts/release/matrix.py --markdown when the manifest changes."""
        manifest, builds = matrix.load()
        readme = (matrix.ROOT / "README.md").read_text()
        block = readme.split("<!-- devices:start", 1)[1].split("-->", 1)[1]
        block = block.split("<!-- devices:end -->", 1)[0].strip()
        self.assertEqual(block, matrix.markdown(manifest, builds))


if __name__ == "__main__":
    unittest.main()
