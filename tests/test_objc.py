"""Unit tests for the ObjC metadata parser and generator (synthetic image only)."""

import contextlib
import copy
import io
import json
import struct
import tempfile
import unittest
from pathlib import Path

import objcfixtures as fx
from mtport import ghidra_common as gc
from mtport import macho, objc


def parsed(data: bytes = fx.IMAGE) -> objc.ObjCImage:
    return objc.parse(macho.MachO(data))


class ParseTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.objc = parsed()
        cls.widget, cls.cbthing = cls.objc.classes

    def test_classes_and_superclasses(self):
        w, cb = self.widget, self.cbthing
        self.assertEqual(
            (w.name, w.address, w.metaclass),
            ("Widget", fx.ADDRS["widget"], fx.ADDRS["widget_meta"]),
        )
        self.assertEqual(w.superclass, objc.ClassRef("NSObject", None))  # imported, via bind
        self.assertEqual(cb.superclass, objc.ClassRef("Widget", fx.ADDRS["widget"]))
        self.assertEqual((w.group, cb.group), ("app", "chartboost"))
        self.assertEqual((w.instance_start, w.instance_size), (4, 12))
        self.assertEqual((w.flags, w.meta_flags), (0, objc.RO_META))

    def test_methods_mask_entsize_flags_and_keep_the_thumb_bit(self):
        self.assertEqual(
            self.widget.instance_methods,
            [
                objc.Method("init", "@8@0:4", fx.IMP_INIT),
                objc.Method("size", "i8@0:4", fx.IMP_SHARED),
                objc.Method("setSize:", "v12@0:4i8", fx.IMP_SET_SIZE),
            ],
        )
        self.assertEqual(
            self.widget.class_methods, [objc.Method("widgetWithSize:", "@12@0:4i8", fx.IMP_SIZE)]
        )
        self.assertEqual([m.selector for m in self.cbthing.instance_methods], ["run"])
        self.assertEqual(self.cbthing.class_methods, [])

    def test_ivars_properties_protocols(self):
        self.assertEqual(
            self.widget.ivars,
            [
                objc.Ivar("_size", "i", 4, fx.ADDRS["ivar_size"], 2, 4),
                objc.Ivar("_name", '@"NSString"', 8, fx.ADDRS["ivar_name"], 2, 4),
            ],
        )
        self.assertEqual(self.widget.properties, [objc.Property("size", "Ti,N,V_size")])
        self.assertEqual(self.widget.protocols, ["WidgetDelegate"])

    def test_protocols_include_referenced_ones(self):
        delegate, nsobject = self.objc.protocols
        self.assertEqual((delegate.name, delegate.protocols), ("WidgetDelegate", ["NSObject"]))
        self.assertEqual(
            delegate.optional_instance_methods, [objc.Method("widgetDidChange:", "v12@0:4@8", 0)]
        )
        self.assertEqual(delegate.properties[0].name, "delegate")
        self.assertEqual(
            (nsobject.name, nsobject.address), ("NSObject", fx.ADDRS["proto_nsobject"])
        )
        self.assertEqual([m.selector for m in nsobject.instance_methods], ["description"])

    def test_category_on_an_imported_class(self):
        (cat,) = self.objc.categories
        self.assertEqual(
            (cat.name, cat.class_name, cat.address), ("Extras", "NSString", fx.ADDRS["category"])
        )
        self.assertIsNone(cat.cls.address)
        self.assertEqual([m.imp for m in cat.instance_methods], [fx.IMP_SHARED])
        self.assertEqual([m.selector for m in cat.class_methods], ["stringWithWidget:"])

    def test_reference_tables(self):
        a = fx.ADDRS
        self.assertEqual(
            self.objc.selrefs, [(a["selref_size"], "size"), (a["selref_init"], "initWithFrame:")]
        )
        self.assertEqual(
            self.objc.classrefs,
            [
                (a["classref_widget"], objc.ClassRef("Widget", a["widget"])),
                (a["classref_nsarray"], objc.ClassRef("NSArray", None)),
            ],
        )
        self.assertEqual(
            self.objc.superrefs,
            [
                (a["superref_meta"], objc.ClassRef("Widget", a["widget_meta"], meta=True)),
                (a["superref_widget"], objc.ClassRef("Widget", a["widget"])),
            ],
        )
        self.assertEqual(self.objc.protorefs, [(a["protoref"], "WidgetDelegate")])

    def test_chartboost_rule(self):
        for name in ("Chartboost", "CBJsonWriter", "CBAPIRequest"):
            self.assertTrue(objc.is_chartboost(name), name)
        for name in ("CashShopKeyPadView", "CBthing", "MapleThiefAppDelegate", "EAGLView"):
            self.assertFalse(objc.is_chartboost(name), name)


class OutputTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.objc = parsed()

    def test_json_is_deterministic_and_complete(self):
        source = {"path": "x", "arch": "armv7", "sha256": "0" * 64}
        a = gc.dump_json(objc.to_json(self.objc, source))
        b = gc.dump_json(objc.to_json(parsed(), source))
        self.assertEqual(a, b)
        doc = json.loads(a)
        self.assertEqual(doc["counts"]["classes"], 2)
        self.assertEqual(doc["counts"]["imps"], 6)
        widget = doc["classes"][0]
        self.assertEqual(
            widget["superclass"],
            {"name": "NSObject", "address": None, "imported": True, "meta": False},
        )
        self.assertEqual(
            widget["instance_methods"][0], {"selector": "init", "types": "@8@0:4", "imp": "0x1801"}
        )
        self.assertEqual(widget["ivars"][1]["offset_address"], objc.hexaddr(fx.ADDRS["ivar_name"]))
        self.assertEqual(doc["selrefs"][1], {"address": "0x5A04", "selector": "initWithFrame:"})
        self.assertNotIn("imp", doc["protocols"][0]["optional_instance_methods"][0])

    def test_names_follow_the_contract(self):
        doc = objc.names_json(self.objc)
        objc.check_names(doc)
        self.assertEqual(doc["source"], "objc")
        by_addr = {n["address"]: n for n in doc["names"]}
        self.assertEqual(len(by_addr), 6)  # one entry per IMP, the shared one only once
        self.assertEqual(by_addr["0x1801"]["name"], "-[Widget init]")
        self.assertEqual(by_addr["0x1811"]["name"], "+[Widget widgetWithSize:]")
        self.assertEqual(by_addr["0x1851"]["name"], "+[NSString(Extras) stringWithWidget:]")
        shared = by_addr[objc.hexaddr(fx.IMP_SHARED)]
        self.assertEqual(shared["name"], "-[Widget size]")
        self.assertIn("also -[NSString(Extras) widgetSize]", shared["comment"])
        self.assertTrue(all(n["kind"] == "function" for n in doc["names"]))
        addrs = [int(n["address"], 16) for n in doc["names"]]
        self.assertEqual(addrs, sorted(addrs))

    def test_check_names_rejects_bad_documents(self):
        good = objc.names_json(self.objc)
        cases = {
            "extra top-level key": lambda d: d.update(version=1),
            "lower-case hex": lambda d: d["names"][0].update(address="0x18a1"),
            "leading zero": lambda d: d["names"][0].update(address="0x01801"),
            "decimal": lambda d: d["names"][0].update(address="6145"),
            "duplicate": lambda d: d["names"][1].update(address=d["names"][0]["address"]),
            "empty name": lambda d: d["names"][0].update(name=" "),
            "newline": lambda d: d["names"][0].update(name="a\nb"),
            "kind": lambda d: d["names"][0].update(kind="data"),
            "missing kind": lambda d: d["names"][0].pop("kind"),
            "unknown key": lambda d: d["names"][0].update(size=4),
            "comment type": lambda d: d["names"][0].update(comment=3),
            "names type": lambda d: d.update(names={}),
            "source": lambda d: d.update(source=""),
        }
        for what, mutate in cases.items():
            doc = copy.deepcopy(good)
            mutate(doc)
            with self.subTest(what), self.assertRaises(ValueError):
                objc.check_names(doc)

    def test_markdown_lists_app_classes_in_full_and_summarizes_chartboost(self):
        md = objc.to_markdown(self.objc)
        self.assertIn("### Widget", md)
        for m in self.objc.classes[0].instance_methods + self.objc.classes[0].class_methods:
            self.assertIn(f"| `{objc.hexaddr(m.imp)}` | `{m.selector}` |", md)
        self.assertIn('| 8 | `_name` | `@"NSString"` | 4 |', md)
        self.assertIn("### NSString (Extras)", md)
        self.assertNotIn("### CBThing", md)  # Chartboost only in the summary table
        self.assertIn("| `CBThing` | `Widget` | 0 | 1 | 0 | `0x1841`–`0x1841` |", md)
        self.assertIn("| `WidgetDelegate` | `NSObject` | 0 | 0 | 1 | 0 | 1 |", md)
        self.assertTrue(md.endswith("\n"))

    def test_markdown_escapes_table_pipes(self):
        self.assertEqual(objc._code("a|b"), "`a\\|b`")
        self.assertEqual(objc._code("a`b"), "`` a`b ``")


