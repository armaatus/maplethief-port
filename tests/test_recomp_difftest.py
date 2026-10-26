"""The differential test (#41, ADR 0017) on a synthetic Mach-O: leaf selection, the seeded
inputs, the C driver (built with the host compiler, MSVC on the Windows runner) and the
comparison. These run in CI without Unicorn. The tests at the end run the whole pipeline
against Unicorn on the same synthetic image; they need the ``difftest`` group
(``scripts/difftest --pytest``) and skip without it. No game code."""

from __future__ import annotations

import struct
from types import SimpleNamespace

import pytest

from machofixtures import thin_image
from recomp import difftest
from recomp.difftest import host, inputs
from recomp.difftest.inputs import HEAP, HEAP_SIZE, RET, SP0, WINDOW, Case
from recomp.difftest.select import pointer_regs, select_leaves
from recomp.discover import discover
from recomp.loader import Image
from recomp_harness import host_cc
from thumbasm import Thumb

BASE = 0x1400  # __text of machofixtures.thin_image; an ARM `bx lr` (the entry point)
F_ADD = 0x1410  # adds r0, r0, r1; str r0, [r2]; bx lr
F_CALL = 0x1420  # push {r7, lr}; bl F_ADD; pop {r7, pc}
F_SVC = 0x1430  # svc #0; bx lr                      (not plain)
F_LOOP = 0x1440  # b .                                (never returns)
F_BAD = 0x1450  # push {r7, lr}; bl F_SVC; pop {r7, pc}  (calls a non-leaf)
F_LDR = 0x1460  # ldr r0, [r0]; bx lr
F_COUNT = 0x1470  # loop: subs r0, #1; bne loop; bx lr
LEAVES = [BASE, F_ADD | 1, F_CALL | 1, F_LDR | 1, F_COUNT | 1]


def synthetic_text() -> bytes:
    text = bytearray(struct.pack("<I", 0xE12FFF1E))  # ARM bx lr

    def put(addr: int, t: Thumb) -> None:
        text.extend(b"\0" * (addr - BASE - len(text)))
        text.extend(t.code)

    put(F_ADD, Thumb(F_ADD).h(0x1840, 0x6010).bx_lr())
    put(F_CALL, Thumb(F_CALL).push_r7_lr().bl(F_ADD).pop_r7_pc())
    put(F_SVC, Thumb(F_SVC).h(0xDF00).bx_lr())
    put(F_LOOP, Thumb(F_LOOP).b(F_LOOP))
    put(F_BAD, Thumb(F_BAD).push_r7_lr().bl(F_SVC).pop_r7_pc())
    put(F_LDR, Thumb(F_LDR).h(0x6800).bx_lr())
    put(F_COUNT, Thumb(F_COUNT).h(0x3801).bcond(1, F_COUNT).bx_lr())
    return bytes(text)


STARTS = [BASE, F_ADD | 1, F_CALL | 1, F_SVC | 1, F_LOOP | 1, F_BAD | 1, F_LDR | 1, F_COUNT | 1]


@pytest.fixture(scope="module")
def macho() -> bytes:
    return thin_image(STARTS, synthetic_text())


@pytest.fixture(scope="module")
def image(macho) -> Image:
    return Image(macho)


@pytest.fixture(scope="module")
def selection(image):
    return select_leaves(image, discover(image))


def test_leaf_selection(selection) -> None:
    assert sorted(selection.leaves) == LEAVES
    assert selection.rejected == {F_SVC | 1: "plain", F_LOOP | 1: "returns", F_BAD | 1: "leaf"}
    assert selection.counts == {
        "discovered": 8, "returns": 7, "closed": 7, "plain": 6, "clean": 6, "leaf": 5,
    }  # fmt: skip
    add, ldr = selection.leaves[F_ADD | 1], selection.leaves[F_LDR | 1]
    assert pointer_regs(add.fn) == {2} and pointer_regs(ldr.fn) == {0}
    assert selection.leaves[F_CALL | 1].has_calls and not add.has_calls
    assert not any(lf.vfp for lf in selection.leaves.values())


