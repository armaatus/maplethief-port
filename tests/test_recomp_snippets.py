"""The snippet suite (#39): every ``tests/recomp/snippets/**/*.snippet`` file is one test that
lifts the hand-written Thumb-2 (or, with ``mode: arm``, A32) code, compiles it with the host
C compiler (MSVC cl on the Windows runner), runs it and compares the final state with the
file's ``[out]``, which was worked out from the ARM ARM pseudocode the file cites. Two more
tests per file check its bytes against its assembly: Capstone everywhere, clang where clang
has the ARM target. Format and how to add a snippet: tests/recomp/README.md. No game code."""

from __future__ import annotations

import re
from pathlib import Path

import pytest

import recomp_snippets as rs
from recomp.snippet import lift_snippet

FILES = rs.all_files()
IDS = [p.relative_to(rs.SNIPPET_DIR).with_suffix("").as_posix() for p in FILES]


@pytest.mark.parametrize("path", FILES, ids=IDS)
def test_snippet(request: pytest.FixtureRequest, tmp_path: Path, path: Path) -> None:
    snip = rs.load(path)
    if snip.xfail:  # strict: once the lifter can do it, the header must go
        request.applymarker(pytest.mark.xfail(reason=snip.xfail, strict=True))
    lifted = lift_snippet(snip.code, rs.SNIPPET_BASE, arm=snip.arm)
    assert not lifted.unsupported, lifted.unsupported
    assert not lifted.problems, lifted.problems
    diffs = rs.run(snip, tmp_path)
    listing = "\n".join(f"  {line.address:#06x}  {line.asm}" for line in snip.lines)
    assert not diffs, (
        f"{snip.title}\n" + "\n".join(diffs) + f"\n{listing}\n--- C ---\n{lifted.source}"
    )


@pytest.mark.parametrize("path", FILES, ids=IDS)
def test_snippet_bytes_match_capstone(path: Path) -> None:
    snip = rs.load(path)
    assert not rs.capstone_mismatches(snip)


@pytest.mark.parametrize("path", FILES, ids=IDS)
def test_snippet_bytes_match_clang(path: Path) -> None:
    if rs.clang() is None:
        pytest.skip("no clang with the ARM target (thumbv7-none-eabi) on PATH")
    snip = rs.load(path)
    encoded = rs.clang_encode(snip)
    diffs = []
    for line in snip.lines:
        clang_bytes = encoded[line.lineno]
        if clang_bytes is not None and clang_bytes != line.data:  # None: a relocation
            file_bytes = line.data.hex() if line.data else "?"
            diffs.append(
                f"line {line.lineno} {line.asm!r}: {file_bytes}, clang {clang_bytes.hex()}"
            )
    assert not diffs, "\n".join(diffs)


def test_suite_size() -> None:
    """The issue's floor: at least 100 snippets that are expected to pass (no xfail)."""
    running = [p for p in FILES if not rs.load(p).xfail]
    assert len(running) >= 100, len(running)


# -- the format itself, on synthetic text --------------------------------------------------

EXAMPLE = """\
# a comment line
title: example
ref: A8.8 ADD (register) T1; #39 names an issue, not a comment
xfail: #34 something

[code]
4218        adds r2, r0, r1     @ a trailing comment
loop:
fed1        bne loop
00f0fcff    bl ext_2000
7047        bx lr
78563412    .long 0x12345678

[in]
r0 = DATA+4         # a trailing comment
s3 = f32:1.5
d17 = f64:-2.0
flags = nZcV
word DATA = 1 -1
dword DATA+8 = 0x1122334455667788

[out]
r2 = ?
r3 = SP0-8
flags = N?c?
half DATA+2 = 0xFFFF
dword DATA+8 = 0x1122334455667788

[c]
void f_00002001(mt_cpu *MT_RESTRICT cpu) { /* # not a comment */ }

[dispatch]
0x3001 = target_fn
"""


def test_parse_example() -> None:
    snip = rs.parse(EXAMPLE, Path("example.snippet"))
    assert (snip.title, snip.xfail) == ("example", "#34 something")
    assert snip.ref.endswith("#39 names an issue, not a comment")
    assert [(line.address, line.asm) for line in snip.lines] == [
        (0x1000, "adds r2, r0, r1"),
        (0x1002, "bne loop"),
        (0x1004, "bl ext_2000"),
        (0x1008, "bx lr"),
        (0x100A, ".long 0x12345678"),
    ]
    assert snip.labels == {"loop": 0x1002, "ext_2000": 0x2000}
    assert snip.label_lines == {"loop": 9}
    assert snip.regs_in == {"r0": rs.DATA_BASE + 4, "s3": 0x3FC00000, "d17": 0xC000000000000000}
    assert snip.flags_in == 0b0101
    assert snip.mem_in == {
        rs.DATA_BASE: bytes([1, 0, 0, 0]),
        rs.DATA_BASE + 4: bytes([0xFF] * 4),
        rs.DATA_BASE + 8: bytes.fromhex("8877665544332211"),
    }
    assert snip.regs_out == {"r2": None, "r3": rs.SP0 - 8}
    assert snip.flags_out == (1, None, 0, None)
    assert snip.mem_out == [
        (rs.DATA_BASE + 2, 2, 0xFFFF),
        (rs.DATA_BASE + 8, 4, 0x55667788),
        (rs.DATA_BASE + 12, 4, 0x11223344),
    ]
    assert "/* # not a comment */" in snip.c
    assert snip.dispatch == {0x3001: "target_fn"}


TRAP_EXAMPLE = """\
title: trap
ref: A8.8 UDF T1

[code]
fede        trap

[in]
r0 = 1

[out]
trap = udf BASE
"""


