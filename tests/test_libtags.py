"""Unit tests for mtport.libtags on a synthetic program (no IPA, no Ghidra).

The fixture is a small armv7 image (machofixtures.ImageBuilder) plus hand-written stand-ins
for Ghidra's functions.json / xrefs.json and #8's objc.json. Each function exercises one
rule; the expected tag and rule per function are in ``EXPECTED``. Like the real binary, the
engine functions sit between IMPs of other classes than the ones that call them, so the
``objc-file`` rule does not claim them.
"""

import json
import unittest

from machofixtures import BUILDER_SECTIONS, ImageBuilder, bind_stream, export_trie
from mtport import libtags, macho
from thumbasm import mov32

TEXT = ("__TEXT", "__text")
TEXT_START = 0x1800
TEXT_SIZE = 0xE0
STUB = 0x2400  # __symbolstub1: the _UIApplicationMain stub

# name -> (address, thumb)
F = {
    "start": (0x1800, False),
    "main": (0x1810, True),
    "cb_far": (0x1818, True),
    "game_imp": (0x1820, True),
    "game_helper": (0x1830, True),
    "game_block": (0x1838, True),
    "other_imp": (0x1840, True),
    "engine1": (0x1848, True),
    "vfunc": (0x1850, True),
    "dead": (0x1858, True),
    "engine2": (0x1860, True),
    "cb_imp": (0x1870, True),
    "cb_helper": (0x1878, True),
    "shared": (0x1880, True),
    "bson": (0x1890, True),
    "oal": (0x18A0, True),
    "init": (0x18B0, True),
    "crc": (0x18C0, True),
    "orphan": (0x18D0, True),
}

EXPECTED = {
    "start": ("lib:crt1", "entry-point"),
    "main": ("app-objc", "entry-point"),
    "cb_far": ("lib:chartboost", "closure"),
    "game_imp": ("app-objc", "objc-imp"),
    "game_helper": ("app-objc", "objc-file"),
    "game_block": ("app-objc", "objc-file"),
    "other_imp": ("app-objc", "objc-imp"),
    "engine1": ("engine", "reach"),
    "vfunc": ("engine", "vtable"),
    "dead": ("engine", "layout"),
    "engine2": ("engine", "reach"),
    "cb_imp": ("lib:chartboost", "objc-imp"),
    "cb_helper": ("lib:chartboost", "objc-file"),
    "shared": ("engine", "reach"),
    "bson": ("lib:chartboost", "export"),
    "oal": ("lib:apple-openal-support", "signature"),
    "init": ("engine", "static-init"),
    "crc": ("engine", "reach"),
    "orphan": ("unknown", "unknown"),
}


def ptr(name: str) -> str:
    addr, thumb = F[name]
    return f"0x{addr | int(thumb):X}"


SECTIONS = {
    **BUILDER_SECTIONS,
    ("__TEXT", "__symbolstub1"): (0x2400, 0x100),
    ("__DATA", "__mod_init_func"): (0x6000, 0x40),
    ("__DATA", "__const"): (0x6040, 0x400),
}


def build():
    b = ImageBuilder(SECTIONS)
    code = bytearray(TEXT_SIZE)
    off = F["crc"][0] - TEXT_START
    code[off : off + 8] = mov32(0, 0xEDB88320)
    assert b.put(TEXT, bytes(code)) == TEXT_START
    b.put(("__TEXT", "__symbolstub1"), b"\0" * 4)
    oal_msg = b.cstring("__cstring", "MyGetOpenALAudioData: failed")
    ti_name = b.cstring("__cstring", "6CThing")
    # typeinfo {vtable ptr (bound), name}, then a vtable {offset_to_top, typeinfo, vfunc}
    ti = b.words(("__DATA", "__const"), 0, ti_name)
    vtable = b.words(("__DATA", "__const"), 0, ti, int(ptr("vfunc"), 16))
    # a block descriptor-like object in __data that points at game_block
    desc = b.words(("__DATA", "__data"), 0, 20, int(ptr("game_block"), 16))
    b.words(("__DATA", "__mod_init_func"), int(ptr("init"), 16))
    data = b.build(
        bind=bind_stream([(ti, "__ZTVN10__cxxabiv117__class_type_infoE", 8)]),
        export=export_trie([("_bson_init", int(ptr("bson"), 16)), ("start", 0x1800)]),
        function_starts=sorted(int(ptr(n), 16) for n in F),
        entry=0x1800,
    )
    functions = {
        "functions": [{"address": ptr(n), "size": 8, "name": n, "thunk_of": None} for n in F]
        + [
            {
                "address": f"0x{STUB:X}",
                "size": 4,
                "name": "_UIApplicationMain",
                "thunk_of": "EXTERNAL:UIKit::_UIApplicationMain",
            }
        ]
    }
    xrefs = []

    def ref(src, to, kind="UNCONDITIONAL_CALL", at=2):
        addr = F[src][0] + at
        target = to if isinstance(to, int) else int(ptr(to), 16) & ~1
        xrefs.append(
            {
                "from": f"0x{addr:X}",
                "to": f"0x{target:X}",
                "type": kind,
                "function": ptr(src),
                "operand": 0,
            }
        )

    ref("start", "main")
    ref("main", STUB)
    ref("game_imp", "game_helper")
    ref("game_imp", desc, "DATA", at=4)
    ref("game_imp", "engine1", at=6)
    ref("engine1", "engine2")
    ref("engine1", "crc", at=4)
    ref("engine2", "shared")
    ref("cb_imp", "shared")
    ref("cb_imp", "cb_helper", at=4)
    ref("cb_imp", "cb_far", at=6)
    ref("oal", oal_msg, "PARAM")
    objc = {
        "classes": [
            {
                "name": "Game",
                "instance_methods": [{"selector": "run", "imp": ptr("game_imp")}],
                "class_methods": [],
            },
            {
                "name": "Other",
                "instance_methods": [{"selector": "tick", "imp": ptr("other_imp")}],
                "class_methods": [],
            },
            {
                "name": "CBThing",
                "instance_methods": [],
                "class_methods": [{"selector": "go", "imp": ptr("cb_imp")}],
            },
        ],
        "categories": [],
        "selrefs": [],
    }
    image = macho.MachO(data)
    return image, functions, {"xrefs": xrefs}, objc, vtable


class TaggingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.image, functions, xrefs, objc, cls.vtable = build()
        cls.prog = libtags.build_program(cls.image, functions, xrefs, objc)
        cls.tagger = libtags.tag_program(cls.prog)
        cls.doc = json.loads(libtags.to_json(cls.tagger, {"arch": "armv7", "sha256": "x"}))

    def test_every_rule_on_its_function(self):
        for name, want in EXPECTED.items():
            a = self.tagger.assigned[F[name][0]]
            with self.subTest(name=name):
                self.assertEqual((a.tag, a.rule), want, a.reason)

    def test_coverage_is_exact(self):
        self.assertEqual(len(self.tagger.assigned), len(F))
        self.assertEqual(libtags.check_doc(self.doc, self.image)["engine"], 7)

    def test_vtable_scan(self):
        (vt,) = self.tagger.vtables
        self.assertEqual((vt.address, vt.class_name), (self.vtable, "CThing"))
        self.assertEqual(vt.functions, (F["vfunc"][0],))

    def test_weak_marker_reported_not_tagged(self):
        crc = [s for s in self.doc["signatures"] if "crc32 polynomial" in s["marker"]][0]
        self.assertEqual(crc["hits"], ["0x18C0"])
        self.assertEqual(crc["functions"], [ptr("crc")])
        self.assertFalse(crc["strong"])

    def test_evidence_for_every_library(self):
        for tag, lib in self.doc["libraries"].items():
            with self.subTest(tag=tag):
                self.assertTrue(lib["evidence"])
                for ev in lib["evidence"]:
                    self.assertRegex(ev["address"], r"^0x[0-9A-F]+")
                    self.assertTrue(ev["reason"])

    def test_counts_add_up(self):
        counts = self.doc["counts"]
        self.assertEqual(sum(c["functions"] for c in counts.values()), len(F))
        self.assertEqual(sum(c["bytes"] for c in counts.values()), TEXT_SIZE)
        self.assertEqual(list(counts)[:2], ["app-objc", "engine"])
        self.assertEqual(list(counts)[-1], "unknown")

    def test_markdown(self):
        md = libtags.to_markdown(self.doc)
        self.assertIn("| `lib:chartboost` | 4 |", md)
        self.assertIn(f"**{TEXT_SIZE:,} ", md)
        self.assertIn("### `lib:crt1`", md)

    def test_json_is_deterministic(self):
        again = libtags.tag_program(self.prog)
        self.assertEqual(
            libtags.to_json(again, {"arch": "armv7", "sha256": "x"}),
            libtags.to_json(self.tagger, {"arch": "armv7", "sha256": "x"}),
        )

    def test_pluggable_engine_evidence(self):
        def provider(prog):
            return [libtags.VTable(0x6000, "std::thing", 0x6004, (F["orphan"][0],))]

        t = libtags.tag_program(self.prog, libtags.default_rules(engine_evidence=provider))
        self.assertEqual(t.assigned[F["orphan"][0]].tag, "lib:stl")
        self.assertEqual(t.assigned[F["vfunc"][0]].tag, "engine")  # now via layout
        self.assertEqual(t.assigned[F["vfunc"][0]].rule, "layout")