class MalformedTest(unittest.TestCase):
    def mutated(self, addr: int, value: int) -> bytes:
        data = bytearray(fx.IMAGE)
        m = macho.MachO(fx.IMAGE)
        struct.pack_into("<I", data, m.vm_to_offset(addr), value)
        return bytes(data)

    def ro(self, cls_addr: int) -> int:
        m = macho.MachO(fx.IMAGE)
        return m.u32(cls_addr + 16) & ~3

    def test_method_count_too_large(self):
        methods = macho.MachO(fx.IMAGE).u32(self.ro(fx.ADDRS["widget"]) + 20)
        with self.assertRaisesRegex(objc.ObjCError, "method_list_t"):
            parsed(self.mutated(methods + 4, 0x7FFFFFFF))

    def test_method_list_past_end_of_data(self):
        methods = macho.MachO(fx.IMAGE).u32(self.ro(fx.ADDRS["widget"]) + 20)
        with self.assertRaises(macho.MachOError):
            parsed(self.mutated(methods + 4, 0x1000))

    def test_metaclass_in_classlist(self):
        classlist = macho.MachO(fx.IMAGE).section("__DATA", "__objc_classlist").addr
        with self.assertRaisesRegex(objc.ObjCError, "is a metaclass"):
            parsed(self.mutated(classlist, fx.ADDRS["widget_meta"]))

    def test_isa_that_is_not_a_metaclass(self):
        with self.assertRaisesRegex(objc.ObjCError, "is not a metaclass"):
            parsed(self.mutated(fx.ADDRS["widget"], fx.ADDRS["cbthing"]))

    def test_dangling_pointer(self):
        with self.assertRaises(macho.MachOError):
            parsed(self.mutated(fx.ADDRS["selref_size"], 0x9000_0000))

    def test_null_protocol(self):
        protolist = macho.MachO(fx.IMAGE).section("__DATA", "__objc_protolist").addr
        with self.assertRaisesRegex(objc.ObjCError, "null protocol"):
            parsed(self.mutated(protolist, 0))


class CommandLineTest(unittest.TestCase):
    def test_generate_writes_three_outputs(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            slice_path = repo / "work" / "slices" / "MapleThief.armv7"
            slice_path.parent.mkdir(parents=True)
            slice_path.write_bytes(fx.IMAGE)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(objc.main(["generate", "--repo", tmp]), 0)
            doc = json.loads((repo / objc.JSON_OUT).read_text())
            self.assertEqual(doc["source"]["path"], "work/slices/MapleThief.armv7")
            names = json.loads((repo / objc.NAMES_OUT).read_text())
            objc.check_names(names)
            md = (repo / objc.MARKDOWN_OUT).read_text()
            self.assertIn(doc["source"]["sha256"], md)
            self.assertIn("imps                 6", out.getvalue())

            first = [(repo / p).read_bytes() for p in (objc.JSON_OUT, objc.NAMES_OUT)]
            with contextlib.redirect_stdout(io.StringIO()):
                objc.main(["generate", "--repo", tmp])
            again = [(repo / p).read_bytes() for p in (objc.JSON_OUT, objc.NAMES_OUT)]
            self.assertEqual(first, again)

    def test_json_command_and_errors(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "image"
            path.write_bytes(fx.IMAGE)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(objc.main(["json", str(path)]), 0)
            self.assertEqual(json.loads(out.getvalue())["classes"][0]["name"], "Widget")
            path.write_bytes(b"junk")
            with contextlib.redirect_stderr(io.StringIO()) as err:
                self.assertEqual(objc.main(["json", str(path)]), 1)
            self.assertIn("error:", err.getvalue())
