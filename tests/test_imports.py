"""Unit tests for the import table (mtport.macho symbol tables, mtport.armscan, mtport.imports).

Synthetic images only (tests/importfixtures.py); the real slice is checked in
test_imports_real.py.
"""

from __future__ import annotations

import struct
import unittest

import capstone

import importfixtures as fx
from mtport import armscan, imports, macho, names, objc


def program(t: fx.Thumb, extra_starts=()) -> macho.MachO:
    return macho.MachO(fx.build(bytes(t.code), [fx.TEXT | 1, *extra_starts]))


def disasm(t: fx.Thumb) -> list[str]:
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB)
    return [f"{i.mnemonic} {i.op_str}".strip() for i in md.disasm(bytes(t.code), t.base)]


def scan(image: macho.MachO) -> armscan.Scan:
    return armscan.scan(image, [fx.STUB_MSGSEND])


class AssemblerTest(unittest.TestCase):
    """The hand encodings decode to what the tests mean."""

    def test_encodings(self):
        t = fx.Thumb()
        t.push_r7_lr().movw(0, 0x1234).movt(0, 0x5678).add_pc(0).ldr(1, 0, 4)
        t.str_sp(1, 8).ldr_sp(2, 8).movs(3, 7).blx(fx.STUB_FOO).bl(fx.TEXT).pop_r7_pc()
        self.assertEqual(
            disasm(t),
            [
                "push {r7, lr}",
                "movw r0, #0x1234",
                "movt r0, #0x5678",
                "add r0, pc",
                "ldr r1, [r0, #4]",
                "str r1, [sp, #8]",
                "ldr r2, [sp, #8]",
                "movs r3, #7",
                f"blx #{fx.STUB_FOO:#x}",
                f"bl #{fx.TEXT:#x}",
                "pop {r7, pc}",
            ],
        )


class SymbolTableTest(unittest.TestCase):
    def setUp(self):
        self.m = program(fx.Thumb().pop_r7_pc())

    def test_symbols_and_libraries(self):
        und = self.m.undefined_symbols
        self.assertEqual([s.name for s in und], [n for n, _ in fx.UNDEFINED])
        self.assertEqual([s.library_ordinal for s in und], [o for _, o in fx.UNDEFINED])
        self.assertFalse(self.m.symbols[0].undefined)
        self.assertEqual(self.m.library_name(2), "/usr/lib/libobjc.A.dylib")
        self.assertEqual(self.m.library_name(0), "<self>")

    def test_indirect_slots(self):
        slots = [(s.kind, s.address, s.symbol) for s in self.m.indirect_slots]
        self.assertEqual(
            slots,
            [
                ("stub", fx.STUB_FOO, "_foo"),
                ("stub", fx.STUB_MSGSEND, "_objc_msgSend"),
                ("stub", fx.STUB_BAR, "_bar"),
                ("lazy", fx.LAZY, "_foo"),
                ("lazy", fx.LAZY + 4, "_objc_msgSend"),
                ("lazy", fx.LAZY + 8, "_bar"),
                ("non_lazy", fx.NL, "_kConst"),
                ("non_lazy", fx.NL + 4, None),  # INDIRECT_SYMBOL_LOCAL
            ],
        )
        self.assertEqual(self.m.stub_symbol(fx.STUB_BAR), "_bar")
        self.assertEqual(self.m.stub_symbol(fx.STUB_BAR | 1), "_bar")
        self.assertIsNone(self.m.stub_symbol(fx.STUB_BAR + 2))
        self.assertIsNone(self.m.stub_symbol(fx.NL + 4))

    def test_indirect_index_out_of_range(self):
        data = bytearray(fx.build(b"\x80\xbd", [fx.TEXT | 1]))
        # Point the stub section's reserved1 past the 8-entry table.
        at = data.find(b"__symbolstub1")
        struct.pack_into("<I", data, at + 60, 7)  # reserved1
        with self.assertRaisesRegex(macho.MachOError, "past the indirect symbol table"):
            _ = macho.MachO(bytes(data)).indirect_slots

    def test_no_symtab(self):
        from machofixtures import thin_image

        m = macho.MachO(thin_image([0x1401]))
        self.assertEqual((m.symbols, m.indirect_slots, m.indirect_symbols), ((), (), ()))


