"""Unit tests for the recompiler's loader (recomp/loader.py, #31) and ``python -m recomp load``.

Synthetic images only (tests/importfixtures.py builds them: stubs, lazy and non-lazy pointers,
bind / weak / lazy / rebase streams, __mod_init_func, ObjC sections). The real slice is
checked in tests/test_recomp_loader_real.py.
"""

from __future__ import annotations

import json

import pytest

import importfixtures as fx
from machofixtures import (
    BIND_DO,
    BIND_DONE,
    BIND_SEGMENT_OFFSET,
    BIND_SYMBOL,
    BIND_TYPE_IMM,
    REBASE_ADD_ADDR_IMM_SCALED,
    REBASE_ADD_ADDR_ULEB,
    REBASE_DONE,
    REBASE_IMM_TIMES,
    REBASE_SEGMENT_OFFSET,
    REBASE_TYPE_IMM,
    uleb128,
)
from mtport import ghidra_common as gc
from mtport import macho
from mtport.names import parse_address
from recomp import __main__ as cli
from recomp import loader

STARTS = [fx.TEXT | 1, (fx.TEXT + 0x10) | 1, (fx.TEXT + 0x20) | 1]
INIT_FUNCS = STARTS[1:]
# Every word of __DATA that holds an address of this image: the LOCAL __nl_symbol_ptr slot
# (0x3010), the selref (0x3018) and both init functions (0x3020, 0x3024).
REBASE = (
    bytes([REBASE_TYPE_IMM | 1, REBASE_SEGMENT_OFFSET | 2])
    + uleb128(0x10)
    + bytes([REBASE_ADD_ADDR_ULEB])
    + uleb128(4)
    + bytes([REBASE_IMM_TIMES | 1, REBASE_ADD_ADDR_IMM_SCALED | 1, REBASE_IMM_TIMES | 2])
    + bytes([REBASE_DONE])
)


def weak_bind(symbol: str, offset: int, segment: int = 2) -> bytes:
    return (
        bytes([BIND_SYMBOL])
        + symbol.encode()
        + b"\0"
        + bytes([BIND_TYPE_IMM | 1, BIND_SEGMENT_OFFSET | segment])
        + uleb128(offset)
        + bytes([BIND_DO, BIND_DONE])
    )


def build(**overrides) -> bytes:
    args = {
        "init_funcs": INIT_FUNCS,
        "rebase": REBASE,
        "weak": weak_bind("_foo", fx.LAZY - 0x3000),
        "entry": STARTS[0],
    }
    args.update(overrides)
    return fx.build(b"\x70\x47" * 0x18, STARTS, **args)


@pytest.fixture(scope="module")
def image() -> loader.Image:
    return loader.Image(build())


def test_layout(image):
    assert [s.name for s in image.segments] == ["__PAGEZERO", "__TEXT", "__DATA", "__LINKEDIT"]
    assert image.text_base == 0x1000
    assert (image.text.addr, image.text.end) == (fx.TEXT, fx.TEXT + 0x30)
    assert image.vm_to_offset(fx.LAZY) == 0x2000
    assert image.offset_to_vm(0x2000) == fx.LAZY
    assert image.const_word(fx.STUBS, 4) == image.macho.u32(fx.STUBS)
    assert image.const_word(fx.LAZY, 4) is None  # __DATA is not constant
    vmaddr, raw = image.segment_bytes("__DATA")
    assert vmaddr == 0x3000 and raw[0x20:0x28] == b"\x11\x18\0\0\x21\x18\0\0"
    data = image.macho.segment("__DATA")
    assert (data.initprot, image.text_seg.initprot) == (3, 5)


def test_code(image):
    assert image.function_starts == tuple(STARTS)
    assert image.function_end(STARTS[0]) == fx.TEXT + 0x10
    assert image.function_end(STARTS[2]) == image.text.end
    assert image.entry_point == STARTS[0]
    assert image.init_functions == tuple(INIT_FUNCS)


