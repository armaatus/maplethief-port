"""Real-asset checks for the recompiler's loader (#31) against the armv7 slice.

Skipped when work/ipa/ is absent (CI). Each external oracle skips when its tool is missing:

- ``nm -m -u`` (Apple's or LLVM's nm): the 333 imports and their libraries;
- docs/re/imports.md (#11): the per-framework import counts;
- ``dyld_info`` (macOS, /usr/bin): ``-fixups`` for the address and kind of every bind, lazy
  bind and rebase, ``-opcodes`` for every bind entry in full (the test replays dyld_info's own
  decoding of the opcode streams);
- ``objdump --macho`` (Apple's llvm-objdump): the bind, lazy-bind, weak-bind and rebase
  tables in full.

``dyld_info -fixups`` names the wrong target for 933 of the 1,268 binds of this slice (it
often shows the symbol of the *next* bind target, e.g. ``_AudioServicesPlaySystemSound`` for
``__objc_empty_vtable``), while its own ``-opcodes`` listing, ``objdump`` and the structures
themselves (RTTI typeinfo sizes, see docs/ANALYSIS.md) agree with the loader. So from
``-fixups`` the test takes only addresses and kinds.
"""

from __future__ import annotations

import hashlib
import json
import re
import shutil
import subprocess
from collections import Counter

import pytest

from mtport import bootstrap as bs
from mtport import imports as re_imports
from recomp import loader
from repo_paths import REPO

pytestmark = pytest.mark.real_assets

SLICE = REPO / bs.slice_path("armv7")


@pytest.fixture(scope="module")
def image() -> loader.Image:
    return loader.Image.load(SLICE)


def _tool(*args: str) -> str:
    if shutil.which(args[0]) is None:
        pytest.skip(f"{args[0]} not on PATH")
    return subprocess.run(
        [*args, str(SLICE)], check=False, capture_output=True, text=True, encoding="utf-8"
    ).stdout


def _lib(image: loader.Image, ordinal: int) -> str:
    """The short library name the tools print (``Foundation``, ``libstdc++``)."""
    return re_imports.framework_name(image.macho.library_name(ordinal))


# -- imports ---------------------------------------------------------------------------------


def test_all_333_imports_resolved(image):
    """Acceptance (#31): every import resolves to its stub / pointer / bound addresses."""
    imps = image.imports
    assert len(imps) == 333
    functions = [i for i in imps if i.stubs]
    assert len(functions) == len(image.stubs) == 217
    for imp in functions:  # one ARM stub and the lazy pointer it loads
        assert len(imp.stubs) == len(imp.lazy) == 1, imp.name
        assert image.import_at(imp.stubs[0]) is imp
        assert image.import_at(imp.lazy[0]) is imp
        word = image.macho.u32(imp.stubs[0])
        assert word & 0xFFFFF000 == 0xE59FF000  # ldr pc, [pc, #n]
        assert imp.stubs[0] + 8 + (word & 0xFFF) == imp.lazy[0]
    data = [i for i in imps if not i.stubs]
    assert len(data) == 116
    for imp in data:  # a data import is reached through a non-lazy pointer or a bound word
        assert imp.non_lazy or imp.binds, imp.name
        for addr in (*imp.non_lazy, *(a for a, _ in imp.binds)):
            assert image.import_at(addr) is imp
    assert sum(len(i.non_lazy) for i in imps) == 28  # the other 54 slots point into the image
    assert image.problems() == []


def test_imports_match_nm(image):
    text = _tool("nm", "-m", "-u")
    if not text.strip():
        text = _tool("nm", "-m")
    theirs = {}
    for line in text.splitlines():
        m = re.search(r"\(undefined\) (?:weak )?external (\S+) \(from (\S+)\)", line)
        if m:
            theirs[m[1]] = m[2]
    if not theirs:
        pytest.skip("this nm does not support nm -m on a Mach-O (Apple's or LLVM's nm does)")
    assert len(theirs) == 333
    ours = {i.name: re_imports.framework_name(i.library) for i in image.imports}
    assert set(ours) == set(theirs)
    for name, lib in theirs.items():  # nm prints "libstdc++" for /usr/lib/libstdc++.6.dylib
        assert ours[name].lower() == lib.lower().split(".", 1)[0], name