def test_inputs_are_seeded_and_inside_the_window(image) -> None:
    data = inputs.data_range(image)
    a = inputs.make_case(41, F_ADD | 1, 7, frozenset({2}), True, data)
    b = inputs.make_case(41, F_ADD | 1, 7, frozenset({2}), True, data)
    c = inputs.make_case(41, F_ADD | 1, 8, frozenset({2}), True, data)
    assert a == b and a != c and a.seed == "41:0x1411:7"
    assert a.regs[13:] == [SP0, RET | 1, 0] and a.cpsr & 0x0FFFFFFF == 0
    pool = inputs.heap_pool(41, F_ADD | 1)
    assert pool == inputs.heap_pool(41, F_ADD | 1) and len(pool) == inputs.POOL
    words = struct.unpack(f"<{HEAP_SIZE // 4}I", pool[0])
    pointers = sum(HEAP <= w < HEAP + HEAP_SIZE and w % 8 == 0 for w in words)
    assert 0.4 < pointers / len(words) < 0.6
    hits = 0
    for i in range(200):
        case = inputs.make_case(41, F_ADD | 1, i, frozenset({2}), False, data)
        hits += HEAP <= case.regs[2] < HEAP + HEAP_SIZE
        for addr, blob in case.patches:
            assert 0 <= addr and addr + len(blob) <= WINDOW
    assert hits > 150  # a memory base register gets heap pointers most of the time
    window = inputs.pristine(image)
    assert len(window) == WINDOW and window[BASE : BASE + 4] == struct.pack("<I", 0xE12FFF1E)
    assert window[0x3000:0x3006] == b"hello\0" and not any(window[HEAP:])


def case(regs: dict[int, int], cpsr: int = 0) -> Case:
    r = [0] * 16
    r[13], r[14] = SP0, RET | 1
    for n, v in regs.items():
        r[n] = v
    return Case("test", r, cpsr, 0, [n * 0x0101010101010101 for n in range(32)], 0)


@pytest.fixture(scope="module")
def driver(image, selection, tmp_path_factory):
    host_cc()  # skips without a C compiler
    out = tmp_path_factory.mktemp("difftest")
    leaves = list(selection.leaves.values())
    exe = host.build(image, leaves, out / "build", jobs=2)
    pristine = out / "pristine.bin"
    pristine.write_bytes(inputs.pristine(image))
    index = {lf.ptr: i for i, lf in enumerate(leaves)}

    def run(ptr: int, cases: list[Case]) -> dict:
        path, res = out / f"{ptr:X}.in", out / f"{ptr:X}.out"
        host.write_cases(path, index[ptr], [bytes(HEAP_SIZE)], list(enumerate(cases)))
        assert host.run_driver(exe, pristine, path, res, 60) == ""
        return host.read_results(res)

    return run


def test_driver_runs_both_variants(driver) -> None:
    res = driver(
        F_ADD | 1,
        [case({0: 5, 1: 7, 2: HEAP + 16}, 0xF0000000), case({0: 0x7FFFFFFF, 1: 1, 2: HEAP})],
    )
    f, g = res[(0, 0)], res[(0, 1)]
    assert f.status == g.status == "return"
    assert f.regs[:3] == g.regs[:3] == [12, 7, HEAP + 16]
    assert f.mem == g.mem == {HEAP + 16: 12}
    assert f.cpsr == 0xF0000000  # production code does not store the flags
    assert g.cpsr >> 28 == 0b0000  # 5 + 7: no flag
    assert f.d == [n * 0x0101010101010101 for n in range(32)]
    assert res[(1, 1)].cpsr >> 28 == 0b1001 and res[(1, 0)].mem == {HEAP + 3: 0x80}


