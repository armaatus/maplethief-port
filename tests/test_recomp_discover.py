"""Function discovery (#32) on hand-assembled Thumb-2: jump tables bounded by their ``cmp``
(``tbb``/``tbh``, ``ldr pc``, ``add pc``), tail calls, noreturn calls, literal pools, SjLj
resume entries, NEON stepped over, the whole-image pass on a synthetic Mach-O, the JSON and
the Ghidra cross-check. The switches are also lifted, compiled and run (MSVC on the Windows
runner). No game code."""

from __future__ import annotations

import json
import struct

import pytest

from machofixtures import thin_image
from recomp import discovery_report
from recomp.discover import (
    Discovery,
    JumpTable,
    discover,
    discover_function,
)
from recomp.lift.core import LiftOptions, lift_function
from recomp.loader import Image
from recomp.snippet import _SnippetProgram, lift_snippet
from recomp_harness import SNIPPET_BASE, run_snippet
from thumbasm import Thumb

HI, LS, HS, LO = 8, 9, 2, 3
BASE = 0x1400  # __text of machofixtures.thin_image


def assemble(build, base: int = BASE) -> tuple[bytes, dict[str, int]]:
    """Two passes: ``build(t, at, mark)`` assembles into ``t``; ``mark(name)`` defines a
    label here and ``at(name)`` is its address (a harmless guess in the first pass)."""
    labels: dict[str, int] = {}
    for _ in range(2):
        t = Thumb(base)
        marks: dict[str, int] = {}

        def at(name: str, t=t, labels=labels) -> int:
            return labels.get(name, (t.here + 7) & ~3)

        def mark(name: str, t=t, marks=marks) -> None:
            marks[name] = t.here

        build(t, at, mark)
        labels = marks
    return bytes(t.code), labels


def disc(code: bytes, base: int = BASE, **kw):
    return discover_function(code, base, base | 1, base + len(code), **kw)


def cases(t: Thumb, at, mark, n: int) -> None:
    """``case<i>: movs r1, #10 * (i + 1); bx lr`` and ``default: movs r1, #99; bx lr``."""
    for i in range(n):
        mark(f"case{i}")
        t.movs(1, 10 * (i + 1)).bx_lr()
    mark("default")
    t.movs(1, 99).bx_lr()


