"""The instruction census (#33, #34) on synthetic code, and the rule that the snippet suite
covers every form of the binary: ``tests/recomp/integer_forms.txt`` and
``tests/recomp/vfp_forms.txt`` list them (the local test ``test_recomp_census_real.py`` keeps
the lists equal to the binary's census), and each one must appear in a snippet that is
expected to pass. No game code."""

from __future__ import annotations

import json

import pytest

import recomp_snippets as rs
from recomp import census
from recomp.discover import discover_function
from recomp_harness import SNIPPET_BASE
from repo_paths import REPO

FORM_LIST = REPO / "tests" / "recomp" / "integer_forms.txt"
VFP_FORM_LIST = REPO / "tests" / "recomp" / "vfp_forms.txt"


def thumb_fn(hexcode: str):
    code = bytes.fromhex(hexcode)
    return discover_function(code, SNIPPET_BASE, SNIPPET_BASE | 1, SNIPPET_BASE + len(code))


def forms_of(hexcode: str, arm: bool = False) -> list[str]:
    code = bytes.fromhex(hexcode)
    ptr = SNIPPET_BASE if arm else SNIPPET_BASE | 1
    fn = discover_function(code, SNIPPET_BASE, ptr, SNIPPET_BASE + len(code))
    return [census.form(i, fn) for i in fn.ordered()]


# (bytes, the form of each instruction); assembled with clang, checked against Capstone
# by the snippet suite where the same encodings appear.
FORMS = [
    ("42187047", ["adds r,r,r", "bx r ret"]),
    ("10f47f427047", ["ands r,r,#c", "bx r ret"]),  # a rotated immediate: carry-out
    ("10f0010f7047", ["tst r,#", "bx r ret"]),  # plain imm: C unchanged
    ("4fea20707047", ["asr r,r,#", "bx r ret"]),  # asr.w r0, r0, #28
    ("5fea20007047", ["asrs r,r,#32", "bx r ret"]),  # asrs.w r0, r0, #32
    ("b1ebe07f7047", ["cmp r,r,asr #", "bx r ret"]),
    ("50f8081f7047", ["ldr r,[r,#]!", "bx r ret"]),
    ("50f8086b7047", ["ldr r,[r],#", "bx r ret"]),
    ("51f8012c7047", ["ldr r,[r,#-]", "bx r ret"]),  # ldr r2, [r1, #-1]
    ("44587047", ["ldr r,[r,r]", "bx r ret"]),
    ("84467c467047", ["mov r,r", "mov r,pc", "bx r ret"]),  # mov ip, r0 / mov r4, pc
    ("1cc87047", ["ldm r!,{..}", "bx r ret"]),  # ldm r0!, {r2, r3, r4}
    ("0fc87047", ["ldm r,{..rn}", "bx r ret"]),  # ldm r0, {r0-r3}: base in the list
    ("90bd", ["pop {..pc} ret"]),
    ("08bf7047", ["it", "bx r ret ?"]),  # it eq / bxeq lr
    ("00f0fdbf", ["b tail"]),  # b.w out of the function
    ("fee7", ["b label"]),  # b . (a loop: inside)
    ("fede", ["trap"]),
    ("43ec122a51ec120a7047", ["vmov s,s,r,r", "vmov r,r,s,s", "bx r ret"]),
    (
        "20ef1001c0ef5000fbff00067047",
        ["neon vorr d,d,d", "neon vmov.i32 q,#", "neon vcvt.f32.s32 d,d", "bx r ret"],
    ),
    ("08bf41ef020d7047", ["it", "neon vadd.f32 d,d,d ?", "bx r ret"]),  # it eq / vaddeq
    ("e0ff10007047", ["neon <undecoded>", "bx r ret"]),  # vshr.u32 d16, d0, #32 (not used)
]


@pytest.mark.parametrize(("hexcode", "want"), FORMS, ids=[h for h, _ in FORMS])
def test_form_keys(hexcode: str, want: list[str]) -> None:
    assert forms_of(hexcode) == want


def test_a32_form_keys() -> None:
    # add r2, r1, r3, lsl #2 / ldr r2, [r3], #4 / addne r8, r8, #1 / bx lr
    got = forms_of("032181e0042093e4018088121eff2fe1", arm=True)
    assert got == ["a32 add r,r,r,lsl #", "a32 ldr r,[r],#", "a32 add r,r,# ?", "a32 bx r ret"]