def test_driver_reports_faults_calls_and_stack_writes(driver) -> None:
    res = driver(F_LDR | 1, [case({0: 0xFFFFFFF0}), case({0: 0x3000})])
    assert res[(0, 0)].status == "fault" and res[(0, 0)].detail == (0xFFFFFFF0, 4, F_LDR)
    assert res[(1, 0)].status == "return" and res[(1, 0)].regs[0] == 0x6C6C6568  # "hell"
    res = driver(F_CALL | 1, [case({0: 1, 1: 2, 2: HEAP})])
    f = res[(0, 0)]
    assert f.regs[0] == 3 and f.regs[13] == SP0 and f.regs[14] == (F_CALL + 6) | 1
    # push {r7, lr} left lr (RET | 1) below sp; r7 was 0 like the pristine stack
    lr = (RET | 1).to_bytes(4, "little")
    assert f.mem == {HEAP: 3, **{SP0 - 4 + i: b for i, b in enumerate(lr) if b}}
    res = driver(F_COUNT | 1, [case({0: 5})])
    assert res[(0, 0)].regs[0] == 0 and res[(0, 1)].cpsr >> 28 == 0b0110


def end(**kw) -> SimpleNamespace:
    base = {"status": "return", "regs": [0] * 16, "cpsr": 0, "fpscr": 0, "d": [0] * 32,
            "mem": {}, "fault": None}  # fmt: skip
    return SimpleNamespace(**{**base, **kw})


def result(**kw) -> host.Result:
    base = {"status": "return", "detail": (0, 0, 0), "regs": [0] * 16, "cpsr": 0, "fpscr": 0,
            "d": [0] * 32, "mem": {}}  # fmt: skip
    return host.Result(**{**base, **kw})


def test_differences() -> None:
    assert difftest.differences(end(), result(), 0, True) == []
    regs = [0] * 16
    regs[13] = 4
    assert difftest.differences(end(regs=regs), result(), 0, True) == [
        "sp: unicorn 0x00000004, C 0x00000000"
    ]
    regs[15] = 9  # the pc is not compared
    assert difftest.differences(end(), result(regs=[0] * 15 + [9]), 0, True) == []
    # NZCV only for the flags variant of a function without calls
    assert difftest.differences(end(cpsr=0x40000000), result(), 0, True) == []
    assert difftest.differences(end(cpsr=0x40000000), result(), 1, False) == []
    assert difftest.differences(end(cpsr=0x40000000), result(), 1, True) == [
        "nzcv: unicorn 0100, C 0000"
    ]
    assert difftest.differences(end(fpscr=0x2000001F), result(fpscr=0x00000000), 0, True) == [
        "fpscr.nzcv: unicorn 0010, C 0000"
    ]
    assert difftest.differences(end(fpscr=0x1F), result(), 0, True) == []  # cumulative bits
    d = [0] * 32
    d[31] = 1
    assert difftest.differences(end(d=d), result(), 0, True) == [
        "d31: unicorn 0x0000000000000001, C 0x0000000000000000"
    ]
    assert difftest.differences(end(mem={0x10: 1, 0x11: 2}), result(mem={0x11: 3}), 0, True) == [
        "mem 0x10: unicorn 0x01, C unchanged"
    ]
    assert difftest.differences(end(status="fault", fault=(8, 0x1460)), result(), 0, True) == [
        "status: unicorn fault, C return (0x0, 0x0, 0x0)"
    ]
    faults = (end(status="fault", fault=(8, 0x1460)), result(status="fault", detail=(8, 4, 0x1462)))
    assert difftest.differences(*faults, 0, True) == [
        "fault: unicorn 0x8 at 0x1460, C 0x8 at 0x1462"
    ]


