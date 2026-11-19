# SPDX-License-Identifier: Apache-2.0
"""Tests for scripts/release/matrix.py.

    python3 -m unittest discover -s scripts/release/tests
"""

import json
import os
import sys
import tempfile
import textwrap
import unittest
import unittest.mock
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import matrix  # noqa: E402

BOARD = "esp32c6_devkitc/esp32c6/hpcore"

VALID = """
variants:
  zephyr-standalone: {set: standalone, provisioner: improv}
  zephyr-ota: {set: ota, profile: profiles/ota.conf, provisioner: ztp}
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
      - {app: modbus-server, variant: zephyr-standalone, pr: true}
      - app: modbus-server
        variant: zephyr-ota
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
        self.assertEqual(ota["firmware_name"], "modbus-server-zephyr-ota-c6")
        json.dumps({"include": entries})  # serialisable

    def test_west_project_paths(self):
        """"{project}/..." resolves through west, and is skipped without it."""
        text = VALID.replace("profiles/ota.conf", '"{tiny-edge}/profiles/ota.conf"')
        project = self.root / "te"
        (project / "profiles").mkdir(parents=True)
        (project / "profiles/ota.conf").write_text("")
        try:
            matrix.west_project_dir.cache_clear()
            orig = matrix.west_project_dir
            # Found: the file is checked where west says the project is.
            matrix.west_project_dir = lambda name: project if name == "tiny-edge" else None
            manifest, builds = self.load(text)
            self.assertEqual(matrix.resolve(self.root, builds[1].conf_files()[0]),
                             project / "profiles/ota.conf")
            (project / "profiles/ota.conf").unlink()
            self.assertIn("{tiny-edge}/profiles/ota.conf does not exist",
                          self.problems(text))
            # No west (the matrix job): not checked, and kept as written.
            matrix.west_project_dir = lambda name: None
            manifest, builds = self.load(text)
            self.assertEqual(builds[1].matrix_entry(manifest)["conf_files"],
                             "{tiny-edge}/profiles/ota.conf;boards/c6.conf")
        finally:
            matrix.west_project_dir = orig

    def test_unknown_app(self):
        p = self.problems(VALID.replace("app: modbus-server\n", "app: nope\n"))
        self.assertIn("device c6, nope zephyr-ota: there is no apps/nope", p)

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
        p = self.problems(VALID.replace("variant: zephyr-standalone", "variant: zephyr-max"))
        self.assertIn("device c6, modbus-server zephyr-max: unknown variant", p)

    def test_tedge_build_must_be_measured(self):
        p = self.problems(VALID.replace("        measured: DEVICES.md#measured-on-the-c6\n", ""))
        self.assertIn("device c6, modbus-server zephyr-ota: a tedge build needs `measured`", p)

    def test_measured_anchor_must_exist(self):
        p = self.problems(VALID.replace("#measured-on-the-c6", "#never-measured"))
        self.assertIn("measured: DEVICES.md has no heading #never-measured", p)

    def test_firmware_name_length(self):
        long_id = "c" + "x" * 30
        p = self.problems(VALID.replace("id: c6", f"id: {long_id}"))
        self.assertIn("characters (at most 47)", p)

    def test_every_problem_is_reported(self):
        text = VALID.replace("boards/c6.conf", "boards/gone.conf").replace(
            "variant: zephyr-standalone", "variant: zephyr-max")
        p = self.problems(text)
        self.assertIn("boards/gone.conf does not exist", p)
        self.assertIn("unknown variant", p)

    def test_main_prints_nothing_on_error(self):
        path = self.root / "devices.yml"
        path.write_text(VALID.replace("variant: zephyr-standalone", "variant: zephyr-max"))
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
        self.assertEqual(groups[0]["group"], "modbus-server-zephyr-c6")
        self.assertEqual(groups[0]["chip"], "esp32c6")
        self.assertEqual(groups[0]["builds"],
                         "modbus-server-zephyr-standalone-c6 "
                         "modbus-server-zephyr-ota-c6")

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
        self.assertEqual([j["builds"] for j in jobs], ["modbus-server-zephyr-standalone-c6"])

    def test_every_device_needs_a_pr_build(self):
        p = self.problems(VALID.replace(", pr: true", ""))
        self.assertIn("device c6: no build is marked `pr: true`", p)

    def test_pr_must_be_boolean(self):
        p = self.problems(VALID.replace("pr: true", "pr: yes please"))
        self.assertIn("pr must be true or false", p)

    IDF = VALID.replace("""  zephyr-ota: {set: ota, profile: profiles/ota.conf, provisioner: ztp}