class ImportTableTest(unittest.TestCase):
    def setUp(self):
        self.m = program(fx.Thumb().pop_r7_pc())
        self.table = {i.name: i for i in imports.import_table(self.m)}

    def test_every_symbol_with_its_places(self):
        t = self.table
        self.assertEqual(list(t), [n for n, _ in fx.UNDEFINED])
        self.assertEqual((t["_foo"].stubs, t["_foo"].lazy), ([fx.STUB_FOO], [fx.LAZY]))
        self.assertEqual(t["_kConst"].non_lazy, [fx.NL])
        self.assertEqual(t["_OBJC_CLASS_$_NSString"].binds, [(fx.CLASSREF, "__objc_classrefs")])
        self.assertEqual(t["_free"].binds, [(fx.DATA_WORD, "__data")])
        self.assertEqual(t["_kConst"].binds, [])  # the nl pointer is not repeated as a bind
        kinds = {n: i.kind for n, i in t.items()}
        self.assertEqual(kinds["_foo"], "function")
        self.assertEqual(kinds["_kConst"], "address")
        self.assertEqual(kinds["_OBJC_CLASS_$_NSString"], "class")
        self.assertTrue(t["_unused"].weak)
        self.assertEqual(t["_unused"].framework, "GameKit")
        self.assertEqual(t["_foo"].framework, "libSystem")

    def test_stubs_are_consistent(self):
        self.assertEqual(imports.check_stubs(self.m), [])

    def test_stub_pointing_elsewhere_is_reported(self):
        # Every stub loads _objc_msgSend's lazy pointer: _foo's and _bar's are wrong.
        words = [0xE59FF000 | (fx.LAZY + 4 - (fx.STUBS + 4 * i) - 8) for i in range(3)]
        m = macho.MachO(fx.build(b"\x80\xbd", [fx.TEXT | 1], words))
        problems = imports.check_stubs(m)
        self.assertEqual(len(problems), 2)
        self.assertIn("_foo", problems[0])

    def test_framework_names(self):
        self.assertEqual(
            imports.framework_name("/System/Library/Frameworks/UIKit.framework/UIKit"), "UIKit"
        )
        self.assertEqual(imports.framework_name("/usr/lib/libstdc++.6.dylib"), "libstdc++")


