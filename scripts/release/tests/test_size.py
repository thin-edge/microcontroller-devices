# SPDX-License-Identifier: Apache-2.0
"""Tests for scripts/release/size.py: map parsing, the malloc arena, the
baseline comparison and the flash budget.

    python3 -m unittest discover -s scripts/release/tests
"""

import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import size  # noqa: E402

MAP = """
Memory Configuration

Name             Origin             Length             Attributes
mcuboot_hdr      0x00000000         0x00000020         r
FLASH            0x00000080         0x003fff80         r
sram0_0_seg      0x40800000         0x00077610         rw
irom0_0_seg      0x42000000         0x00400000         xr
lp_ram_seg       0x50000000         0x00003cf8         rw
*default*        0x00000000         0xffffffff

Linker script and memory map

                0x4087c610                        _heap_sentry = 0x4087c610
                0x4086da80                        _end = .
"""


def elf32(sections):
    """A minimal little-endian ELF32 whose section headers say `sections`
    ([(addr, size, flags)] or [(name, addr, size, flags)]); enough for
    size.sections(). The last header is the section-name string table."""
    sections = [s if len(s) == 4 else ("", *s) for s in sections]
    shentsize, shnum = 40, len(sections) + 1
    ehsize = 52
    strtab = b"\0"
    name_off = []
    for name, *_ in sections:
        name_off.append(len(strtab))
        strtab += name.encode() + b"\0"
    shoff = ehsize + len(strtab)
    hdr = bytearray(ehsize)
    hdr[0:4] = b"\x7fELF"
    hdr[4] = 1  # ELFCLASS32
    hdr[5] = 1  # little-endian
    struct.pack_into("<I", hdr, 0x20, shoff)
    struct.pack_into("<HHH", hdr, 0x2E, shentsize, shnum, shnum - 1)
    body = bytearray()
    for (name, addr, sz, flags), off in zip(sections, name_off):
        body += struct.pack("<IIIIIIIIII", off, 1, flags, addr, 0, sz, 0, 0, 4, 0)
    body += struct.pack("<IIIIIIIIII", 0, 3, 0, 0, ehsize, len(strtab), 0, 0, 1, 0)
    return bytes(hdr) + strtab + bytes(body)


def report(image=100, ram=None, arena=1000, prov_image=None, prov_ram=None):
    r = {
        "name": "modbus-server-tedge-ota-c6", "app": "modbus-server",
        "image": image, "slot0": 1000, "slot0_percent": image / 10.0,
        "ram": [{"region": k, "used": v, "size": 2 * v, "percent": 50.0}
                for k, v in (ram or {"sram0_0_seg": 500}).items()],
        "arena": {"region": "sram0_0_seg", "start": 0, "end": arena, "size": arena},
    }
    if prov_image is not None:
        r["provisioner"] = {
            "image": prov_image, "prov": 1000, "prov_percent": prov_image / 10.0,
            "ram": [{"region": k, "used": v, "size": 2 * v, "percent": 50.0}
                    for k, v in (prov_ram or {"sram0_0_seg": 300}).items()],
            "arena": {"region": "sram0_0_seg", "start": 0, "end": 5000, "size": 5000},
        }
    return r


class MapTest(unittest.TestCase):
    def test_regions_skip_default_and_flash_kept(self):
        regs = size.regions(MAP)
        self.assertEqual([r[0] for r in regs],
                         ["mcuboot_hdr", "FLASH", "sram0_0_seg", "irom0_0_seg", "lp_ram_seg"])
        self.assertEqual(regs[2], ("sram0_0_seg", 0x40800000, 0x77610))

    def test_heap_symbols(self):
        self.assertEqual(size.heap_symbols(MAP),
                         {"_heap_sentry": 0x4087c610, "_end": 0x4086da80})

    def test_arena_is_end_to_sentry(self):
        a = size.arena(size.heap_symbols(MAP), size.regions(MAP))
        self.assertEqual(a["size"], 0x4087c610 - 0x4086da80)
        self.assertEqual(a["region"], "sram0_0_seg")

    def test_arena_missing_symbols(self):
        self.assertIsNone(size.arena({"_end": 1}, size.regions(MAP)))

    def test_ram_use_only_ram_regions(self):
        regs = size.regions(MAP)
        secs = [(0x40800000, 100), (0x40800100, 50), (0x42000000, 9999),
                (0x50000000, 36), (0x00000080, 5)]
        ram = size.ram_use(secs, regs)
        self.assertEqual([(r["region"], r["used"]) for r in ram],
                         [("sram0_0_seg", 150), ("lp_ram_seg", 36)])

    def test_sections_reads_allocated_only(self):
        with tempfile.TemporaryDirectory() as d:
            elf = Path(d) / "zephyr.elf"
            elf.write_bytes(elf32([(0x40800000, 100, size.SHF_ALLOC),
                                   (0x40800100, 0, size.SHF_ALLOC),
                                   (0x40800200, 7, 0)]))
            self.assertEqual(size.sections(elf), [(0x40800000, 100)])

    def test_named_sections_and_padding(self):
        with tempfile.TemporaryDirectory() as d:
            elf = Path(d) / "zephyr.elf"
            elf.write_bytes(elf32([(".text", 0x42000000, 1000, size.SHF_ALLOC),
                                   (".flash.align_text", 0x11e00, 0xe200, size.SHF_ALLOC),
                                   (".flash.align_rom", 0xaf930, 0x6d0, size.SHF_ALLOC)]))
            named = size.sections(elf, names=True)
            self.assertEqual(named[0], (".text", 0x42000000, 1000))
            self.assertEqual(size.padding(named), 0xe200 + 0x6d0)


class BaselineTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = Path(self.tmp.name) / "size-baseline.json"

    def tearDown(self):
        self.tmp.cleanup()

    def test_write_then_compare_equal(self):
        r = report(prov_image=900)
        size.write_baseline(self.path, r)
        problems, notes = size.compare(r, size.load_baseline(self.path), 256)
        self.assertEqual(problems, [])
        self.assertTrue(any("image: 100 B (baseline 100 B, +0)" in n for n in notes))
        self.assertTrue(any("provisioner image" in n for n in notes))

    def test_write_keeps_other_builds_and_exceptions(self):
        self.path.write_text(json.dumps({
            "other-build": {"image": 1, "ram": {}, "arena": None},
            "modbus-server-tedge-ota-c6": {
                "image": 1, "ram": {}, "arena": None,
                "budget_exceptions": {"provisioner": "96.8 % on 2026-09-22"}},
        }))
        size.write_baseline(self.path, report())
        data = json.loads(self.path.read_text())
        self.assertIn("other-build", data)
        self.assertEqual(data["modbus-server-tedge-ota-c6"]["image"], 100)
        self.assertEqual(data["modbus-server-tedge-ota-c6"]["budget_exceptions"],
                         {"provisioner": "96.8 % on 2026-09-22"})

    def test_ram_growth_past_tolerance_fails(self):
        size.write_baseline(self.path, report(ram={"sram0_0_seg": 500}))
        problems, _ = size.compare(report(ram={"sram0_0_seg": 800}),
                                   size.load_baseline(self.path), 256)
        self.assertEqual(len(problems), 1)
        self.assertIn("modbus-server-tedge-ota-c6: sram0_0_seg grew by 300 B", problems[0])

    def test_growth_within_tolerance_passes(self):
        size.write_baseline(self.path, report(image=100, ram={"sram0_0_seg": 500}))
        problems, _ = size.compare(report(image=300, ram={"sram0_0_seg": 700}),
                                   size.load_baseline(self.path), 256)
        self.assertEqual(problems, [])

    def test_image_growth_fails_and_names_delta(self):
        size.write_baseline(self.path, report(image=100))
        problems, _ = size.compare(report(image=1000), size.load_baseline(self.path), 256)
        self.assertEqual(problems, ["modbus-server-tedge-ota-c6: image grew by 900 B "
                                    "past the baseline (100 -> 1000 B, tolerance 256 B)"])

    def test_provisioner_growth_fails(self):
        size.write_baseline(self.path, report(prov_image=900, prov_ram={"sram0_0_seg": 300}))
        problems, _ = size.compare(report(prov_image=900, prov_ram={"sram0_0_seg": 1300}),
                                   size.load_baseline(self.path), 256)
        self.assertEqual(len(problems), 1)
        self.assertIn("provisioner sram0_0_seg grew by 1000 B", problems[0])

    def test_shrinking_is_a_note_not_a_problem(self):
        size.write_baseline(self.path, report(image=1000, ram={"sram0_0_seg": 900}))
        problems, notes = size.compare(report(image=100, ram={"sram0_0_seg": 500}),
                                       size.load_baseline(self.path), 256)
        self.assertEqual(problems, [])
        self.assertIn("image: 100 B (baseline 1000 B, -900)", notes)
        self.assertIn("sram0_0_seg: 500 B (baseline 900 B, -400)", notes)

    def test_unknown_build_fails(self):
        problems, _ = size.compare(report(), {}, 256)
        self.assertEqual(problems, ["modbus-server-tedge-ota-c6: not in the baseline; "
                                    "add it with --write-baseline"])

    def test_new_region_fails(self):
        size.write_baseline(self.path, report(ram={"sram0_0_seg": 500}))
        problems, _ = size.compare(report(ram={"sram0_0_seg": 500, "lp_ram_seg": 36}),
                                   size.load_baseline(self.path), 256)
        self.assertEqual(len(problems), 1)
        self.assertIn("lp_ram_seg: region not in the baseline", problems[0])


class BudgetTest(unittest.TestCase):
    def test_within_budget(self):
        problems, warnings = size.budget(report(image=700, prov_image=900), 80, 92, 95, {})
        self.assertEqual((problems, warnings), ([], []))

    def test_app_over_budget_names_partition_and_percent(self):
        problems, _ = size.budget(report(image=850), 80, 92, 95, {})
        self.assertEqual(problems, ["modbus-server-tedge-ota-c6: app image is 850 B, 85.0% "
                                    "of the 1000 B slot0 partition (budget 80%)"])

    def test_provisioner_over_budget(self):
        problems, _ = size.budget(report(prov_image=950), 80, 92, 95, {})
        self.assertEqual(len(problems), 1)
        self.assertIn("provisioner image is 950 B, 95.0% of the 1000 B prov partition", problems[0])

    def test_known_exception_is_a_warning(self):
        baseline = {"modbus-server-tedge-ota-c6": {
            "budget_exceptions": {"provisioner": "until group 9 trims it"}}}
        problems, warnings = size.budget(report(prov_image=950), 80, 92, 95, baseline)
        self.assertEqual(problems, [])
        self.assertEqual(len(warnings), 1)
        self.assertIn("known exception: until group 9 trims it", warnings[0])

    def test_ota_ceiling_is_separate_and_not_excepted(self):
        baseline = {"modbus-server-tedge-ota-c6": {"budget_exceptions": {"app": "x"}}}
        problems, warnings = size.budget(report(image=960), 80, 92, 95, baseline)
        self.assertEqual(len(warnings), 1)  # the budget, excepted
        self.assertEqual(len(problems), 1)  # the ceiling, never excepted
        self.assertIn("OTA ceiling 95%", problems[0])


if __name__ == "__main__":
    unittest.main()