""", """  zephyr-ota: {set: ota, profile: profiles/ota.conf, provisioner: ztp}
  idf-full: {framework: esp-idf, set: full, provisioner: onboard}
""").replace("""  ztp: prov/ztp.conf
""", """  ztp: prov/ztp.conf
  onboard: null
""") + """      - app: agent-idf
        variant: idf-full
        measured: DEVICES.md#measured-on-the-c6
"""

    def idf_app(self, defaults=True):
        app = self.root / "apps" / "agent-idf"
        app.mkdir(parents=True)
        (app / "CMakeLists.txt").write_text("")
        if defaults:
            (app / "sdkconfig.defaults").write_text("")

    def test_esp_idf_build(self):
        """An ESP-IDF build needs no Zephyr board files or profile, and
        lands in a matrix of its own."""
        self.idf_app()
        manifest, builds = self.load(self.IDF)
        idf = [b for b in builds if b.app == "agent-idf"][0]
        entry = idf.matrix_entry(manifest)
        self.assertEqual(entry["framework"], "esp-idf")
        self.assertEqual(entry["conf_files"], "")
        self.assertEqual(entry["firmware_name"], "agent-idf-idf-full-c6")

        from contextlib import redirect_stdout
        import io
        path = self.root / "devices.yml"
        out = io.StringIO()
        with redirect_stdout(out):
            rc = matrix.main(["--manifest", str(path), "--root", str(self.root),
                              "--github-output"])
        self.assertEqual(rc, 0)
        lines = dict(l.split("=", 1) for l in out.getvalue().splitlines())
        zephyr = json.loads(lines["matrix"])["include"]
        idf_jobs = json.loads(lines["idf_matrix"])["include"]
        self.assertEqual([j["app"] for j in zephyr], ["modbus-server"])
        self.assertEqual([j["builds"] for j in idf_jobs],
                         ["agent-idf-idf-full-c6"])

    def test_app_name(self):
        """The `apps` section names an app whose directory says more; the
        image name and the job use it, the build its directory."""
        self.idf_app()
        text = self.IDF.replace("provisioners:", "apps:\n  agent-idf: {name: agent}\nprovisioners:")
        manifest, builds = self.load(text)
        idf = [b for b in builds if b.app == "agent-idf"][0]
        entry = idf.matrix_entry(manifest)
        self.assertEqual(entry["firmware_name"], "agent-idf-full-c6")
        self.assertEqual(entry["app"], "agent-idf")
        self.assertEqual(matrix.groups([idf])[0]["group"], "agent-idf-c6")

    def test_app_name_needs_the_app(self):
        p = self.problems(VALID.replace("provisioners:", "apps:\n  gone: {name: x}\nprovisioners:"))
        self.assertIn("apps: there is no apps/gone", p)

    def test_variant_name_follows_framework_and_set(self):
        self.idf_app()
        p = self.problems(self.IDF.replace("idf-full: {framework: esp-idf",
                                           "zephyr-full: {framework: esp-idf")
                                  .replace("variant: idf-full", "variant: zephyr-full"))
        self.assertIn("variant zephyr-full: a esp-idf variant with set full is "
                      "named idf-full", p)
        p = self.problems(VALID.replace("set: ota", "set: max"))
        self.assertIn("variant zephyr-ota: set must be one of standalone, full, ota", p)

    def test_firmware_names_are_unique(self):
        """Two app directories with the same name would share images."""
        self.idf_app()
        # The same app name is fine while the variants differ...
        text = self.IDF.replace("provisioners:", "apps:\n  agent-idf: {name: modbus-server}\nprovisioners:")
        manifest, builds = self.load(text)
        names = [b.firmware_name for b in builds]
        self.assertEqual(len(names), len(set(names)))
        # ...but not when an image would carry another's name.
        dup = text + """      - app: agent-idf
        variant: zephyr-ota
        measured: DEVICES.md#measured-on-the-c6