class ScannerTest(unittest.TestCase):
    def test_call_and_literal_pool(self):
        t = fx.Thumb().push_r7_lr()
        call_site = t.here
        t.blx(fx.STUB_FOO)
        lit = t.here
        t.ldr_lit(0, None)
        add = t.here
        t.add_pc(0).ldr(1, 0).pop_r7_pc().align4()
        pool = t.here
        t.word(fx.NL - (add + 4))
        t.code[lit - t.base : lit - t.base + 2] = fx.Thumb(lit).ldr_lit(0, pool).code
        result = scan(program(t))
        refs = {(r.site, r.target, r.kind) for r in result.refs}
        self.assertIn((call_site, fx.STUB_FOO, "call"), refs)
        self.assertIn((add + 2, fx.NL, "load"), refs)
        self.assertEqual(result.skipped_bytes, 4)  # the literal was not decoded

    def test_movw_movt_and_stack_spill(self):
        t = fx.Thumb().push_r7_lr()
        t.movw(0, 0)  # patched below
        t.movt(0, 0)
        add = t.here
        t.add_pc(0).str_sp(0, 4).movs(0, 0).ldr_sp(2, 4)
        site = t.here
        t.ldr(3, 2).pop_r7_pc()
        value = fx.CLASSREF - (add + 4)
        t.code[2:6] = fx.Thumb().movw(0, value & 0xFFFF).code
        t.code[6:10] = fx.Thumb().movt(0, value >> 16).code
        refs = {(r.site, r.target, r.kind) for r in scan(program(t)).refs}
        self.assertIn((site, fx.CLASSREF, "load"), refs)

    def test_join_keeps_only_values_both_paths_agree_on(self):
        def body(same: bool):
            t = fx.Thumb().push_r7_lr()
            branch = t.here
            path_b, join = branch + 10, branch + 16
            t.cbz(3, path_b)
            lit_a = t.here  # path A: r0 = &nl_ptr
            t.ldr_lit(0, None)
            add_a = t.here
            t.add_pc(0).b(join).nop()
            assert t.here == path_b  # path B: r0 = &nl_ptr or &classref
            t.ldr_lit(0, None)
            add_b = t.here
            t.add_pc(0).nop()
            assert t.here == join
            t.ldr(1, 0).pop_r7_pc().align4()
            pool = t.here
            t.word(fx.NL - (add_a + 4)).word((fx.NL if same else fx.CLASSREF) - (add_b + 4))
            t.code[lit_a - t.base : lit_a - t.base + 2] = fx.Thumb(lit_a).ldr_lit(0, pool).code
            t.code[path_b - t.base : path_b - t.base + 2] = (
                fx.Thumb(path_b).ldr_lit(0, pool + 4).code
            )
            return t, join, add_a, add_b

        t, join, _, _ = body(True)
        refs = {(r.site, r.target, r.kind) for r in scan(program(t)).refs}
        self.assertIn((join, fx.NL, "load"), refs)
        t, join, add_a, add_b = body(False)
        refs = {(r.site, r.target, r.kind) for r in scan(program(t)).refs}
        self.assertNotIn(join, {s for s, _, _ in refs})
        self.assertIn((add_a, fx.NL, "address"), refs)
        self.assertIn((add_b, fx.CLASSREF, "address"), refs)

    def test_send_records_receiver_and_selector(self):
        t = fx.Thumb().push_r7_lr()
        pool_refs = []
        for rd, slot in ((0, fx.CLASSREF), (1, fx.SELREF)):
            pool_refs.append((t.here, rd, slot))
            t.ldr_lit(rd, None)
            t.add_pc(rd)
            t.ldr(rd, rd)
        site = t.here
        t.blx(fx.STUB_MSGSEND)
        after = t.here
        t.blx(fx.STUB_MSGSEND).pop_r7_pc().align4()
        pool = t.here
        for k, (lit, rd, slot) in enumerate(pool_refs):
            t.word(slot - (lit + 2 + 4))
            t.code[lit - t.base : lit - t.base + 2] = fx.Thumb(lit).ldr_lit(rd, pool + 4 * k).code
        result = scan(program(t))
        first, second = result.sends
        self.assertEqual(first.site, site)
        self.assertEqual(first.regs[0], ("m", fx.CLASSREF))
        self.assertEqual(first.regs[1], ("m", fx.SELREF))
        self.assertEqual(
            (second.site, second.regs[0], second.regs[1]), (after, ("ret", site), None)
        )

    def test_send_records_stack_arguments(self):
        """A selector stored to [sp, #4] before the call is the send's second stack word."""
        t = fx.Thumb().push_r7_lr()
        lit = t.here
        t.ldr_lit(0, None)
        add = t.here
        t.add_pc(0).ldr(0, 0).str_sp(0, 4)
        site = t.here
        t.blx(fx.STUB_MSGSEND).pop_r7_pc().align4()
        pool = t.here
        t.word(fx.SELREF - (add + 4))
        t.code[lit - t.base : lit - t.base + 2] = fx.Thumb(lit).ldr_lit(0, pool).code
        (send,) = scan(program(t)).sends
        self.assertEqual(send.site, site)
        self.assertEqual(len(send.stack), armscan.STACK_ARGS)
        self.assertEqual(send.stack[:2], (None, ("m", fx.SELREF)))

    def test_call_records_constant_arguments(self):
        """r0-r3 at a direct call, including a negative constant built with mvn."""
        t = fx.Thumb().push_r7_lr().movs(0, 2).h(0xF06F, 0x0104)  # mvn r1, #4
        site = t.here
        t.bl(fx.STUB_FOO).pop_r7_pc()
        self.assertIn("mvn r1, #4", disasm(t))
        args = scan(program(t)).call_args[site]
        self.assertEqual(args[0][:2], ("k", 2))
        self.assertEqual(args[1][:2], ("k", 0xFFFFFFFB))
        self.assertEqual(args[2:], (None, None))

    def test_undecodable_bytes_are_skipped(self):
        t = fx.Thumb().h(0xFFFF, 0xFFFF).pop_r7_pc()
        result = scan(program(t))
        self.assertGreater(result.skipped_bytes, 0)
        self.assertEqual(result.functions, 1)

    def test_function_index(self):
        index = armscan.FunctionIndex([0x2001, 0x1001, 0x3000])
        self.assertEqual(index.of(0x1000), 0x1001)
        self.assertEqual(index.of(0x2FFF), 0x2001)
        self.assertEqual(index.of(0x3002), 0x3000)
        self.assertIsNone(index.of(0x0FFF))
        self.assertEqual(index.next_start(0x1001), 0x2000)
        self.assertEqual(index.next_start(0x0FFF), 0x1000)
        self.assertEqual(index.next_start(0x2FFE), 0x3000)
        self.assertIsNone(index.next_start(0x3000))


