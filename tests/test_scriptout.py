"""Unit tests for the .out script loader and disassembler (synthetic bytecode only).

Format: docs/formats/script-out.md. The tests build Lua 2.4-style chunks with a tiny
assembler below: it lays out the code, threads every global / string operand into its
``V`` / ``S`` chain and writes the ``F`` records the way luac 2.4 does (minus the header).
"""

from __future__ import annotations

import json
import plistlib
import struct

import pytest

from mtport import names as nm
from mtport import scriptout as so
from mtport.bytereader import FormatError

STALE_POINTER = 0x049489F0  # what luac leaves in a PUSHFUNCTION operand


def _string(text: str, encoding: str = "utf-8") -> bytes:
    raw = text.encode(encoding) + b"\0"
    return struct.pack("<H", len(raw)) + raw


def assemble(code: list[tuple]) -> tuple[bytes, list[tuple[str, int, str]], list[int]]:
    """``[(opname, *operands)]`` -> (code bytes, [(kind, head, name)], PUSHFUNCTION offsets).

    Global / string operands are names; jump operands are instruction indexes (labels).
    """
    offsets, pos = [], 0
    for ins in code:
        offsets.append(pos)
        op = so.BY_NAME[ins[0]]
        pos += op.fixed_size if op.fixed_size is not None else 2 + 2 * len(ins[1:])
    offsets.append(pos)
    out = bytearray()
    uses: dict[tuple[str, str], list[int]] = {}
    funcs = []
    for index, ins in enumerate(code):
        op = so.BY_NAME[ins[0]]
        out.append(op.code)
        args = list(ins[1:])
        kinds = op.operands if op.name != "STORERECORD" else "b" + "s" * len(args)
        if op.name == "STORERECORD":
            args = [len(args), *args]
        args.reverse()
        for kind in kinds:
            arg = None if kind == "p" else args.pop()
            if kind == "b":
                out.append(arg)
            elif kind == "f":
                out += struct.pack("<f", arg)
            elif kind == "p":
                funcs.append(len(out))
                out += struct.pack("<I", STALE_POINTER)
            elif kind in "gs":
                uses.setdefault(("V" if kind == "g" else "S", arg), []).append(len(out))
                out += b"\0\0"
            elif kind == "j":
                out += struct.pack("<H", offsets[arg] - offsets[index + 1])
            elif kind == "u":
                out += struct.pack("<H", offsets[index + 1] - offsets[arg])
            else:
                out += struct.pack("<H", arg)
    refs = []
    for (kind, name), at in uses.items():
        for here, nxt in zip(
            at, at[1:], strict=False
        ):  # each use links to the next; the last ends with 0
            struct.pack_into("<H", out, here, nxt)
        refs.append((kind, at[0], name))
    return bytes(out), refs, funcs


def function(line: int, code: list[tuple], header: bytes) -> bytes:
    raw, refs, _ = assemble(code)
    out = b"F" + struct.pack("<HHH", len(raw) >> 16, len(raw) & 0xFFFF, line) + header + raw
    for kind, head, name in refs:
        out += kind.encode() + struct.pack("<H", head) + _string(name)
    return out


def chunk(main: list[tuple], *subs: tuple[int, list[tuple]]) -> bytes:
    """main + sub-functions ``(line, code)``; the n-th sub-function fills the n-th PUSHFUNCTION."""
    _, _, pushes = assemble(main)
    out = b"\x1b" + function(0, main, _string("Temp.dat"))
    for (line, code), marked in zip(subs, pushes, strict=False):
        out += function(line, code, struct.pack("<H", marked))
    return out