def test_imports_match_imports_md(image):
    """The per-framework counts of docs/re/imports.md (#11, "By framework")."""
    md = (REPO / "docs/re/imports.md").read_text(encoding="utf-8")
    table = md.split("## By framework", 1)[1].split("\n## ", 1)[0]
    theirs = {
        m[1]: int(m[2]) for m in re.finditer(r"^\| \[([^\]]+)\]\([^)]*\) \| (\d+) \|", table, re.M)
    }
    assert re.search(r"^\| \*\*Total\*\* \| \*\*333\*\* \|", table, re.M)
    ours = Counter(re_imports.framework_name(i.library) for i in image.imports)
    assert dict(ours) == theirs


# -- dyld fixups -----------------------------------------------------------------------------

_FIXUP = re.compile(
    r"^\s*(?P<seg>__\w+)\s+(?P<sect>__\w+)\s+0x(?P<addr>[0-9A-Fa-f]+)\s+"
    r"(?P<kind>bind|lazy-bind|weak-bind|rebase)\s+(?P<target>.*?)\s*$"
)


def test_fixups_match_dyld_info(image):
    """Acceptance (#31): rebase count and every bind's address and kind vs ``dyld_info``."""
    text = _tool("dyld_info", "-fixups")
    rows = [m.groupdict() for line in text.splitlines() if (m := _FIXUP.match(line))]
    if not rows:
        pytest.skip("dyld_info printed no fixups")
    kinds = {"bind": "bind", "lazy-bind": "lazy", "weak-bind": "weak", "rebase": "rebase"}
    theirs = Counter((int(r["addr"], 16), kinds[r["kind"]]) for r in rows)
    ours = Counter((b.address, b.kind) for b in image.binds if b.kind != "weak")
    ours.update((r.address, "rebase") for r in image.rebases)
    assert theirs == ours
    assert sum(theirs.values()) == 1268
    assert len(image.rebases) == sum(r["kind"] == "rebase" for r in rows) == 0
    for r in rows:  # the section column agrees with the loader's section map
        sect = image.macho.section_at(int(r["addr"], 16))
        assert (sect.segname, sect.sectname) == (r["seg"], r["sect"])


def _replay_dyld_info_opcodes(text: str, image: loader.Image) -> dict[str, list[tuple]]:
    """The bind entries of dyld_info's ``-opcodes`` listing, by stream (bind, lazy).

    dyld_info prints decoded operands: ``DO_BIND_ADD_ADDR_IMM_SCALED(n)`` shows the whole
    advance (``(imm + 1) * 4``), the other operands are the raw values."""
    streams: dict[str, list[tuple]] = {"bind": [], "lazy": []}
    current = None
    ordinal = addend = flags = 0
    symbol = ""
    base = offset = 0

    def emit() -> None:  # reads the replay state at call time
        streams[current].append(((base + offset) & 0xFFFFFFFF, symbol, ordinal, addend, flags))

    for line in text.splitlines():
        stripped = line.strip()
        if stripped == "bind opcodes:":
            current = "bind"
        elif stripped == "lazy bind opcodes:":
            current = "lazy"
        elif re.fullmatch(r"[a-z ]+ opcodes:", stripped):  # e.g. a weak stream, if listed
            pytest.fail(f"dyld_info lists a stream the replay does not know: {stripped}")
        m = re.match(r"0x[0-9A-F]+ (\w+)\((.*)\)$", stripped)
        if current is None or m is None:
            continue
        op, args = m[1], [a.strip() for a in m[2].split(",")] if m[2] else []
        nums = [int(a, 0) for a in args if re.fullmatch(r"0x[0-9A-Fa-f]+|-?\d+", a)]

        if op.endswith("SET_DYLIB_ORDINAL_IMM") or op.endswith("SET_DYLIB_ORDINAL_ULEB"):
            ordinal = nums[0]
        elif op.endswith("SET_SYMBOL_TRAILING_FLAGS_IMM"):
            flags, symbol = int(args[0], 0), args[1]
        elif op.endswith("SET_ADDEND_SLEB"):
            addend = nums[0]
        elif op.endswith("SET_SEGMENT_AND_OFFSET_ULEB"):
            base, offset = image.segments[nums[0]].vmaddr, nums[1]
        elif op.endswith("_ADD_ADDR_ULEB") and "DO_" not in op:
            offset += nums[0]
        elif op == "BIND_OPCODE_DO_BIND":
            emit()
            offset += 4
        elif op == "BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB":
            emit()
            offset += 4 + nums[0]
        elif op == "BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED":
            emit()
            offset += nums[0]
        elif op == "BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB":
            for _ in range(nums[0]):
                emit()
                offset += 4 + nums[1]
        elif op != "BIND_OPCODE_DONE" and op != "BIND_OPCODE_SET_TYPE_IMM":
            pytest.fail(f"opcode the replay does not know: {stripped}")
        offset &= 0xFFFFFFFFFFFFFFFF
    return streams