def test_case_file_round_trip(tmp_path) -> None:
    c = case({0: 1, 12: 0xFFFFFFFF}, 0x20000000)
    c.patches = [(SP0, b"\x01\x02\x03"), (HEAP, b"\xff" * 4)]
    host.write_cases(tmp_path / "c.in", 3, [bytes(HEAP_SIZE)], [(9, c)])
    data = (tmp_path / "c.in").read_bytes()
    head = struct.unpack_from("<7I", data, 0)
    assert head == (0x3144544D, 3, WINDOW, HEAP, HEAP_SIZE, 1, 1)
    off = 28 + HEAP_SIZE
    fixed = struct.unpack_from("<84I", data, off)
    assert fixed[:3] == (9, 0, 1) and fixed[14] == 0xFFFFFFFF and fixed[18] == 0x20000000
    assert struct.unpack_from("<3I", data, off + 84 * 4) == (2, SP0, 3)
    assert len(data) == off + 84 * 4 + 4 + (8 + 4) + (8 + 4)


# -- against Unicorn (difftest group only) -------------------------------------------------


@pytest.fixture(scope="module")
def slice_path(macho, tmp_path_factory):
    pytest.importorskip("unicorn", reason="unicorn not installed (scripts/difftest --pytest)")
    host_cc()
    path = tmp_path_factory.mktemp("slice") / "synthetic.armv7"
    path.write_bytes(macho)
    return path


def test_synthetic_leaves_agree_with_unicorn(slice_path, tmp_path) -> None:
    run = difftest.run(slice_path, wanted=200, max_attempts=2000, jobs=2, out_dir=tmp_path)
    by_ptr = {r.ptr: r for r in run.results}
    assert sorted(by_ptr) == LEAVES
    for r in run.results:
        assert r.passed(200), (hex(r.ptr), r.statuses, r.examples, r.errors, r.driver_error)
    assert by_ptr[F_LDR | 1].faulted > 0  # random r0 faults outside the window, both sides
    assert by_ptr[F_COUNT | 1].statuses.get("budget", 0) > 0  # r0 = 0 loops 2^32 times
    assert run.summary()["with_mismatches"] == 0
    assert '"passed": true' in run.to_json() and "| leaf | 5 |" in run.to_markdown()


def test_a_wrong_translation_is_caught(slice_path, tmp_path, monkeypatch) -> None:
    """A store of the wrong width in F_ADD (an injected lifter bug) shows as a memory
    mismatch with its seed."""
    real = host.sources

    def broken(image, leaves):
        return {k: v.replace("MT_ST32(", "MT_ST16(") for k, v in real(image, leaves).items()}

    monkeypatch.setattr(host, "sources", broken)
    run = difftest.run(slice_path, wanted=100, max_attempts=500, jobs=1, only=[F_ADD | 1],
                       out_dir=tmp_path)  # fmt: skip
    (r,) = run.results
    assert r.mismatches > 50 and not r.passed(100)
    ex = r.examples[0]
    assert ex["first"].startswith("mem 0x") and ex["seed"].startswith("41:0x1411:")
    assert "## Mismatches" in run.to_markdown()


def test_unicorn_runs_are_independent(image, selection) -> None:
    """A long-lived emulator gives each case the same end state as a fresh one (Unicorn
    keeps translated code across runs; a budget stop must not leak into the next run)."""
    pytest.importorskip("unicorn", reason="unicorn not installed (scripts/difftest --pytest)")
    from recomp.difftest.emulator import Emulator

    window = inputs.pristine(image)
    data = inputs.data_range(image)
    shared = Emulator(window, inputs.text_range(image))
    for ptr in (F_COUNT | 1, F_LDR | 1, F_ADD | 1):
        lf = selection.leaves[ptr]
        pool = inputs.heap_pool(41, ptr)
        for i in range(40):
            c = inputs.make_case(41, ptr, i, lf.pointer_regs, lf.vfp, data)
            a, b = (
                shared.run(ptr, c, pool),
                Emulator(window, inputs.text_range(image)).run(ptr, c, pool),
            )
            assert (a.status, a.regs, a.cpsr, a.mem, a.fault) == (
                b.status, b.regs, b.cpsr, b.mem, b.fault,
            ), (hex(ptr), i)  # fmt: skip