"""
        (self.root / "apps/agent-idf/boards").mkdir()
        (self.root / f"apps/agent-idf/boards/{matrix.board_dir_name(BOARD)}.conf").write_text("")
        p = self.problems(dup)
        self.assertIn("firmware name modbus-server-zephyr-ota-c6 is also", p)

    def test_esp_idf_app_must_be_an_idf_project(self):
        self.idf_app(defaults=False)
        p = self.problems(self.IDF)
        self.assertIn("apps/agent-idf is not an ESP-IDF project", p)

    def test_unknown_framework(self):
        self.idf_app()
        p = self.problems(self.IDF.replace("framework: esp-idf", "framework: threadx"))
        self.assertIn("variant idf-full: framework must be one of", p)

    NUTTX = VALID.replace("""  zephyr-ota: {set: ota, profile: profiles/ota.conf, provisioner: ztp}
""", """  zephyr-ota: {set: ota, profile: profiles/ota.conf, provisioner: ztp}
  nuttx-full: {framework: nuttx, set: full, provisioner: onboard}
""").replace("""  ztp: prov/ztp.conf
""", """  ztp: prov/ztp.conf
  onboard: null
""").replace("""    tedge_board_conf: boards/c6.conf
""", """    tedge_board_conf: boards/c6.conf
    nuttx_board: esp32c6-devkitc
""") + """      - app: modbus-server
        variant: nuttx-full
        extra_conf: [nuttx/psram.conf]
        measured: DEVICES.md#measured-on-the-c6
"""

    def test_nuttx_build(self):
        """A NuttX build passes its fragments and board to tiny-edge's
        build.sh, and lands in a matrix of its own, whose group is not the
        same app's Zephyr group."""
        (self.root / "nuttx").mkdir()
        (self.root / "nuttx/psram.conf").write_text("")
        manifest, builds = self.load(self.NUTTX)
        nx = [b for b in builds if b.variant == "nuttx-full"][0]
        entry = nx.matrix_entry(manifest)
        self.assertEqual(entry["framework"], "nuttx")
        self.assertEqual(entry["nuttx_board"], "esp32c6-devkitc")
        self.assertEqual(entry["conf_files"], "nuttx/psram.conf")
        self.assertEqual(entry["firmware_name"], "modbus-server-nuttx-full-c6")

        from contextlib import redirect_stdout
        import io
        out = io.StringIO()
        with redirect_stdout(out):
            rc = matrix.main(["--manifest", str(self.root / "devices.yml"),
                              "--root", str(self.root), "--github-output"])
        self.assertEqual(rc, 0)
        lines = dict(l.split("=", 1) for l in out.getvalue().splitlines())
        zephyr = json.loads(lines["matrix"])["include"]
        nuttx = json.loads(lines["nuttx_matrix"])["include"]
        self.assertEqual([j["group"] for j in zephyr], ["modbus-server-zephyr-c6"])
        self.assertEqual([j["group"] for j in nuttx], ["modbus-server-nuttx-c6"])
        self.assertEqual([j["builds"] for j in nuttx], ["modbus-server-nuttx-full-c6"])

    def test_nuttx_build_needs_a_board(self):
        (self.root / "nuttx").mkdir()
        (self.root / "nuttx/psram.conf").write_text("")
        p = self.problems(self.NUTTX.replace("    nuttx_board: esp32c6-devkitc\n", ""))
        self.assertIn("a NuttX build needs the device's nuttx_board", p)

    def test_project_dir_from_the_environment(self):
        """Where west is not installed (the NuttX container), <PROJECT>_DIR
        names a west project's directory."""
        matrix.west_project_dir.cache_clear()
        try:
            with unittest.mock.patch.dict(os.environ, {"TINY_EDGE_DIR": "/x/te"}):
                self.assertEqual(matrix.resolve(self.root, "{tiny-edge}/VERSION"),
                                 Path("/x/te/VERSION"))
        finally:
            matrix.west_project_dir.cache_clear()

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