class CoverageTest(unittest.TestCase):
    def setUp(self):
        self.image, functions, xrefs, objc, _ = build()
        t = libtags.tag_program(libtags.build_program(self.image, functions, xrefs, objc))
        self.doc = json.loads(libtags.to_json(t, {}))

    def check(self):
        return libtags.check_doc(self.doc, self.image)

    def test_missing_function(self):
        self.doc["functions"].pop()
        with self.assertRaisesRegex(libtags.CoverageError, "missing 1"):
            self.check()

    def test_extra_function(self):
        self.doc["functions"].append(dict(self.doc["functions"][0], address="0x18D9"))
        with self.assertRaisesRegex(libtags.CoverageError, "extra 1"):
            self.check()

    def test_wrong_thumb_bit(self):
        self.doc["functions"][1]["address"] = "0x1810"
        with self.assertRaises(libtags.CoverageError):
            self.check()

    def test_duplicate(self):
        self.doc["functions"].append(self.doc["functions"][0])
        with self.assertRaisesRegex(libtags.CoverageError, "duplicate"):
            self.check()

    def test_bad_tag(self):
        self.doc["functions"][0]["tag"] = "lib:"
        with self.assertRaisesRegex(libtags.CoverageError, "bad tag"):
            self.check()

    def test_library_without_evidence(self):
        self.doc["libraries"]["lib:crt1"]["evidence"] = []
        with self.assertRaisesRegex(libtags.CoverageError, "no evidence"):
            self.check()


class ApiTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        image, functions, xrefs, objc, _ = build()
        t = libtags.tag_program(libtags.build_program(image, functions, xrefs, objc))
        cls.tags = libtags.FunctionTags(json.loads(libtags.to_json(t, {})))

    def test_tag_of(self):
        self.assertEqual(self.tags.tag_of(0x1821), "app-objc")
        self.assertEqual(self.tags.tag_of(0x1820), "app-objc")
        self.assertEqual(self.tags.tag_of("0x1871"), "lib:chartboost")
        self.assertIsNone(self.tags.tag_of(0x1822))

    def test_tag_containing(self):
        self.assertEqual(self.tags.tag_containing(0x1826), "app-objc")
        self.assertIsNone(self.tags.tag_containing(0x17FF))

    def test_functions_with(self):
        self.assertEqual(
            self.tags.functions_with("lib:chartboost"),
            [ptr("cb_far"), ptr("cb_imp"), ptr("cb_helper"), ptr("bson")],
        )
        self.assertEqual(len(self.tags.functions_with("lib:")), 6)
        self.assertTrue(self.tags.is_library(0x1891))
        self.assertFalse(self.tags.is_library(0x1841))

    def test_ghidra_plan(self):
        plan = libtags.ghidra_plan(self.tags.doc)
        self.assertEqual(plan[0x1800], "lib:crt1")
        self.assertEqual(len(plan), len(F))
        comments = libtags.tag_comments(self.tags.doc)
        self.assertEqual(set(comments), set(plan.values()))
        self.assertTrue(libtags.is_scheme_tag("lib:zlib"))
        self.assertTrue(libtags.is_scheme_tag("engine"))
        self.assertFalse(libtags.is_scheme_tag("lib:"))
        self.assertFalse(libtags.is_scheme_tag("my-note"))

    def test_readback_round_trip(self):
        doc = self.tags.doc
        plan = libtags.ghidra_plan(doc)
        held = {a: [t, "my-note"] for a, t in plan.items()} | {0x9000: ["my-note"]}
        text = libtags.readback_json(doc, held)
        self.assertEqual(text, libtags.readback_json(doc, dict(reversed(held.items()))))
        back = json.loads(text)
        self.assertEqual(back["count"], len(plan))
        self.assertEqual(back["functions"][0], {"address": ptr("start"), "tags": ["lib:crt1"]})
        self.assertEqual(libtags.readback_problems(doc, back), [])

    def test_readback_problems(self):
        doc = self.tags.doc
        held = {a: [t] for a, t in libtags.ghidra_plan(doc).items()}
        held[0x1800] = ["engine", "lib:crt1"]
        held[0x9000] = ["engine"]
        del held[0x1820]
        problems = libtags.readback_problems(doc, json.loads(libtags.readback_json(doc, held)))
        self.assertEqual(
            problems,
            [
                "0x1800: Ghidra has ['engine', 'lib:crt1'], expected ['lib:crt1']",
                "0x1820: Ghidra has [], expected ['app-objc']",
                "0x9000: Ghidra has ['engine'], untagged",
            ],
        )
        self.assertEqual(len(libtags.readback_problems(doc, {"schema": 0})), 1)


class ConstScanTest(unittest.TestCase):
    def test_movw_movt_pairs(self):
        image, functions, xrefs, objc, _ = build()
        prog = libtags.build_program(image, functions, xrefs, objc)
        hits = libtags._const32_hits(prog)
        self.assertEqual(hits[0xEDB88320], [0x18C0])


if __name__ == "__main__":
    unittest.main()