def test_dyld_fixups(image):
    assert [r.address for r in image.rebases] == [0x3010, 0x3018, 0x3020, 0x3024]
    assert {r.type for r in image.rebases} == {macho.REBASE_TYPE_POINTER}
    assert [(b.kind, b.address, b.symbol, b.ordinal) for b in image.binds] == [
        ("bind", fx.NL, "_kConst", 1),
        ("bind", fx.CLASSREF, "_OBJC_CLASS_$_NSString", 2),
        ("bind", fx.DATA_WORD, "_free", 1),
        ("weak", fx.LAZY, "_foo", 0),
        ("lazy", fx.LAZY, "_foo", 1),
        ("lazy", fx.LAZY + 4, "_objc_msgSend", 2),
        ("lazy", fx.LAZY + 8, "_bar", 1),
    ]


def test_imports(image):
    names = [i.name for i in image.imports]
    assert names == [n for n, _ in fx.UNDEFINED]
    foo = image.import_named("_foo")
    assert (foo.stubs, foo.lazy, foo.non_lazy, foo.binds) == ([fx.STUB_FOO], [fx.LAZY], [], [])
    assert foo.kind == "function" and foo.library == "/usr/lib/libSystem.B.dylib"
    assert image.import_named("_kConst").non_lazy == [fx.NL]
    assert image.import_named("_free").binds == [(fx.DATA_WORD, "__data")]
    assert image.import_named("_OBJC_CLASS_$_NSString").kind == "class"
    unused = image.import_named("_unused")
    assert unused.weak and not (unused.stubs or unused.lazy or unused.non_lazy or unused.binds)
    assert image.import_named("_nope") is None


def test_import_at(image):
    assert image.import_at(fx.STUB_MSGSEND).name == "_objc_msgSend"
    assert image.import_at(fx.STUB_MSGSEND | 1).name == "_objc_msgSend"  # a Thumb-bit pointer
    assert image.import_at(fx.LAZY + 8).name == "_bar"
    assert image.import_at(fx.NL).name == "_kConst"
    assert image.import_at(fx.CLASSREF).name == "_OBJC_CLASS_$_NSString"
    assert image.import_at(fx.DATA_WORD).name == "_free"
    assert image.import_at(fx.NL + 4) is None  # the LOCAL slot points into the image
    assert image.import_at(fx.TEXT) is None
    assert {a: s.symbol for a, s in image.stubs.items()} == {
        fx.STUB_FOO: "_foo",
        fx.STUB_MSGSEND: "_objc_msgSend",
        fx.STUB_BAR: "_bar",
    }


def test_objc_sections(image):
    found = [(s.segname, s.sectname, s.addr) for s in image.objc_sections]
    assert found == [
        ("__TEXT", "__objc_methname", fx.METHNAME),
        ("__DATA", "__objc_classrefs", fx.CLASSREF),
        ("__DATA", "__objc_selrefs", fx.SELREF),
    ]
    assert image.objc_section("__objc_selrefs").size == 4
    assert image.objc_section("__objc_classlist") is None


def test_sound_image_has_no_problems(image):
    assert image.problems() == []
    assert image.summary() == {
        "segments": 4,
        "sections": 9,
        "dylibs": 3,
        "imports": 7,
        "function imports": 3,
        "data imports": 4,
        "stubs": 3,
        "binds": 3,
        "weak binds": 1,
        "lazy binds": 3,
        "rebases": 4,
        "function starts": 3,
        "init functions": 2,
        "objc sections": 3,
    }


@pytest.mark.parametrize(
    ("overrides", "problem"),
    [
        ({"init_funcs": [fx.TEXT + 0x31]}, "init function 0x1831 is not a function start"),
        ({"entry": None}, "no LC_UNIXTHREAD entry point"),
        ({"entry": 0x1804}, "entry point 0x1804 is not a function start"),
        ({"weak": weak_bind("_nope", 0x1C)}, "weak bind at 0x301C: _nope is not an import"),
        (
            {"rebase": bytes([REBASE_TYPE_IMM | 1, REBASE_SEGMENT_OFFSET | 1, 0, 0x51, 0])},
            "rebase at 0x1000 is not in a writable segment",
        ),
        # The second stub loads the _foo lazy pointer instead of its own.
        ({"stub_words": [0xE59FF0F8, 0xE59FF0F4, 0xE59FF0F8]}, "stub 0x2F04 (_objc_msgSend)"),
    ],
)
def test_problems(overrides, problem):
    problems = loader.Image(build(**overrides)).problems()
    assert any(p.startswith(problem) for p in problems), problems