class ClassifyTest(unittest.TestCase):
    def test_reachability(self):
        # 1 -> 2 -> 4, 3 (Chartboost) -> 4, 3 -> 5, 6 (dead) -> 7, 8 <-> 9 (no way in)
        graph = {1: {2}, 2: {4}, 3: {4, 5}, 4: set(), 5: set(), 6: {7}, 7: set(), 8: {9}, 9: {8}}
        out = imports.classify_functions(graph, graph, chartboost=[3], dead=[6])
        self.assertEqual(
            out,
            {
                1: "game",
                2: "game",
                3: "chartboost",
                4: "both",
                5: "chartboost",
                6: "unreferenced",
                7: "unreferenced",
                8: "game",
                9: "game",
            },
        )

    def test_import_class(self):
        self.assertEqual(imports.import_class(True, True), "both")
        self.assertEqual(imports.import_class(False, True), "chartboost")
        self.assertEqual(imports.import_class(True, False), "game")
        self.assertEqual(imports.import_class(False, False), "unreferenced")

    def test_category_methods_follow_their_senders(self):
        cb_imp, used_by_cb, unsent, used_by_game, game_fn = 0x101, 0x201, 0x301, 0x401, 0x501
        image = objc.ObjCImage(
            classes=[
                objc.ObjCClass(
                    0x9000,
                    "CBThing",
                    0x9100,
                    None,
                    0,
                    0,
                    0,
                    instance_methods=[objc.Method("go", "v8@0:4", cb_imp)],
                )
            ],
            categories=[
                objc.Category(
                    0x9200,
                    "Extras",
                    objc.ClassRef("NSArray", None),
                    instance_methods=[
                        objc.Method("each:", "v12@0:4@8", used_by_cb),
                        objc.Method("filter:", "@12@0:4@8", unsent),
                        objc.Method("keys", "@8@0:4", used_by_game),
                    ],
                )
            ],
            protocols=[],
            selrefs=[],
            classrefs=[],
            superrefs=[],
            protorefs=[],
        )
        functions = [cb_imp, used_by_cb, unsent, used_by_game, game_fn]
        graph = {f: set() for f in functions}
        senders = {"each:": {cb_imp, unsent}, "keys": {game_fn}}
        seeds, classes, cats = imports.chartboost_entries(image, senders, functions, graph)
        self.assertEqual(seeds, {cb_imp, used_by_cb})
        self.assertEqual(classes[unsent], "unreferenced")
        self.assertEqual(classes[used_by_game], "game")
        self.assertEqual(cats, {"NSArray(Extras)": "both"})


def ghidra_for(image: macho.MachO, xrefs=()) -> imports.Ghidra:
    starts = image.function_starts
    return imports.Ghidra(
        {s: f"fn_{s:x}" for s in starts}, {s: "names" for s in starts}, list(xrefs)
    )