CHK = [
    ("ADJUST0",),
    ("PUSH1",),
    ("MINUSOP",),
    ("STOREGLOBAL", "notify"),
    ("PUSHGLOBAL", "getQuestState"),
    ("PUSH0",),
    ("PUSHBYTE", 12),
    ("CALLFUNC", 2, 1),
    ("STOREGLOBAL", "state"),
    ("PUSHGLOBAL", "state"),
    ("PUSH1",),
    ("GEOP",),
    ("IFFJMP", 16),
    ("PUSHGLOBAL", "setNpcHide"),
    ("PUSH1",),
    ("CALLFUNC", 1, 0),
    ("RETCODE0",),
]
DIALOG = [
    ("ADJUST0",),
    ("PUSHGLOBAL", "initMsg"),
    ("CALLFUNC", 0, 0),
    ("PUSHGLOBAL", "regMsgNPC"),
    ("PUSHSTRING", "Chapter1_Quest_Test.dialog.1"),
    ("CALLFUNC", 1, 0),
    ("PUSHGLOBAL", "regSelList"),
    ("PUSHSTRING", "Chapter1_Quest_Test.dialog.2"),
    ("PUSHWORD", 300),
    ("CALLFUNC", 2, 0),
    ("PUSHGLOBAL", "showMsg"),
    ("PUSH2",),
    ("CALLFUNC", 1, 0),
    ("RETCODE0",),
]
MAIN = [
    ("PUSHFUNCTION",),
    ("STOREGLOBAL", "chkQuest"),
    ("PUSHFUNCTION",),
    ("STOREGLOBAL", "dialog"),
    ("PUSHGLOBAL", "dialog"),
    ("CALLFUNC", 0, 0),
    ("RETCODE0",),
]
NPC = chunk(MAIN, (5, CHK), (20, DIALOG))


def test_header_layout_matches_a_shipped_file():
    # Manzie.out begins 1B 46 | 00 00 17 00 | 00 00 | 09 00 "Temp.dat\0" | 08 <stale ptr>.
    data = NPC
    assert data[:1] == b"\x1b"
    c = so.decode(data)
    assert data[1:8] == b"F\0\0\x17\0\0\0"
    assert data[8:20] == b"\x09\x00Temp.dat\x00\x08"
    assert len(c.main.code) == 0x17 and c.main.source == "Temp.dat"


def test_functions_slots_and_names():
    c = so.decode(NPC)
    assert [f.slot for f in c.functions] == [None, 0, 1]
    assert [f.line for f in c.functions] == [0, 5, 20]
    assert [f.marked for f in c.subfunctions] == [1, 9]
    assert c.function_names() == {0: "chkQuest", 1: "dialog"}
    # The stale compiler pointer is replaced by the slot, as the loader does.
    pushes = [i for i in c.instructions(c.main) if i.name == "PUSHFUNCTION"]
    assert [i.operands for i in pushes] == [(0,), (1,)]
    assert pushes[0].raw[1:] == struct.pack("<I", STALE_POINTER)


def test_threaded_operands_resolve_to_names():
    c = so.decode(NPC)
    chk = c.instructions(c.functions[1])
    globals_ = [i.names[0] for i in chk if i.name in ("PUSHGLOBAL", "STOREGLOBAL")]
    assert globals_ == ["notify", "getQuestState", "state", "state", "setNpcHide"]
    links = so.chains(c.functions[1])
    state = [at for at, ref in links.items() if ref.name == "state"]
    assert len(state) == 2  # one V record, two uses chained together


def test_jump_targets_and_sizes():
    c = so.decode(NPC)
    chk = c.instructions(c.functions[1])
    jump = next(i for i in chk if i.name == "IFFJMP")
    assert jump.target == chk[16].offset  # the RETCODE0
    assert chk[-1].end == len(c.functions[1].code)
    assert sum(len(i.raw) for i in chk) == len(c.functions[1].code)


def test_backward_jump():
    loop = [
        ("ADJUST0",),
        ("PUSHGLOBAL", "x"),
        ("IFFJMP", 4),
        ("UPJMP", 1),
        ("RETCODE0",),
    ]
    c = so.decode(chunk([("PUSHFUNCTION",), ("STOREGLOBAL", "f"), ("RETCODE0",)], (3, loop)))
    code = c.instructions(c.functions[1])
    assert code[3].name == "UPJMP" and code[3].target == code[1].offset
    assert code[2].target == code[4].offset