def test_bind_entries_match_dyld_info_opcodes(image):
    """Acceptance (#31): every bind entry (address, symbol, library ordinal, addend, flags)
    equals dyld_info's own decoding of the bind and lazy-bind streams."""
    text = _tool("dyld_info", "-opcodes")
    if "bind opcodes:" not in text:
        pytest.skip("dyld_info printed no opcodes")
    assert "no rebase opcodes" in text and image.rebases == ()
    theirs = _replay_dyld_info_opcodes(text, image)
    for kind in ("bind", "lazy"):
        ours = [
            (b.address, b.symbol, b.ordinal, b.addend, b.flags)
            for b in image.binds
            if b.kind == kind
        ]
        assert theirs[kind] == ours, kind
    assert (len(theirs["bind"]), len(theirs["lazy"])) == (1055, 213)


def _objdump_rows(flag: str) -> list[list[str]]:
    text = _tool("objdump", "--macho", flag)
    lines = text.splitlines()
    try:
        start = next(i for i, line in enumerate(lines) if line.startswith("segment "))
    except StopIteration:
        pytest.skip("this objdump has no --macho bind tables (Apple's llvm-objdump has)")
    return [line.split() for line in lines[start + 1 :] if line.strip()]


def test_binds_match_objdump(image):
    """Every bind, lazy bind, weak bind and rebase vs llvm-objdump's tables."""
    sect = image.macho.section_at

    def where(addr: int) -> tuple[str, str]:
        s = sect(addr)
        return s.segname, s.sectname

    theirs = sorted(
        (*r[:2], int(r[2], 16), int(r[4]), r[5], r[6], r[7:] == ["(weak_import)"])
        for r in _objdump_rows("--bind")
    )
    ours = sorted(
        (
            *where(b.address),
            b.address,
            b.addend,
            _lib(image, b.ordinal),
            b.symbol,
            bool(b.flags & 1),
        )
        for b in image.binds
        if b.kind == "bind"
    )
    assert theirs == ours
    theirs = sorted((*r[:2], int(r[2], 16), r[3], r[4]) for r in _objdump_rows("--lazy-bind"))
    ours = sorted(
        (*where(b.address), b.address, _lib(image, b.ordinal), b.symbol)
        for b in image.binds
        if b.kind == "lazy"
    )
    assert theirs == ours
    theirs = sorted((*r[:2], int(r[2], 16), int(r[4]), r[5]) for r in _objdump_rows("--weak-bind"))
    ours = sorted(
        (*where(b.address), b.address, b.addend, b.symbol) for b in image.binds if b.kind == "weak"
    )
    assert theirs == ours and len(ours) == 4  # operator new / delete, C++ weak definitions
    assert _objdump_rows("--rebase") == [] and image.rebases == ()


# -- the rest of the loader view -------------------------------------------------------------


def test_code_and_objc(image):
    assert image.entry_point == 0x27E0  # crt1 start, ARM
    assert len(image.function_starts) == 2655
    assert len(image.init_functions) == 2
    assert all(p & 1 and p in image.function_starts for p in image.init_functions)
    names = [s.sectname for s in image.objc_sections]
    assert len(names) == len(set(names)) == 15
    assert "__cfstring" in names and "__objc_classlist" in names
    assert image.objc_section("__objc_classlist").size == 75 * 4
    assert image.objc_section("__cfstring").size == 0x16F0


def test_manifest_covers_everything(image, tmp_path):
    out = tmp_path / "loader.json"
    written = loader.write_manifest(SLICE, out)
    doc = json.loads(out.read_text(encoding="utf-8"))
    assert doc["source"]["path"] == "work/slices/MapleThief.armv7"
    assert doc["source"]["sha256"] == hashlib.sha256(SLICE.read_bytes()).hexdigest()
    assert len(doc["imports"]) == 333 and len(doc["binds"]) == 1272 and doc["rebases"] == []
    assert doc["entry_point"] == "0x27E0" and len(doc["init_functions"]) == 2
    assert written.summary()["imports"] == 333