class EndToEndTest(unittest.TestCase):
    """A two-function image: main (game) calls _foo and loads _kConst; CBThing's IMP calls _bar."""

    def setUp(self):
        t = fx.Thumb().push_r7_lr()
        self.call_foo = t.here
        t.blx(fx.STUB_FOO)
        lit = t.here
        t.ldr_lit(0, None)
        add = t.here
        t.add_pc(0)
        self.load = t.here
        t.ldr(1, 0).pop_r7_pc().align4()
        pool = t.here
        t.word(fx.NL - (add + 4))
        t.code[lit - t.base : lit - t.base + 2] = fx.Thumb(lit).ldr_lit(0, pool).code
        self.cb = t.here
        t.push_r7_lr()
        self.call_bar = t.here
        t.blx(fx.STUB_BAR).pop_r7_pc()
        self.image = program(t, [self.cb | 1])
        self.ghidra = ghidra_for(
            self.image,
            [
                {
                    "from": hexs(self.call_foo),
                    "to": hexs(fx.STUB_FOO),
                    "type": "UNCONDITIONAL_CALL",
                },
                {"from": hexs(self.load), "to": hexs(fx.NL), "type": "READ"},
                {
                    "from": hexs(self.call_bar),
                    "to": hexs(fx.STUB_BAR),
                    "type": "UNCONDITIONAL_CALL",
                },
            ],
        )

    def analyse(self) -> imports.Analysis:
        table = imports.import_table(self.image)
        s = armscan.scan(
            self.image, [a for i in table if i.name in imports.DISPATCHERS for a in i.stubs]
        )
        oimg = objc.ObjCImage(
            classes=[
                objc.ObjCClass(
                    0x9000,
                    "CBThing",
                    0x9100,
                    None,
                    0,
                    0,
                    0,
                    instance_methods=[objc.Method("go", "v8@0:4", self.cb | 1)],
                )
            ],
            categories=[],
            protocols=[],
            selrefs=[],
            classrefs=[],
            superrefs=[],
            protorefs=[],
        )
        return imports.analyse(self.image, oimg, s, self.ghidra)

    def test_classes_and_outputs(self):
        a = self.analyse()
        self.assertEqual(a.classes["_foo"], "game")
        self.assertEqual(a.classes["_kConst"], "game")
        self.assertEqual(a.classes["_bar"], "chartboost")
        self.assertEqual(a.classes["_unused"], "unreferenced")
        self.assertEqual(a.classes["_free"], "game")  # static data, no code loads it
        c = imports.counts(a)
        self.assertEqual((c["imports"], c["hle"], c["ghidra_agree"]), (7, c["game"] + c["both"], 7))
        doc = imports.to_json(a)
        self.assertEqual(len(doc["imports"]), 7)
        md = imports.to_markdown(a)
        for name, _ in fx.UNDEFINED:
            self.assertEqual(md.count(f"| `{name}` |"), 1, name)
        self.assertIn(f"**The HLE layer must implement {c['hle']} of the 7 imports**", md)

    def test_ghidra_disagreement_is_reported(self):
        self.ghidra.xrefs.pop(1)
        a = self.analyse()
        self.assertEqual(a.ghidra_check["_kConst"]["only_capstone"], [self.load])
        self.assertIn("`_kConst`: Capstone 1, Ghidra 0", imports.to_markdown(a))

    def test_names_file_only_for_unnamed_stubs(self):
        a = self.analyse()
        g = imports.Ghidra(
            {fx.STUB_FOO: "_foo", fx.STUB_BAR: "FUN_00002f08"},
            {fx.STUB_FOO: "imported", fx.STUB_BAR: "default"},
            [],
        )
        doc = imports.names_json(a, g)
        names.parse(doc, "imports.json")
        self.assertEqual(
            [(e["address"], e["name"]) for e in doc["names"]],
            [(hexs(fx.STUB_MSGSEND), "_objc_msgSend"), (hexs(fx.STUB_BAR), "_bar")],
        )


def hexs(value: int) -> str:
    return f"0x{value:X}"


if __name__ == "__main__":
    unittest.main()