def test_jump_table_sites_are_their_own_form() -> None:
    # cmp r0, #2 / bhi default / tbb [pc, r0] / 3 entries + pad / case0..2: movs, default: bx lr
    fn = thumb_fn("022806d8dfe800f0020304000020012002207047")
    keys = [census.form(i, fn) for i in fn.ordered()]
    assert "tbb [pc,r] table:tbb" in keys


def test_conditions_and_classes() -> None:
    fn = thumb_fn("002818bf0120f1ee10fa7047")  # cmp / it ne / movne / vmrs / bx lr
    got = census.function_forms(fn)
    assert "cond ne" in got and "mov r,# ?" in got
    assert census.form_class("vmrs APSR_nzcv,fpscr") == "vfp"
    assert {census.form_class(k) for k in got} == {"integer", "vfp"}
    assert census.form_class("cond eq") == "integer"
    assert census.form_class("neon <undecoded>") == "neon"
    assert census.form_class("neon vadd.f32 d,d,d") == "neon"


def test_take_counts_and_json() -> None:
    fn = thumb_fn("e0ff1000012001207047")  # vshr.u32 d16, d0, #32 (undecoded) / movs x2 / bx lr
    c = census.take([fn], unsupported=[SNIPPET_BASE])
    assert c.forms["movs r,#"].count == 2 and c.forms["movs r,#"].example == SNIPPET_BASE + 4
    assert c.forms["neon <undecoded>"].unsupported == 1
    counts = c.counts()
    assert counts["unsupported_instructions"] == 1
    assert counts["unsupported_integer_instructions"] == 0
    assert (counts["integer_forms"], counts["neon_forms"]) == (2, 1)
    doc = json.loads(c.to_json({"path": None, "arch": "armv7", "sha256": "0"}))
    assert doc["schema"] == 1 and doc["counts"] == counts
    assert doc["forms"][0] == {"form": "bx r ret", "class": "integer", "count": 1,
                               "functions": 1, "example": "0x1008", "unsupported": 0}  # fmt: skip
    text = census.integer_form_list(c)
    assert census.read_form_list(text) == ["bx r ret", "movs r,#"]


def _covered() -> dict[str, list[str]]:
    covered: dict[str, list[str]] = {}
    for path in rs.all_files():
        snip = rs.load(path)
        if snip.xfail:
            continue  # a snippet the lifter can't run yet tests nothing
        for key in rs.forms(snip):
            covered.setdefault(key, []).append(snip.name)
    return covered


def test_every_integer_form_of_the_binary_has_a_snippet() -> None:
    """#33's acceptance criterion: each integer form of the census is in a passing snippet
    (which also proves it has a lifter: test_snippet fails on an unsupported instruction)."""
    listed = census.read_form_list(FORM_LIST.read_text(encoding="utf-8"))
    assert len(listed) == len(set(listed)) and listed == sorted(listed)
    assert all(census.form_class(f) == "integer" for f in listed)
    missing = sorted(set(listed) - set(_covered()))
    assert missing == [], f"integer forms without a snippet: {missing}"


def test_every_vfp_form_of_the_binary_has_a_snippet() -> None:
    """#34's acceptance criterion: each VFP and NEON form of the census is in a passing
    snippet (so it decodes, has a lifter and computes what the ARM ARM says)."""
    listed = census.read_form_list(VFP_FORM_LIST.read_text(encoding="utf-8"))
    assert len(listed) == len(set(listed)) and listed == sorted(listed)
    assert all(census.form_class(f) in ("vfp", "neon") for f in listed)
    assert "neon <undecoded>" not in listed
    missing = sorted(set(listed) - set(_covered()))
    assert missing == [], f"VFP/NEON forms without a snippet: {missing}"


def test_vfp_form_list_text() -> None:
    fn = thumb_fn("20ef100130ee201a7047")  # vorr d0, d0, d0 / vadd.f32 s2, s0, s1 / bx lr
    text = census.vfp_form_list(census.take([fn]))
    assert census.read_form_list(text) == ["neon vorr d,d,d", "vadd.f32 s,s,s"]