def test_trap_expectation_is_checked(tmp_path: Path) -> None:
    good = rs.parse(TRAP_EXAMPLE, Path("trap.snippet"))
    assert good.trap == (6, rs.SNIPPET_BASE)
    for name in ("good", "bad", "wrong"):
        (tmp_path / name).mkdir()
    assert rs.run(good, tmp_path / "good") == []
    bad = rs.parse(TRAP_EXAMPLE.replace("udf BASE", "udf BASE+2"), Path("trap.snippet"))
    assert rs.run(bad, tmp_path / "bad") == [f"trap_pc: got {rs.SNIPPET_BASE:#x}, expected 0x1002"]
    wrong = rs.parse(TRAP_EXAMPLE.replace("r0 = 1", "").replace("[out]", "[out]\nr0 = 1"),
                     Path("trap.snippet"))  # fmt: skip
    assert rs.run(wrong, tmp_path / "wrong") == ["r0: got 0x0, expected 0x1"]


def test_arm_mode_header() -> None:
    snip = rs.parse(EXAMPLE.replace("xfail: #34 something", "mode: arm"), Path("x.snippet"))
    assert snip.arm and snip.mode == "arm"
    with pytest.raises(rs.SnippetError, match="mode must be one of"):
        rs.parse(EXAMPLE.replace("xfail: #34 something", "mode: a64"), Path("x.snippet"))


def test_unnamed_registers_must_be_unchanged() -> None:
    snip = rs.parse(EXAMPLE, Path("example.snippet"))
    want = rs._expected(snip)
    assert want["r0"] == rs.DATA_BASE + 4 and want["r1"] == 0 and want["r2"] is None
    assert (want["r13"], want["r14"]) == (rs.SP0, rs.RET)
    assert want["h3"] == 0x3FC00000 and want["h2"] == 0  # s3, s2
    assert (want["h34"], want["h35"]) == (0, 0xC0000000)  # d17
    assert (want["N"], want["Z"], want["C"], want["V"]) == (1, None, 0, None)
    assert "r15" not in want and "fpscr" not in want


@pytest.mark.parametrize(
    ("change", "message"),
    [
        (("xfail: #34 something\n", "ref:\n"), "missing header"),
        (("[dispatch]", "[nope]"), "unknown section"),
        (("[c]", "[in]"), "section [in] twice"),
        (("flags = nZcV", "flags = nZc"), "four letters"),
        (("flags = nZcV", "flags = nZ?V"), "N Z C V"),
        (("r0 = DATA+4", "r0 = DATA*4"), "bad value"),
        (("r0 = DATA+4", "r15 = 0"), "unknown state name"),
        (("r0 = DATA+4", "r0 = 0x1_0000_0000"), "does not fit"),
        (("word DATA = 1 -1", "word 0x10 = 1"), "memory outside"),
        (("fed1        bne loop", "zz          bne loop"), "bad hex"),
        (("loop:", "loop:\nloop:"), "defined twice"),
        (("7047        bx lr", "7047"), "expected '<hex bytes> <assembly>'"),
        (("0x3001 = target_fn", "0x3001 target_fn"), "<guest address> = <C function>"),
        (("r2 = ?", "trap = boom BASE"), "expected 'trap = <"),
    ],
)
def test_parse_errors(change: tuple[str, str], message: str) -> None:
    text = EXAMPLE.replace(*change)
    assert text != EXAMPLE
    with pytest.raises(rs.SnippetError, match=re.escape(message)):
        rs.parse(text, Path("bad.snippet"))


@pytest.mark.parametrize(
    ("ours", "capstone_text"),
    [
        ("ldr r3, [r0, #-4]", "ldr r3, [r0, #-0x4]"),
        ("tst.w r0, #0x80000000", "tst.w r0, #-0x80000000"),
        ("push.w {r4, r8, r9, r10, r11, lr}", "push.w {r4, r8, sb, sl, fp, lr}"),
        ("vmov.f32 s1, #-0.125", "vmov.f32 s1, #-1.250000e-01"),
        ("ldm.w r0, {r4, r8, r12}", "ldm.w r0, {r4, r8, ip}"),
    ],
)
def test_normalize_matches_capstone_spelling(ours: str, capstone_text: str) -> None:
    assert rs.normalize(ours) == rs.normalize(capstone_text)


def test_normalize_resolves_labels() -> None:
    assert rs.normalize("bne loop", {"loop": 0x1002}) == rs.normalize("bne #0x1002")
    assert rs.normalize("adds r2, r0, r1") != rs.normalize("adds r3, r0, r1")


def test_capstone_catches_wrong_assembly() -> None:
    good = rs.parse(EXAMPLE, Path("x.snippet"))
    assert not rs.capstone_mismatches(good)
    bad = rs.parse(EXAMPLE.replace("adds r2, r0, r1", "adds r3, r0, r1"), Path("x.snippet"))
    assert any("Capstone reads 'adds r2, r0, r1'" in p for p in rs.capstone_mismatches(bad))
    bad = rs.parse(EXAMPLE.replace(".long 0x12345678", ".long 0x12345679"), Path("x.snippet"))
    assert any(".long 0x12345679 is" in p for p in rs.capstone_mismatches(bad))


def test_clang_source_places_code_and_callees() -> None:
    src = rs.clang_source(rs.parse(EXAMPLE, Path("x.snippet")))
    assert f".space {rs.SNIPPET_BASE:#x}" in src
    assert "mt_line_7:\n    adds r2, r0, r1" in src and "loop:\nmt_line_9:" in src
    assert ".org 0x2000\n.thumb\n.thumb_func\next_2000:" in src