def test_init_functions_need_whole_pointers():
    data = bytearray(build())
    m = macho.MachO(bytes(data))
    sect = m.section("__DATA", "__mod_init_func")
    # Find the section header by its name and shrink its size field to 6 bytes.
    header = data.index(b"__mod_init_func\0")
    assert int.from_bytes(data[header + 36 : header + 40], "little") == sect.size
    data[header + 36 : header + 40] = (6).to_bytes(4, "little")
    with pytest.raises(macho.MachOError, match="not whole pointers"):
        loader.Image(bytes(data)).init_functions  # noqa: B018


def test_manifest(image):
    doc = image.manifest()
    assert json.loads(gc.dump_json(doc)) == doc  # plain JSON types only
    assert list(doc) == [
        "schema",
        "source",
        "header",
        "entry_point",
        "segments",
        "sections",
        "dylibs",
        "imports",
        "binds",
        "rebases",
        "function_starts",
        "init_functions",
        "objc_sections",
    ]
    assert doc["schema"] == loader.SCHEMA_VERSION == 1
    assert doc["source"]["path"] is None and len(doc["source"]["sha256"]) == 64
    assert doc["header"] == {"cputype": 12, "cpusubtype": 9, "filetype": 2, "flags": "0x0"}
    assert doc["entry_point"] == "0x1801"
    assert doc["segments"][2] == {
        "name": "__DATA",
        "vmaddr": "0x3000",
        "vmsize": 0x1000,
        "fileoff": 0x2000,
        "filesize": 0x1000,
        "maxprot": 3,
        "initprot": 3,
    }
    stubs = next(s for s in doc["sections"] if s["name"] == "__symbolstub1")
    assert stubs == {
        "segment": "__TEXT",
        "name": "__symbolstub1",
        "addr": "0x2F00",
        "size": 12,
        "offset": 0x1F00,
        "type": 8,
        "attributes": "0x80000400",
        "zerofill": False,
    }
    assert doc["dylibs"][2] == {
        "ordinal": 3,
        "path": "/System/Library/Frameworks/GameKit.framework/GameKit",
        "weak": True,
    }
    assert doc["imports"][0] == {
        "name": "_foo",
        "library": "/usr/lib/libSystem.B.dylib",
        "weak": False,
        "kind": "function",
        "stubs": ["0x2F00"],
        "lazy_pointers": ["0x3000"],
        "non_lazy_pointers": [],
        "binds": [],
    }
    assert doc["imports"][5]["binds"] == ["0x301C"]
    assert doc["binds"][3] == {
        "kind": "weak",
        "address": "0x3000",
        "symbol": "_foo",
        "ordinal": 0,
        "type": 1,
        "addend": 0,
        "flags": 0,
    }
    assert doc["rebases"][0] == {"address": "0x3010", "type": 1}
    assert doc["function_starts"] == ["0x1801", "0x1811", "0x1821"]
    assert doc["init_functions"] == ["0x1811", "0x1821"]
    assert doc["objc_sections"][0] == {
        "segment": "__TEXT",
        "name": "__objc_methname",
        "addr": "0x2E00",
        "size": 7,
    }
    # Every address in the manifest parses as the repo writes addresses.
    for b in doc["binds"]:
        parse_address(b["address"])
    for imp in doc["imports"]:
        for key in ("stubs", "lazy_pointers", "non_lazy_pointers", "binds"):
            for a in imp[key]:
                parse_address(a)


def test_load_command(tmp_path, capsys):
    slice_path = tmp_path / "slice"
    slice_path.write_bytes(build())
    out = tmp_path / "recomp" / "loader.json"
    assert cli.main(["load", "--slice", str(slice_path), "--out", str(out)]) == 0
    doc = json.loads(out.read_text(encoding="utf-8"))
    assert doc["source"]["path"] == "slice"  # outside the repo: the file name
    assert doc == loader.Image(build()).manifest(doc["source"])
    printed = capsys.readouterr().out
    assert "imports          7" in printed and "0 problems" in printed


def test_load_command_reports_problems(tmp_path, capsys):
    slice_path = tmp_path / "slice"
    slice_path.write_bytes(build(entry=None))
    out = tmp_path / "loader.json"
    assert cli.main(["load", "--slice", str(slice_path), "--out", str(out)]) == 1
    assert out.is_file()
    assert "problem: no LC_UNIXTHREAD entry point" in capsys.readouterr().out
