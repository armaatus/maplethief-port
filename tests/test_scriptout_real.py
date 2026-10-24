"""Real-asset checks for the script disassembler (docs/formats/script-out.md, #19).

Skipped when work/ipa/ is absent (CI); run scripts/bootstrap first.
"""

from __future__ import annotations

from collections import Counter

import pytest

from mtport import objfile as of
from mtport import scriptout as so

pytestmark = pytest.mark.real_assets

# PUSHSTRING constants that are text, not Scripts.strings keys. regMsgNPC & co. pass them to
# NSLocalizedString, which returns an unknown key unchanged, so they would show as written
# (Korean). All but one sit in scripts that no .obj file names, i.e. never run.
INLINE_TEXT = {
    "chapter1/Sabi2.out": 1,  # "...": a placeholder line
    "chapter3/Darklord_new.out": 38,
    "common/Barun_old.out": 1,
    "common/Manzie_error.out": 1,
    "common/Spiegelman1.out": 25,
    "common/Spiegelman2.out": 25,
    "common/Spiegelman3.out": 25,
    "common/pig_old.out": 11,
}
UNREFERENCED = {
    "Barun_old",
    "Darklord_0827",
    "Darklord_new",
    "Darklord_ori",
    "Manzie_error",
    "Spiegelman1",
    "Spiegelman2",
    "Spiegelman3",
    "YetiMap",
    "pig_old",
}
OPCODES_USED = {
    "ADDOP", "ADJUST0", "CALLFUNC", "EQOP", "GEOP", "GTOP", "IFFJMP", "JMP", "LEOP", "LTOP",
    "MINUSOP", "ONFJMP", "ONTJMP", "POP", "PUSH0", "PUSH1", "PUSH2", "PUSHBYTE",
    "PUSHFUNCTION", "PUSHGLOBAL", "PUSHSTRING", "PUSHWORD", "RETCODE0", "STOREGLOBAL",
}  # fmt: skip


@pytest.fixture(scope="module")
def app(ipa_dir):
    return ipa_dir / "Payload" / "MapleThief.app"


@pytest.fixture(scope="module")
def table(app):
    return so.load_strings(app / "en.lproj" / "Scripts.strings")


@pytest.fixture(scope="module")
def chunks(app):
    root = app / "script"
    return {str(p.relative_to(root)): so.load(p) for p in sorted(root.rglob("*.out"))}


@pytest.fixture(scope="module")
def stats(app, table):
    result, errors = so.scan(app / "script", table)
    assert errors == []
    return result


def test_all_307_files_decode_with_no_unknown_opcode(chunks, stats):
    assert len(chunks) == 307
    folders = Counter(name.split("/")[0] for name in chunks)
    assert folders == {"chapter1": 27, "chapter2": 26, "chapter3": 29, "common": 225}
    assert stats.unknown == 0
    assert (stats.files, stats.functions, stats.instructions) == (307, 1086, 38102)
    assert set(stats.opcodes) == OPCODES_USED
    # The opcodes the game's interpreter gets wrong never occur (script-vm.md).
    assert not set(stats.opcodes) & so.DESYNC_OPCODES
    assert not any(op.startswith(("STORELOCAL", "PUSHLOCAL")) for op in stats.opcodes)


def test_every_byte_is_accounted_for(chunks):
    for name, c in chunks.items():
        for func in c.functions:
            code = c.instructions(func)
            assert b"".join(i.raw for i in code) == func.code, name
            assert all(f.source == "Temp.dat" for f in c.functions if f.is_main), name


def test_every_native_called_is_catalogued_with_its_arity(stats):
    assert set(stats.native_calls) <= set(so.NATIVE_BY_NAME)
    assert len(stats.native_calls) == 44
    for name, arities in stats.native_arity.items():
        native = so.NATIVE_BY_NAME[name]
        assert native.handler is not None, name
        assert set(arities) == {len(native.params)}, name
        # A literal argument always has the type the handler reads.
        for k, types in enumerate(stats.native_types[name]):
            literal = set(types) - {"any"}
            want = {"int": {"int"}, "string": {"string"}, "function": {"function"}}
            assert literal <= want[native.params[k]], (name, k, types)


def test_every_other_callee_is_a_function_of_the_same_file(stats):
    assert stats.other_calls
    assert {where for where, _ in stats.other_calls} == {"local"}


def test_every_dialogue_key_resolves(stats, table):
    assert len(stats.keys) == 1761
    assert stats.missing_keys == set()
    assert len(set(table) - stats.keys) == 458  # keys no script references (e.g. Back*)


def test_inline_text_exceptions(chunks, table):
    found = {}
    for name, c in chunks.items():
        n = sum(
            1
            for f in c.functions
            for i in c.instructions(f)
            if i.name == "PUSHSTRING" and i.names[0] not in table
        )
        if n:
            found[name] = n
    assert found == INLINE_TEXT


def test_scripts_named_by_maps(app, chunks):
    named = {r.name for p in (app / "map").glob("*.obj") for r in of.load(p).records if r.name}
    stems = {name.split("/")[1][:-4] for name in chunks}
    assert named <= stems
    assert stems - named == UNREFERENCED


def test_functions_per_file(chunks):
    counts = Counter(len(c.subfunctions) for c in chunks.values())
    assert max(counts) <= so.MAX_FUNCTIONS
    first = Counter(c.function_names().get(0) for c in chunks.values() if c.subfunctions)
    assert first["chkQuest"] == 114


def test_change_map_calls_take_three_literals(chunks):
    seen = 0
    for c in chunks.values():
        for f in c.functions:
            for call in so.calls(c, f):
                if call.callee == "changeMap":
                    seen += 1
                    assert [a.type for a in call.args] == ["int"] * 3
    assert seen == 173