def test_every_opcode_decodes():
    body = []
    for op in so.OPCODES:
        if op.name in ("PUSHFUNCTION", "RETCODE0") or op.name in so.JUMPS:
            continue
        args = {"b": 1, "w": 7, "f": 1.5, "g": "g", "s": "s", "r": None}
        if op.name == "STORERECORD":
            body.append((op.name, "k1", "k2"))
        else:
            body.append((op.name, *[args[k] for k in op.operands]))
    body.append(("RETCODE0",))
    c = so.decode(chunk([("PUSHFUNCTION",), ("STOREGLOBAL", "f"), ("RETCODE0",)], (1, body)))
    names = [i.name for i in c.instructions(c.functions[1])]
    assert names == [b[0] for b in body]
    rec = next(i for i in c.instructions(c.functions[1]) if i.name == "STORERECORD")
    assert rec.operands == (2, 0, 0) and rec.names == ("k1", "k2")
    flt = next(i for i in c.instructions(c.functions[1]) if i.name == "PUSHFLOAT")
    assert "1.5" in so.listing(c) and len(flt.raw) == 5


def test_opcode_table_is_complete_and_contiguous():
    assert len(so.OPCODES) == 0x43
    assert so.BY_NAME["CALLFUNC"].code == 0x3F and so.BY_NAME["SETLINE"].code == 0x42
    assert so.BY_NAME["STORELIST"].fixed_size == 3 and so.BY_NAME["STORERECORD"].fixed_size is None


def test_unknown_opcode_is_reported_not_fatal():
    c = so.decode(chunk([("PUSHNIL",), ("RETCODE0",)]))
    bad = so.Function(None, 0, "Temp.dat", None, bytes([0x43, 0x40]), (), 0)
    code = so.decode_code(bad)
    assert [i.name for i in code] == ["UNKNOWN_43", "RETCODE0"]
    assert c.main.code == bytes([0x00, 0x40])


def test_calls_recover_callee_and_argument_types():
    c = so.decode(NPC)
    calls = so.calls(c, c.functions[2])
    assert [(k.callee, len(k.args), k.results) for k in calls] == [
        ("initMsg", 0, 0),
        ("regMsgNPC", 1, 0),
        ("regSelList", 2, 0),
        ("showMsg", 1, 0),
    ]
    assert [a.type for a in calls[2].args] == ["string", "int"]
    assert calls[2].args[1].text == "300"
    chk = so.calls(c, c.functions[1])
    assert chk[0].callee == "getQuestState" and [a.text for a in chk[0].args] == ["0", "12"]


def test_nested_call_as_argument():
    body = [
        ("ADJUST0",),
        ("PUSHGLOBAL", "setNpcHide"),
        ("PUSHGLOBAL", "getLevel"),
        ("CALLFUNC", 0, 1),
        ("CALLFUNC", 1, 0),
        ("RETCODE0",),
    ]
    c = so.decode(chunk([("PUSHFUNCTION",), ("STOREGLOBAL", "f"), ("RETCODE0",)], (2, body)))
    calls = so.calls(c, c.functions[1])
    assert [k.callee for k in calls] == ["getLevel", "setNpcHide"]
    assert calls[1].args[0].kind == "call"


def test_listing_resolves_dialogue_text():
    table = {
        "Chapter1_Quest_Test.dialog.1": "Hello there.",
        "Chapter1_Quest_Test.dialog.2": "Yes",
    }
    text = so.listing(so.decode(NPC, "Test.out"), table)
    assert '"Chapter1_Quest_Test.dialog.1"  ; "Hello there."' in text
    assert "function f1 dialog  ; line 20" in text
    assert 'regSelList("Chapter1_Quest_Test.dialog.2", 300) [native]' in text
    assert "PUSHFUNCTION  f0 (chkQuest)" in text
    missing = so.listing(so.decode(NPC), {})
    assert "key not in Scripts.strings" in missing


def test_stats_count_keys_natives_and_opcodes(tmp_path):
    (tmp_path / "chapter1").mkdir()
    (tmp_path / "chapter1" / "Test.out").write_bytes(NPC)
    table = {"Chapter1_Quest_Test.dialog.1": "Hello"}
    stats, errors = so.scan(tmp_path, table)
    assert errors == [] and stats.files == 1 and stats.functions == 3 and stats.unknown == 0
    assert stats.native_calls == {
        "getQuestState": 1,
        "setNpcHide": 1,
        "initMsg": 1,
        "regMsgNPC": 1,
        "regSelList": 1,
        "showMsg": 1,
    }
    assert stats.native_arity["regSelList"] == {2: 1}
    assert stats.missing_keys == {"Chapter1_Quest_Test.dialog.2"}
    assert stats.other_calls == {("local", "dialog"): 1}