def tbb_switch(t: Thumb, at, mark) -> None:
    t.cmp(0, 2).bcond(HI, at("default"))
    mark("site")
    t.tbb(0)
    site = at("site")
    t.bytes8(*((at(f"case{i}") - (site + 4)) // 2 for i in range(3)), 0)
    cases(t, at, mark, 3)


def test_tbb_bounded_by_cmp_and_bhi() -> None:
    code, lab = assemble(tbb_switch)
    fn = disc(code)
    assert not fn.problems
    table = fn.tables[lab["site"]]
    assert (table.kind, table.count, table.index, table.bound) == ("tbb", 3, 0, BASE)
    assert table.cases == tuple((i, lab[f"case{i}"]) for i in range(3))
    assert fn.succ[lab["site"]] == table.targets
    assert {lab[f"case{i}"] for i in range(3)} <= fn.labels
    assert fn.data[lab["site"] + 4] == (4, "table")  # 3 entries, padded to a halfword
    assert all(lab[k] in fn.insns for k in ("case0", "case1", "case2", "default"))
    exits = {b.start: b.exit for b in fn.blocks()}
    assert exits[lab["site"] - 4] == "cond" and exits[lab["site"]] == "table"
    assert exits[lab["case1"]] == "return"


@pytest.mark.parametrize("r0", [0, 1, 2, 3, 0xFFFFFFFF])
def test_tbb_switch_lifts_and_runs(tmp_path, r0: int) -> None:
    code, _ = assemble(tbb_switch, SNIPPET_BASE)
    out = run_snippet(tmp_path, code, {"r0": r0})
    assert out["r1"] == (10 * (r0 + 1) if r0 < 3 else 99)


def o0_tbh_switch(t: Thumb, at, mark) -> None:
    """The -O0 shape: the index goes through stack slots and another register is compared."""
    t.adds(0, 0, 0)  # adds r0, r0, #0 (a computed index)
    t.str_sp(0, 16).ldr_sp(1, 16)  # str r0, [sp, #16] / ldr r1, [sp, #16]
    mark("cmp")
    t.cmp(1, 3)  # cmp r1, #3: r1 is a copy of the index
    t.str_sp(0, 12)  # str r0, [sp, #12]
    t.bcond(HI, at("default"))
    t.ldr_sp(1, 12).ldr_sp(1, 12)  # ldr r1, [sp, #12] (twice, as -O0 does)
    mark("site")
    t.tbh(1)
    site = at("site")
    t.raw(struct.pack("<4H", *((at(f"case{i}") - (site + 4)) // 2 & 0xFFFF for i in range(4))))
    cases(t, at, mark, 4)


def test_tbh_index_followed_through_stack_slots() -> None:
    code, lab = assemble(o0_tbh_switch)
    fn = disc(code)
    assert not fn.problems
    table = fn.tables[lab["site"]]
    assert (table.kind, table.count, table.index, table.bound) == ("tbh", 4, 1, lab["cmp"])
    assert fn.data[lab["site"] + 4] == (8, "table")


@pytest.mark.parametrize("r0", [0, 3, 4])
def test_tbh_switch_lifts_and_runs(tmp_path, r0: int) -> None:
    code, _ = assemble(o0_tbh_switch, SNIPPET_BASE)
    out = run_snippet(tmp_path, code, {"r0": r0})
    assert out["r1"] == (10 * (r0 + 1) if r0 < 4 else 99)


def test_table_reached_by_the_taken_side_of_bls() -> None:
    def build(t, at, mark):
        t.cmp(0, 1).bcond(LS, at("site")).b(at("default"))
        mark("site")
        t.tbb(0)
        site = at("site")
        t.bytes8(*((at(f"case{i}") - (site + 4)) // 2 for i in range(2)))
        cases(t, at, mark, 2)

    code, lab = assemble(build)
    fn = disc(code)
    assert not fn.problems
    assert fn.tables[lab["site"]].count == 2


def test_bhs_bounds_strictly() -> None:
    def build(t, at, mark):
        t.cmp(0, 2).bcond(HS, at("default"))
        mark("site")
        t.tbb(0)
        site = at("site")
        t.bytes8(*((at(f"case{i}") - (site + 4)) // 2 for i in range(2)))
        cases(t, at, mark, 2)

    code, lab = assemble(build)
    assert disc(code).tables[lab["site"]].count == 2


@pytest.mark.parametrize("guard", ["none", "wrong-side", "flags-clobbered", "index-changed"])
def test_unbounded_tables_are_problems(guard: str) -> None:
    def build(t, at, mark):
        if guard == "wrong-side":
            t.cmp(0, 2).bcond(LS, at("default"))  # falls through only when r0 > 2
        elif guard == "flags-clobbered":
            t.cmp(0, 2).cmp(1, 0).bcond(HI, at("default"))
        elif guard == "index-changed":
            t.cmp(0, 2).bcond(HI, at("default")).adds(0, 0, 1)
        mark("site")
        t.tbb(0)
        t.bytes8(4, 6)
        cases(t, at, mark, 1)

    code, lab = assemble(build)
    fn = disc(code)
    assert lab["site"] not in fn.tables
    assert any("tbb jump table not resolved" in p for p in fn.problems), fn.problems


def ldr_pc_switch(t: Thumb, at, mark) -> None:
    t.cmp(0, 1).bcond(HI, at("default"))
    t.adr(1, at("table"))
    mark("site")
    t.ldr_lsl2(15, 1, 0)  # ldr.w pc, [r1, r0, lsl #2]
    t.align4()
    mark("table")
    t.word(at("case0") | 1).word(at("case1") | 1)
    cases(t, at, mark, 2)


def add_pc_switch(t: Thumb, at, mark) -> None:
    t.cmp(0, 1).bcond(HI, at("default"))
    t.adr(1, at("table"))
    mark("load")
    t.ldr_lsl2(2, 1, 0)  # ldr.w r2, [r1, r0, lsl #2]
    mark("site")
    t.add_pc_reg(2)  # add pc, r2
    t.align4()
    mark("table")
    site = at("site")
    t.word(at("case0") - (site + 4)).word(at("case1") - (site + 4))
    cases(t, at, mark, 2)


@pytest.mark.parametrize(("build", "kind"), [(ldr_pc_switch, "ldr"), (add_pc_switch, "add")])
def test_ldr_pc_and_add_pc_tables(build, kind: str) -> None:
    code, lab = assemble(build)
    fn = disc(code)
    assert not fn.problems
    table = fn.tables[lab["site"]]
    assert (table.kind, table.count, table.base) == (kind, 2, lab["table"])
    assert table.targets == (lab["case0"], lab["case1"])
    assert fn.data[lab["table"]] == (8, "table")
    if kind == "add":  # the switch is on the loaded offset
        assert table.index == 2
        assert [v for v, _ in table.cases] == [t - (lab["site"] + 4) for t in table.targets]
    assert not fn.indirect


@pytest.mark.parametrize("build", [ldr_pc_switch, add_pc_switch], ids=["ldr-pc", "add-pc"])
@pytest.mark.parametrize("r0", [0, 1, 2])
def test_ldr_pc_and_add_pc_tables_run(tmp_path, build, r0: int) -> None:
    code, _ = assemble(build, SNIPPET_BASE)
    # add pc: the offset is loaded at run time, so the table must be in guest memory
    out = run_snippet(tmp_path, code, {"r0": r0}, memory={SNIPPET_BASE: code})
    assert out["r1"] == (10 * (r0 + 1) if r0 < 2 else 99)


def test_indirect_jump_that_is_no_table() -> None:
    code = Thumb(BASE).mov(15, 3).code  # mov pc, r3
    fn = disc(bytes(code))
    assert fn.indirect == {BASE} and not fn.tables and not fn.problems


def test_tail_calls() -> None:
    t = Thumb(BASE).movs(0, 1)
    t.b(BASE + 0x20)  # b to the next function
    code = bytes(t.code)
    end = BASE + len(code)
    fn = discover_function(code, BASE, BASE | 1, end, starts={BASE, BASE + 0x20})
    assert fn.tail_calls == {BASE + 2: BASE + 0x21} and not fn.problems
    assert fn.blocks()[-1].exit == "tail"
    fn = discover_function(code, BASE, BASE | 1, end, starts={BASE})
    assert any("not a function start" in p for p in fn.problems)
    fn = discover_function(code, BASE, BASE | 1, end)  # no starts: any branch out is a tail call
    assert not fn.problems


def noreturn_caller(t: Thumb, at, mark) -> None:
    t.push_r7_lr()
    t.cbz(0, at("out"))
    mark("call")
    t.bl(0x3000)
    t.h(0xDEFE, 0xDEFE)  # trap / trap: what follows a noreturn call is not code
    mark("out")
    t.pop_r7_pc()


def test_noreturn_call_ends_the_path() -> None:
    code, lab = assemble(noreturn_caller)
    fn = disc(code, noreturn={0x3000})
    assert fn.noreturn_calls == {lab["call"]}
    assert fn.calls == {lab["call"]: 0x3001}
    assert lab["call"] + 4 not in fn.insns and lab["out"] in fn.insns
    assert fn.succ[lab["call"]] == ()
    lifted = lift_function(fn, _SnippetProgram(code, BASE), LiftOptions())
    assert "MT_TRAP(MT_TRAP_UNREACHABLE" in lifted.source
    assert not lifted.problems
    fn = disc(code)  # not known to be noreturn: the path runs into the traps
    assert lab["call"] + 4 in fn.insns


def test_literal_pools_are_data() -> None:
    def build(t, at, mark):
        t.ldr_lit(0, at("pool"))
        t.h(0xED9F, 0x0B02)  # vldr d0, [pc, #8]: Align(pc, 4) + 8, after the first word
        t.bx_lr().align4()
        mark("pool")
        t.word(0x12345678).word(1).word(2)

    code, lab = assemble(build)
    fn = disc(code)
    assert fn.data == {lab["pool"]: (4, "literal"), lab["pool"] + 4: (8, "literal")}
    assert not fn.problems
    assert fn.extent == lab["pool"] + 12


def test_code_running_into_a_literal_pool_is_a_problem() -> None:
    def build(t, at, mark):
        t.ldr_lit(0, at("pool"))
        t.cbz(0, at("pool"))  # a branch into the pool
        t.bx_lr().align4()
        mark("pool")
        t.word(0x47704770)

    code, lab = assemble(build)
    fn = disc(code)
    assert any("overlaps literal data" in p for p in fn.problems), fn.problems


def test_sjlj_resume_point_is_an_entry() -> None:
    def build(t, at, mark):
        mark("mov")
        t.mov(4, 15)  # mov r4, pc       r4 = mov + 4
        t.adds(4, 4, 7)  # adds r4, r4, #7  r4 = mov + 11: Thumb code at mov + 10
        t.h(0x606C)  # str r4, [r5, #4]  into the jump buffer
        t.movs(0, 0)
        t.b(at("join"))
        mark("resume")
        t.movs(0, 1)  # reached only when the unwinder resumes here
        mark("join")
        t.bx_lr()

    code, lab = assemble(build)
    assert lab["resume"] == lab["mov"] + 10
    fn = disc(code)
    assert set(fn.entries) == {lab["resume"]} and lab["resume"] in fn.insns
    assert lab["resume"] not in fn.labels  # #37 emits its label with the resume hook
    assert not fn.problems
    assert lab["resume"] in {b.start for b in fn.blocks()}


def sjlj_dispatch(t: Thumb, at, mark) -> None:
    """Chartboost's (clang) SjLj shape: the resume address is ``ldr r1, =k; orr r1, #1;
    add r1, pc``; the landing code switches on the call-site index through a table of
    ``b.w`` instructions (``addw r1, pc, #k; add.w r1, r1, r0, lsl #2; mov pc, r1``)."""
    t.ldr_lit(1, at("pool"))
    t.orr_imm8(1, 1, 1)  # orr.w r1, r1, #1
    mark("addpc")
    t.add_pc(1)  # add r1, pc: r1 = the resume address, Thumb bit set
    t.h(0x6069)  # str r1, [r5, #4]: into the jump buffer
    t.movs(1, 7).bx_lr()  # the normal path returns
    mark("resume")
    t.cmp(0, 2).bcond(HI, at("default"))
    t.addw_pc(1, at("table"))
    t.add_lsl2(1, 1, 0)
    mark("site")
    t.mov(15, 1)  # mov pc, r1
    mark("table")
    for i in range(3):
        t.b_w(at(f"case{i}"))
    cases(t, at, mark, 3)
    t.align4()
    mark("pool")
    t.word(at("resume") + 1 - (at("addpc") + 4))


def test_branch_table_and_resume_through_a_literal() -> None:
    code, lab = assemble(sjlj_dispatch)
    fn = disc(code)
    assert not fn.problems
    assert set(fn.entries) == {lab["resume"]}
    table = fn.tables[lab["site"]]
    assert (table.kind, table.count, table.index, table.base) == ("branch", 3, 0, lab["table"])
    assert table.targets == tuple(lab["table"] + 4 * i for i in range(3))
    assert lab["table"] not in fn.data  # the table is code: one b.w per case
    assert all(lab[f"case{i}"] in fn.insns for i in range(3)) and not fn.indirect
    assert fn.data == {lab["pool"]: (4, "literal")}


@pytest.mark.parametrize("r0", [0, 2, 3])
def test_branch_table_runs(tmp_path, r0: int) -> None:
    def build(t, at, mark):  # enter the landing code the way the unwinder would
        t.b_w(at("resume"))
        sjlj_dispatch(t, at, mark)

    code, _ = assemble(build, SNIPPET_BASE)
    out = run_snippet(tmp_path, code, {"r0": r0}, memory={SNIPPET_BASE: code})
    assert out["r1"] == (10 * (r0 + 1) if r0 < 3 else 99)


def test_gaps_in_the_report() -> None:
    def build(t, at, mark):
        t.adr(0, at("floats"))  # a table read through a register, not a literal load
        t.h(0xEDD0, 0x0A00)  # vldr s1, [r0]
        t.cbz(0, at("call"))
        t.b(at("out"))
        mark("dead")
        t.movs(0, 3)  # no path, no pointer
        mark("call")
        t.bl(0x3000)  # noreturn
        mark("after")
        t.movs(0, 4)
        mark("out")
        t.bx_lr().nop().align4()
        mark("floats")
        t.word(0x3F800000).word(0x40000000)

    code, lab = assemble(build)
    fn = disc(code, noreturn={0x3000})
    assert fn.data_refs == {BASE: lab["floats"]}
    found = {(lo, why) for lo, _, why in discovery_report.gaps(fn, code, BASE)}
    assert found == {
        (lab["dead"], "unreached"),
        (lab["after"], "after-noreturn"),
        (lab["out"] + 2, "addressed-data"),  # the alignment nop and the floats after it
    }
    pad = bytes(Thumb(BASE).bx_lr().nop().code)
    assert discovery_report.gaps(disc(pad), pad, BASE) == [(BASE + 2, BASE + 4, "padding")]


def test_neon_is_stepped_over() -> None:
    code = bytes.fromhex("20ef1001") + bytes(Thumb(BASE + 4).movs(0, 1).bx_lr().code)
    fn = disc(code)  # vorr d0, d0, d0 (NEON, decoded by #34) / movs r0, #1 / bx lr
    assert sorted(fn.insns) == [BASE, BASE + 4, BASE + 6] and not fn.problems
    snippet = lift_snippet(code, SNIPPET_BASE)
    assert len(snippet.unsupported) == 1  # still a trap in place for the lifter


def test_arm_function_is_a_listed_failure() -> None:
    fn = discover_function(b"\x1e\xff\x2f\xe1", BASE, BASE, BASE + 4)
    assert fn.mode == "arm" and "ARM-mode function" in fn.problems[0]


# -- the whole image -------------------------------------------------------------------------

F1, F2, F3 = BASE + 4, BASE + 0x20, BASE + 0x40  # Thumb functions after the ARM entry stub


def synthetic_image() -> tuple[Image, dict[str, int]]:
    """An ARM entry stub (``bx lr``) at 0x1400, then F1 (calls F2 unless r0 == 0), F2 (calls
    F3), F3 (``b .``: never returns), and F4 at 0x1460 with a tbb switch."""
    text = bytearray(b"\x1e\xff\x2f\xe1")
    code1, lab = assemble(noreturn_caller_to(F2), F1)
    text += b"\0" * (F1 - BASE - len(text)) + code1
    f2 = Thumb(F2).push_r7_lr().bl(F3).pop_r7_pc()
    text += b"\0" * (F2 - BASE - len(text)) + f2.code
    f3 = Thumb(F3).b(F3)
    text += b"\0" * (F3 - BASE - len(text)) + f3.code
    code4, lab4 = assemble(tbb_switch, BASE + 0x60)
    text += b"\0" * (0x60 - len(text)) + code4
    starts = [BASE, F1 | 1, F2 | 1, F3 | 1, (BASE + 0x60) | 1]
    lab.update({f"f4.{k}": v for k, v in lab4.items()})
    return Image(thin_image(starts, bytes(text))), lab


def noreturn_caller_to(target: int):
    def build(t, at, mark):
        t.push_r7_lr()
        t.cbz(0, at("out"))
        mark("call")
        t.bl(target)
        t.h(0xDEFE, 0xDEFE)
        mark("out")
        t.pop_r7_pc()

    return build


def test_whole_image_discovery_and_noreturn_fixpoint() -> None:
    image, lab = synthetic_image()
    found = discover(image)
    assert sorted(found.functions) == [BASE, F1 | 1, F2 | 1, F3 | 1, (BASE + 0x60) | 1]
    assert [p for p, _ in found.failures()] == [BASE]  # the ARM stub, with its reason
    # F3 loops forever, so F2 (which only calls it) never returns either; F1's call to F2
    # then ends its path, and F1 still returns through its cbz.
    assert {a: w for a, w in found.noreturn.items()} == {
        F2: "function never returns",
        F3: "function never returns",
    }
    f1 = found.functions[F1 | 1]
    assert f1.noreturn_calls == {lab["call"]} and not f1.noreturn
    assert lab["call"] + 4 not in f1.insns
    assert found.functions[F2 | 1].noreturn and found.functions[F3 | 1].noreturn
    assert found.seeds[F2 | 1] == {"function_starts", "call"}
    assert found.seeds[BASE] == {"function_starts", "entry"}
    assert not found.outside_starts()
    assert found.containing(lab["f4.case1"]).ptr == (BASE + 0x60) | 1
    counts = found.counts()
    assert (counts["functions"], counts["failed"], counts["tables"]) == (5, 1, 1)


def test_functions_json() -> None:
    image, lab = synthetic_image()
    found = discover(image)
    doc = json.loads(found.to_json({"path": None, "arch": "armv7", "sha256": "0"}))
    assert doc["schema"] == 1 and doc["counts"]["functions"] == 5
    by_ptr = {f["ptr"]: f for f in doc["functions"]}
    f4 = by_ptr[f"0x{(BASE + 0x60) | 1:X}"]
    (table,) = f4["tables"]
    assert table["kind"] == "tbb" and table["count"] == 3
    assert [c[1] for c in table["cases"]] == [f"0x{lab[f'f4.case{i}']:X}" for i in range(3)]
    assert f4["data"] == [[f"0x{lab['f4.site'] + 4:X}", 4, "table"]]
    assert by_ptr[f"0x{BASE:X}"]["problems"] and by_ptr[f"0x{BASE:X}"]["mode"] == "arm"
    f1 = by_ptr[f"0x{F1 | 1:X}"]
    assert f1["noreturn_calls"] == [f"0x{lab['call']:X}"]
    blocks = f1["blocks"]
    assert blocks[0]["start"] == f"0x{F1:X}" and {b["exit"] for b in blocks} >= {"noreturn"}
    assert {n["address"] for n in doc["noreturn"]} == {f"0x{F2:X}", f"0x{F3:X}"}


def test_ghidra_cross_check_explains_differences() -> None:
    image, lab = synthetic_image()
    found = discover(image)

    def fn(ptr: int, ranges: list[tuple[int, int]], **kw) -> dict:
        body = [[f"0x{a:X}", f"0x{b:X}"] for a, b in ranges]
        return {"address": f"0x{ptr:X}", "body": body, "section": "__TEXT,__text",
                "block": "__text", "no_return": False, **kw}  # fmt: skip

    f4 = (BASE + 0x60) | 1
    f4_fn = found.functions[f4]
    export = {
        "functions": [
            fn(BASE, [(BASE, BASE + 4)]),  # ARM: discovery does not decode it yet
            fn(F1 | 1, [(F1, F1 | 1), (F1 + 1, lab["out"] + 2)]),  # + the 4 B after the call
            fn(F2 | 1, [(F2, F2 + 8)], no_return=True),
            fn(F3 | 1, [(F3, F3 + 2)], no_return=True),
            # Ghidra decodes the table as code, but misses the default case: unexplained
            fn(f4, [(f4 & ~1, lab["f4.default"])]),
            fn(lab["f4.site"] + 5, [(lab["f4.site"] + 4, lab["f4.site"] + 8)]),  # table data
        ]
    }
    diff = discovery_report.compare(found, export, image)
    by = {d.ptr: d for d in diff.diffs}
    assert by[BASE].ghidra_only == {"arm": 2}
    assert by[F1 | 1].ghidra_only == {"after-noreturn": 2}
    assert by[f4].ghidra_only == {"data": 2}
    assert by[f4].ours_only["unexplained"] == len(range(lab["f4.default"], f4_fn.extent, 2))
    assert [d.ptr for d in diff.unexplained] == [f4]
    assert diff.only_ghidra == [(lab["f4.site"] + 5, f"data of 0x{f4:X} (literal pool or table)")]
    assert diff.noreturn_only_ghidra == [] and diff.noreturn_only_ours == []
    assert diff.unexplained_share() == pytest.approx(1 / 5)
    xrefs = {"xrefs": [{"from": f"0x{lab['f4.site']:X}", "to": f"0x{lab[f'f4.case{i}']:X}",
                        "type": "COMPUTED_JUMP"} for i in range(2)]}  # fmt: skip
    discovery_report.compare_tables(found, xrefs, diff)
    assert diff.tables_agree == 0
    assert diff.tables_differ == [(f4, lab["f4.site"], [lab["f4.case2"]], [])]
    text = discovery_report.render(found, image, diff)
    assert "1 unexplained (20.00%, **over** the 1% target)" in text
    assert "0 of 1 have exactly the same targets" in text


def test_jump_table_value_object() -> None:
    t = JumpTable(0x10, "tbb", 0x14, 1, 3, 0, 0x8, ((0, 0x20), (1, 0x18), (2, 0x20)))
    assert t.targets == (0x18, 0x20) and t.size == 4


def test_discovery_without_functions() -> None:
    empty = Discovery({}, {}, {}, ())
    assert empty.containing(0x1000) is None and empty.failures() == []