def test_scan_names_files_by_their_posix_path(tmp_path):
    """Decode errors name the file as chapter1/Bad.out on every OS, never with a backslash."""
    (tmp_path / "chapter1").mkdir()
    (tmp_path / "chapter1" / "Bad.out").write_bytes(b"\x1bX")
    _, errors = so.scan(tmp_path)
    assert len(errors) == 1 and errors[0].startswith("chapter1/Bad.out:"), errors


def test_euc_kr_strings_decode():
    body = [("ADJUST0",), ("PUSHSTRING", "몬스터"), ("POP",), ("RETCODE0",)]
    data = bytearray(chunk([("PUSHFUNCTION",), ("STOREGLOBAL", "f"), ("RETCODE0",)], (1, body)))
    utf8 = _string("몬스터")
    data = bytes(data).replace(utf8, _string("몬스터", "euc_kr"))
    c = so.decode(data)
    assert c.instructions(c.functions[1])[1].names == ("몬스터",)


def test_strings_table(tmp_path):
    path = tmp_path / "Scripts.strings"
    path.write_bytes(plistlib.dumps({"a.b.1": "x"}, fmt=plistlib.FMT_BINARY))
    assert so.load_strings(path) == {"a.b.1": "x"}
    path.write_bytes(plistlib.dumps(["not", "a", "table"]))
    with pytest.raises(FormatError):
        so.load_strings(path)


@pytest.mark.parametrize(
    "text, ok",
    [
        ("Chapter1_Quest_Manji.dialog.1", True),
        ("ManufactureItem_Chapter2.ItemListEar.12", True),
        ("a.b", False),
        ("a.b.c", False),
        ("Hello there.", False),
        ("a b.c.1", False),
    ],
)
def test_is_dialogue_key(text, ok):
    assert so.is_dialogue_key(text) is ok


# -- malformed input ---------------------------------------------------------------------------


def corrupt(data: bytes, offset: int, value: bytes) -> bytes:
    return data[:offset] + value + data[offset + len(value) :]


@pytest.mark.parametrize(
    "data, message",
    [
        (b"", "chunk id"),
        (b"\x1c", "not a script chunk"),
        (b"\x1b", "no functions"),
        (b"\x1bX", "expected an 'F' record"),
        (NPC[:30], "needs"),
        (NPC + b"\x00", "expected an 'F' record"),
        (b"\x1bF\0\0\0\0\x05\0\0\0", "first function is not main"),
        (corrupt(NPC, 8, b"\x00\x00"), "length 0"),
        (corrupt(NPC, 10, b"T\0mp"), "not one NUL-terminated"),
    ],
)
def test_rejects_malformed_files(data, message):
    with pytest.raises(FormatError, match=message):
        so.decode(data)


def test_rejects_a_second_main():
    with pytest.raises(FormatError, match="second main"):
        so.decode(NPC + function(0, [("RETCODE0",)], _string("x")))


def test_rejects_too_many_functions():
    main = [("PUSHFUNCTION",)] * 31 + [("RETCODE0",)]
    subs = [(1, [("RETCODE0",)])] * 31
    with pytest.raises(FormatError, match="slot table holds 30"):
        so.decode(chunk(main, *subs))


def test_rejects_bad_chains():
    good = so.decode(NPC)
    func = good.functions[1]
    raw = bytearray(func.code)
    struct.pack_into("<H", raw, 4, 4)  # the chain points at itself
    loop = so.Function(1, 5, None, 1, bytes(raw), (so.SymbolRef("V", 4, "x"),), 0)
    with pytest.raises(FormatError, match="reused"):
        so.chains(loop)
    outside = so.Function(1, 5, None, 1, func.code, (so.SymbolRef("V", 0x400, "x"),), 0)
    with pytest.raises(FormatError, match="outside the code"):
        so.chains(outside)
    # A global operand reached by an 'S' chain instead of a 'V' chain.
    wrong = so.Function(1, 5, None, 1, func.code, (so.SymbolRef("S", 4, "notify"),), 0)
    with pytest.raises(FormatError, match="no V record"):
        so.decode_code(wrong)
    # A chain that ends on a byte that is not a threaded operand (the MINUSOP at 2).
    stray = func.refs + (so.SymbolRef("S", 2, "x"),)
    raw = bytearray(func.code)
    raw[2:4] = b"\0\0"
    with pytest.raises(FormatError):
        so.decode_code(so.Function(1, 5, None, 1, bytes(raw), stray, 0))


def test_rejects_a_jump_into_an_instruction():
    body = [("ADJUST0",), ("JMP", 3), ("PUSHBYTE", 1), ("RETCODE0",)]
    data = chunk([("PUSHFUNCTION",), ("STOREGLOBAL", "f"), ("RETCODE0",)], (1, body))
    at = data.index(bytes([0x3A, 2, 0]))
    with pytest.raises(FormatError, match="JMP"):
        so.decode(corrupt(data, at + 1, b"\x01"))


def test_rejects_unclaimed_pushfunction():
    with pytest.raises(FormatError, match="no function"):
        so.decode(chunk([("PUSHFUNCTION",), ("STOREGLOBAL", "f"), ("RETCODE0",)]))


def test_rejects_marked_that_misses_a_pushfunction():
    data = bytearray(NPC)
    second = data.index(b"F", 2 + 1 + 6 + 11 + 0x17)  # the first sub-function's record
    struct.pack_into("<H", data, second + 7, 2)  # marked -> not a PUSHFUNCTION operand
    with pytest.raises(FormatError, match="not a free PUSHFUNCTION operand"):
        so.decode(bytes(data))


# -- natives and names ---------------------------------------------------------------------


def test_native_table_order_matches_the_binary():
    # 0x117950 lists 65 names; the dispatcher's cases run from 6 (println) to 64 (removeItem).
    assert len(so.NATIVES) == 65
    assert [n.name for n in so.NATIVES[:3]] == ["assert", "dofile", "dostring"]
    assert so.NATIVE_BY_NAME["regCallBack"].index == 13
    assert so.NATIVE_BY_NAME["removeItem"].index == 64
    with_case = [n for n in so.NATIVES if n.handler is not None]
    assert len(with_case) == 53 and min(n.index for n in with_case) == 6
    assert all(h & 1 for h in (n.handler for n in with_case))  # Thumb function pointers
    assert so.NATIVE_BY_NAME["changeMap"].params == ("int", "int", "int")


def test_names_file_is_valid():
    doc = json.loads(so.names_json())
    assert doc["source"] == "scriptvm"
    source, entries = nm.parse(doc, "scriptvm.json")
    assert source == "scriptvm"
    locations = [e.location for e in entries]
    assert len(locations) == len(set(locations))
    by_name = {e.name: e for e in entries}
    assert by_name["Lua::execute"].address == 0x9BC11
    assert by_name["ScriptNative_changeMap"].address == 0x99969
    assert by_name["g_scriptNativeNames"].kind == "data"


def test_cli_dis_and_names(tmp_path, capsys):
    path = tmp_path / "Test.out"
    path.write_bytes(NPC)
    assert so.main(["dis", str(path)]) == 0
    assert "function f0 chkQuest" in capsys.readouterr().out
    out = tmp_path / "names" / "scriptvm.json"
    assert so.main(["names", str(out)]) == 0
    assert json.loads(out.read_text(encoding="utf-8"))["source"] == "scriptvm"
    (tmp_path / "bad.out").write_bytes(b"\x1bX")
    assert so.main(["dis", str(tmp_path / "bad.out")]) == 1


def test_native_table_in_the_docs_matches_the_catalogue():
    from repo_paths import REPO

    text = (REPO / "docs" / "re" / "script-vm.md").read_text(encoding="utf-8")
    rows = {}
    for line in text.splitlines():
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) == 7 and cells[0].isdigit():
            rows[int(cells[0])] = cells
    assert sorted(rows) == list(range(len(so.NATIVES)))
    for n in so.NATIVES:
        index, name, handler, args, returns = rows[n.index][:5]
        assert name == f"`{n.name}`"
        assert handler == (f"`0x{n.handler:X}`" if n.handler else "–")
        assert args == (", ".join(n.params) or "–")
        assert returns == (n.returns or "–")
        assert rows[n.index][6] == n.does
