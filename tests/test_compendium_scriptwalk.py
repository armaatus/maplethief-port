"""mtport.compendium.scriptwalk and the quests stage (#217) on synthetic bytecode only.

Scripts are written with a small structured assembler on top of tests/test_scriptout.py's
``chunk``: ``if_`` / ``and_`` / ``or_`` lay out the forward jumps the way luac 2.4 does, and
``call`` / ``get`` push a native and its arguments. Every key and text below is made up.
"""

from __future__ import annotations

import itertools
import plistlib
from dataclasses import dataclass
from pathlib import Path

import pytest

from mtport import scriptout as so
from mtport.compendium import kb as kbm
from mtport.compendium import quests
from mtport.compendium import scriptwalk as sw
from test_compendium_kb import make_app
from test_scriptout import chunk

# -- a structured assembler -------------------------------------------------------------------

JUMPS = ("JMP", "IFFJMP", "ONTJMP", "ONFJMP")
_labels = itertools.count()


@dataclass(frozen=True)
class Label:
    name: str


class G(str):
    """A global read as an argument (a script function for regCallBack)."""


def _label(kind: str) -> Label:
    return Label(f"{kind}{next(_labels)}")


def lay(items: list) -> list[tuple]:
    """Instructions with labels -> instructions with jump operands as instruction indexes."""
    where: dict[str, int] = {}
    code: list[tuple] = []
    for it in items:
        if isinstance(it, Label):
            where[it.name] = len(code)
        else:
            code.append(it)
    return [(ins[0], where[ins[1]]) if ins[0] in JUMPS else ins for ins in code]


def push(v) -> list[tuple]:
    if v is None:
        return [("PUSHNIL",)]
    if isinstance(v, G):
        return [("PUSHGLOBAL", str(v))]
    if isinstance(v, str):
        return [("PUSHSTRING", v)]
    if v < 0:
        return [*push(-v), ("MINUSOP",)]
    if v <= 2:
        return [(f"PUSH{v}",)]
    return [("PUSHBYTE", v)] if v < 256 else [("PUSHWORD", v)]


def call(fn: str, *args, n: int = 0) -> list:
    out: list = [("PUSHGLOBAL", fn)]
    for a in args:
        out += a if isinstance(a, list) else push(a)
    return [*out, ("CALLFUNC", len(args), n)]


def get(fn: str, *args) -> list:
    return call(fn, *args, n=1)


def cmp(x: list, op: str, v: int) -> list:
    return [*x, *push(v), ({"==": "EQOP", "<": "LTOP", ">=": "GEOP"}[op],)]


def eq(x: list, v: int) -> list:
    return cmp(x, "==", v)


def qs(kind: int, number: int) -> list:
    return get("getQuestState", kind, number)


def evt() -> list:
    return get("getEvtIndex")


def if_(cond: list, then: list, else_: list | None = None) -> list:
    e, end = _label("else"), _label("end")
    if not else_:
        return [*cond, ("IFFJMP", e.name), *then, e]
    return [*cond, ("IFFJMP", e.name), *then, ("JMP", end.name), e, *else_, end]


def and_(a: list, b: list) -> list:
    done = _label("and")
    return [*a, ("ONFJMP", done.name), ("POP",), *b, done]


def or_(a: list, b: list) -> list:
    done = _label("or")
    return [*a, ("ONTJMP", done.name), ("POP",), *b, done]


def script(*funcs: tuple[str, list], main: list | None = None) -> bytes:
    """Main stores each function in its global (slot n = the n-th), then runs ``main``."""
    head: list = []
    for name, _ in funcs:
        head += [("PUSHFUNCTION",), ("STOREGLOBAL", name)]
    body = lay([*head, *(main or []), ("RETCODE0",)])
    subs = [(10 * k + 5, lay([*code, ("RETCODE0",)])) for k, (_, code) in enumerate(funcs)]
    return chunk(body, *subs)


def npc(state: list, dialog: list, *more: tuple[str, list]) -> bytes:
    """An NPC script: ``chkQuest`` in slot 0 (the NPC-state pass), main runs ``dialog``."""
    return script(("chkQuest", state), ("dialog", dialog), *more, main=call("dialog"))


def load(data: bytes, name: str = "chapter1/Test") -> sw.Script:
    return sw.Script(so.decode(data, name), name)


def natives(p: sw.Path | sw.Step) -> tuple[str, ...]:
    return tuple(e.native for e in p.events)


def conds(p: sw.Path | sw.Step) -> set[tuple]:
    return {(c.term.name or c.term.op, c.term.args, c.op, c.value) for c in p.conds}


# -- the walker -------------------------------------------------------------------------------


def test_slots_names_and_entry_points():
    s = load(npc([], [], ("onAnswer", [])))
    assert s.names == {0: "chkQuest", 1: "dialog", 2: "onAnswer"}
    assert sw.entry_points(s) == [("main", None), ("state", 0)]
    assert sw.entry_points(load(script(main=call("changeMap", 0, 1, 0)))) == [("main", None)]


def test_contradictory_branches_are_pruned():
    body = [
        *if_(eq(qs(0, 1), 0), call("setNpcNotify", 1)),
        *if_(eq(qs(0, 1), 0), call("setNpcHide", 1)),
        # the native's range: getLevel is 1..255, checkRequire 0 or 1
        *if_(cmp(get("getLevel"), "<", 1), call("startBoss")),
        *if_(
            eq(get("checkRequire", 0, 1), 1),
            call("setNpcDir", 1),
            if_(eq(get("checkRequire", 0, 1), 0), call("setNpcDir", 0), call("startBoss")),
        ),
    ]
    paths = sw.walk(load(script(("f", body), main=call("f"))), None)
    assert sorted(natives(p) for p in paths) == [
        ("setNpcDir",),
        ("setNpcDir",),
        ("setNpcNotify", "setNpcHide", "setNpcDir"),
        ("setNpcNotify", "setNpcHide", "setNpcDir"),
    ]
    both = [p for p in paths if "setNpcNotify" in natives(p)]
    assert all(("getQuestState", (0, 1), "==", 0) in conds(p) for p in both)


def test_a_native_that_changes_the_game_starts_a_new_epoch():
    body = [
        *if_(eq(qs(0, 1), 0), call("startQuest")),
        *if_(eq(qs(0, 1), 0), call("setNpcNotify", 1)),
    ]
    paths = sw.walk(load(script(main=body)), None)
    assert sorted(natives(p) for p in paths) == [
        (),
        ("startQuest",),
        ("startQuest", "setNpcNotify"),
    ]


def test_and_or_short_circuit():
    both = and_(cmp(get("getLevel"), ">=", 30), eq(get("getHeroClass"), 1))
    paths = sw.walk(load(script(main=if_(both, call("setClassUp", 1, 2)))), None)
    assert len(paths) == 3  # the two ways to miss do not cover every case: they stay apart
    (up,) = [p for p in paths if natives(p)]
    assert conds(up) == {("getLevel", (), ">=", 30), ("getHeroClass", (), "==", 1)}

    either = or_(eq(get("getLevel"), 1), eq(qs(0, 1), 2))
    paths = sw.walk(load(script(main=if_(either, call("setNpcHide", 1)))), None)
    hits = sorted((conds(p) for p in paths if natives(p)), key=len)
    assert [{c[2:] for c in h} for h in hits] == [{("==", 1)}, {("!=", 1), ("==", 2)}]
    assert [conds(p) for p in paths if not natives(p)] == [
        {("getLevel", (), "!=", 1), ("getQuestState", (0, 1), "!=", 2)}
    ]


def test_merged_globals_split_where_they_are_read():
    """Five independent flags: merging keeps the walk small, and every combination of quest
    states still lands on exactly one path with the right outcome."""
    body = [("PUSHNIL",), ("STOREGLOBAL", "x")]
    for k in range(1, 6):
        body += if_(eq(qs(0, k), 2), [("PUSH1",), ("STOREGLOBAL", "x")])
    body += if_([("PUSHGLOBAL", "x")], call("setNpcNotify", 1))
    paths = sw.walk(load(script(main=body)), None)
    assert len(paths) < 2**5
    for states in itertools.product((0, 2), repeat=5):
        env = dict(zip(range(1, 6), states, strict=True))
        fits = [
            p for p in paths if all(sw._holds(c.op, env[c.term.args[1]], c.value) for c in p.conds)
        ]
        assert len(fits) == 1, states
        assert (natives(fits[0]) == ("setNpcNotify",)) == (2 in states)


def test_past_the_budget_merges_are_approximate_and_say_so(monkeypatch):
    """An or-chain over flags: its exact paths are many; past the budget the walk merges them
    anyway and marks the merged paths."""
    body = []
    for k in range(1, 4):
        body += if_(eq(qs(0, k), 2), [("PUSH2",), ("STOREGLOBAL", f"f{k}")])
    chain = eq([("PUSHGLOBAL", "f1")], 2)
    for k in (2, 3):
        chain = or_(chain, eq([("PUSHGLOBAL", f"f{k}")], 2))
    body += if_(chain, call("setNpcNotify", 1))
    s = load(script(("f", body), main=call("f")))
    tree, exact = sw.conversation(s, None)
    assert exact and all(sw.APPROX not in st.conds for st in tree.walk_steps())
    monkeypatch.setattr(sw, "EXACT_PATHS", 2)
    tree, exact = sw.conversation(s, None)
    assert not exact
    hits = [st for st in tree.walk_steps() if natives(st) == ("setNpcNotify",)]
    assert len(hits) == 1 and sw.APPROX in hits[0].conds


def _shop_dialog() -> bytes:
    dialog = [
        *call("initMsg"),
        *call("regMsgNPC", "T_A.dialog.1"),
        *call("regCallBack", G("onAsk")),
        *call("showMsg", 1),
    ]
    on_ask = if_(
        eq(evt(), 1),
        [
            *call("initMsg"),
            *call("regSelList", "T_A.dialog.2", 10),
            *call("regSelList", "T_A.dialog.3", 20),
            *call("regCallBack", G("onPick")),
            *call("showMsg", 2),
        ],
        [*call("regMsgNPC", "T_A.dialog.4"), *call("regCallBack", G("onAsk")), *call("showMsg", 1)],
    )
    on_pick = [
        *if_(eq(evt(), 10), call("showMarket", 3)),
        *if_(eq(evt(), 20), call("changeMap", 0, 1, 0)),
        *if_(eq(evt(), 30), call("startBoss")),  # no row has 30
    ]
    return npc([], dialog, ("onAsk", on_ask), ("onPick", on_pick))


def test_dialogue_follows_nested_callbacks_and_choices():
    root = sw.dialogue(load(_shop_dialog()), None)
    (start,) = root.steps
    ask = start.next
    assert (ask.function, ask.answers) == ("onAsk", (0, 1))
    accept, refuse = sorted(ask.steps, key=lambda s: s.next is None)
    assert accept.show[0] == sw.MODE_LIST
    assert [c.key for c in accept.show[1]] == ["T_A.dialog.2", "T_A.dialog.3"]
    pick = accept.next
    assert (pick.function, pick.answers) == ("onPick", (10, 20))
    assert sorted(natives(s) for s in pick.steps) == [("changeMap",), ("showMarket",)]
    # Refuse asks again: the same point of the conversation is a loop, not a deeper tree.
    assert refuse.loop == "onAsk" and refuse.next is None
    assert all("startBoss" not in natives(s) for s in root.walk_steps())


def test_answers_of_each_box_mode():
    rows = [sw.Choice("T_A.dialog.2", 10), sw.Choice(None, 20)]
    assert sw.answers_of(sw.MODE_ACCEPT, rows) == {0, 1}
    assert sw.answers_of(sw.MODE_ACCEPT2, []) == {0, 1}
    assert sw.answers_of(sw.MODE_LIST, rows) == {10, 20}
    assert sw.answers_of(sw.MODE_LIST, []) == {sw.NO_ANSWER}
    assert sw.answers_of(sw.MODE_OK, rows) == {sw.NO_ANSWER}
    assert sw.answers_of(sw.Term("call", "getLevel"), rows) is None


def test_unknown_opcodes_and_recursion_are_walk_errors():
    data = bytearray(chunk([("PUSHNIL",), ("RETCODE0",)]))
    data[-2] = 0x43  # no such opcode
    with pytest.raises(sw.WalkError, match="unknown opcode 0x43"):
        sw.walk(load(bytes(data)), None)
    loop = script(("f", call("f")), main=call("f"))
    with pytest.raises(sw.WalkError, match="recursive call of f"):
        sw.walk(load(loop), None)
    with pytest.raises(sw.WalkError, match="arguments"):
        sw.walk(load(script(main=call("showMsg"))), None)


def test_standalone_reads_unset_globals_as_free_terms():
    f = if_(eq([("PUSHGLOBAL", "who")], 1), call("setNpcHide", 1))
    s = load(script(("f", f), main=[("PUSH1",), ("STOREGLOBAL", "who")]))
    paths = sw.standalone(s, 0)
    assert sorted(natives(p) for p in paths) == [(), ("setNpcHide",)]
    assert {c.term for p in paths for c in p.conds} == {sw.Term("global", "who")}


def test_simplify_drops_conditions_that_cover_every_value():
    lvl = sw.Term("call", "getLevel")
    q = sw.Term("call", "getQuestState", (0, 1))
    alts = [
        (sw.Cond(lvl, ">=", 10), sw.Cond(q, "==", 0)),
        (sw.Cond(lvl, ">=", 10), sw.Cond(q, "==", 1)),
        (sw.Cond(lvl, ">=", 10), sw.Cond(q, ">=", 2)),
        (sw.Cond(lvl, ">=", 10), sw.Cond(q, "==", 1), sw.Cond(lvl, "<", 50)),
    ]
    assert sw.simplify(alts) == [(sw.Cond(lvl, ">=", 10),)]
    # groups whose union is one of their conditions become that condition
    assert sw.simplify(
        [
            (sw.Cond(lvl, "<", 30), sw.Cond(lvl, ">=", 10), sw.Cond(q, "==", 1)),
            (sw.Cond(lvl, ">=", 10), sw.Cond(lvl, ">=", 30), sw.Cond(q, "==", 1)),
        ]
    ) == [(sw.Cond(lvl, ">=", 10), sw.Cond(q, "==", 1))]
    # a == condition makes the others on the same term redundant
    assert sw.simplify([(sw.Cond(q, "!=", 0), sw.Cond(q, "==", 2))]) == [(sw.Cond(q, "==", 2),)]


# -- the quests stage on a synthetic bundle -------------------------------------------------
#
# make_app's world: map 0-0 (NPC 0 runs Lee and Lee2, portal000 -> 0-1), map 0-1 (map script
# TestMap, portal010 -> 0-0, portal013 -> 1-0), map 1-0 (portal100: common -> 0-1, chapter 1
# -> 0-6 once main quest 1 is done) and map 0-6 (NPC 1 runs Guard). The new game starts on
# 0-0 here; TestMap ends chapter 0 back on 0-0.

COMMON_LEE = npc(
    if_(eq(qs(0, 1), 0), call("setNpcNotify", 1), call("setNpcNotify", 0)),
    [
        *call("initMsg"),
        *if_(
            eq(qs(0, 1), 0),
            [
                *call("setQuest", 0, 1, "T_Quest.main1.1", "T_Quest.main1.2"),
                *call("regRequireMob", 2, 5),
                *call("regRewardExp", 81),
                *call("regRewardMeso", 550),
                *call("regRewardItem", 1, 1, 3),
                *call("regMsgNPC", "T_Lee.dialog.1"),
                *call("regCallBack", G("onOffer")),
                *call("showMsg", 1),
            ],
            if_(
                eq(qs(0, 1), 1),
                if_(
                    eq(get("checkRequire", 0, 1), 1),
                    [
                        *call("getReward", 0, 1),
                        *call("regMsgNPC", "T_Lee.dialog.3"),
                        *call("showMsg", 0),
                    ],
                    [*call("regMsgNPC", "T_Lee.dialog.4"), *call("showMsg", 0)],
                ),
            ),
        ),
    ],
    (
        "onOffer",
        if_(
            eq(evt(), 1),
            [*call("startQuest"), *call("regMsgHero", "T_Lee.dialog.2", 1), *call("showMsg", 0)],
        ),
    ),
)

CHAPTER1_LEE = npc(
    [],
    [
        *call("initMsg"),
        *if_(
            eq(qs(1, 1), 0),
            [
                *call("setQuest", 1, 1, "T_Quest.sub1.1", "T_Quest.sub1.2"),
                *call("regRequireItem", 1, 1, 2),
                *call("regRewardItem", 0, 0, 1),
                *call("regCallBack", G("onOffer")),
                *call("showMsg", 1),
            ],
            if_(
                eq(qs(1, 1), 2),
                if_(
                    eq(qs(1, 2), 0),
                    [
                        *call("setQuest", 1, 2, "T_Quest.sub2.1", "T_Quest.sub2.2"),
                        *call("regRequireMeso", 100),
                        *call("regRewardExp", 10),
                        *call("regCallBack", G("onOffer")),
                        *call("showMsg", 3),
                    ],
                ),
            ),
        ),
    ],
    ("onOffer", if_(eq(evt(), 1), call("startQuest"))),
)

LEE2 = script(
    main=[
        *call("showMarket", 0),
        *if_(eq(get("getItem", 1, 1, 1), 1), call("startBoss")),
    ]
)

GUARD = npc(
    [],
    [
        *call("initMsg"),
        *call("regSelList", "T_Guard.dialog.1", 1),
        *call("regSelList", "T_Guard.dialog.2", 2),
        *call("regSelList", "T_Guard.dialog.3", 3),
        *call("regSelList", "T_Guard.dialog.4", 4),
        *call("regCallBack", G("onPick")),
        *call("showMsg", 2),
    ],
    (
        "onPick",
        [
            *if_(
                eq(evt(), 1),
                [
                    *call("setQuest", 1, 77, "T_Guard.craft.1", "T_Guard.craft.2"),
                    *call("regRequireItem", 1, 1, 4),
                    *call("regRequireMeso", 300),
                    *call("regRewardItem", 0, 0, 1),
                    *call("makeEquipItem", 1, 77),
                    *call("removeQuest", 1, 77),
                ],
            ),
            *if_(
                eq(evt(), 2),
                [
                    *call("setQuest", 1, 78, "T_Guard.trade.1", "T_Guard.trade.2"),
                    *call("regRequireItem", 1, 0, 1),
                    *call("regRewardItem", 1, 1, 2),
                    *call("removeQuest", 1, 78),
                ],
            ),
            *if_(eq(evt(), 3), call("showMarket", 3)),
            *if_(eq(evt(), 4), if_(cmp(get("getLevel"), ">=", 10), call("setClassUp", 1, 1))),
        ],
    ),
)

TEST_MAP = script(
    main=[
        *call("setNPC", 0),
        *call("regMsgNPC", "T_Map.dialog.1"),
        *call("showMsg", 0),
        *if_(
            eq(qs(0, 1), 2),
            [*call("runAnimation", 5), *call("clearAll"), *call("changeMap", 0, 0, 0)],
        ),
    ]
)

GATED_PORTAL = script(main=if_(eq(qs(0, 1), 2), call("changeMap", 0, 6, 0)))

SCRIPTS = {
    "common/Lee": COMMON_LEE,
    "chapter1/Lee": CHAPTER1_LEE,
    "common/Lee2": LEE2,
    "common/Guard": script(),
    "chapter1/Guard": GUARD,
    "common/TestMap": TEST_MAP,
    "chapter1/TestMap": script(),  # no chapter end in chapter 1: chapters 2 and 3 stay empty
    "chapter1/portal100": GATED_PORTAL,
    "common/portal999": script(main=call("initMonCa", 1)),  # no map names it
}
NO_KOREAN = "T_Lee.dialog.4"


def _keys() -> set[str]:
    out = set()
    for data in SCRIPTS.values():
        c = so.decode(data)
        for f in (c.main, *c.subfunctions):
            for ins in c.instructions(f):
                if ins.name == "PUSHSTRING" and so.is_dialogue_key(ins.names[0]):
                    out.add(ins.names[0])
    return out


@pytest.fixture()
def kb(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> dict:
    app = make_app(tmp_path)
    for rel, data in SCRIPTS.items():
        (app / "script" / f"{rel}.out").write_bytes(data)
    for lang in ("en", "ko"):
        table = {k: f"{lang.upper()} {k}" for k in _keys() if lang == "en" or k != NO_KOREAN}
        path = app / f"{lang}.lproj" / "Scripts.strings"
        path.write_bytes(plistlib.dumps(table, fmt=plistlib.FMT_BINARY))
    monkeypatch.setattr(quests, "INTRO_MAPS", ("0-0",))
    built = kbm.build(app)
    assert built.problems == []
    out = tmp_path / "kb"
    kbm.write(built, out)
    loaded = kbm.load_kb(out)
    assert kbm.check_kb(loaded, kbm.load_relations(out)) == []
    loaded["_notes"] = kbm.report_lines(built)
    return loaded


def test_every_native_the_stage_reads_is_in_the_fixtures():
    read = {
        *quests.REQUIRE,
        *quests.REWARD,
        *quests.EVENT_NATIVES,
        *quests.MESSAGE_NATIVES,
        *quests.GETTERS,
        *quests.BOX_NATIVES,
        "setQuest",
        "startQuest",
        "getReward",
        "makeEquipItem",
        "removeQuest",
        "setClassUp",
        "changeMap",
        "showMarket",
        "setNPC",
        "getItem",
        "setNpcNotify",
        "getQuestState",
        "checkRequire",
        "getEvtIndex",
    }
    called = set()
    for data in SCRIPTS.values():
        c = so.decode(data)
        for f in (c.main, *c.subfunctions):
            called |= {i.names[0] for i in c.instructions(f) if i.name == "PUSHGLOBAL"}
    assert read - called <= {"getHeroClass", "getClassUpCount"}  # covered by the walker tests


def test_quests_givers_requirements_and_rewards(kb: dict):
    q = kb["quest"]
    assert sorted(q) == ["0-main-1", "1-sub-1", "1-sub-2"]
    main = q["0-main-1"]
    assert main["name"] == {"en": "EN T_Quest.main1.1", "ko": "KO T_Quest.main1.1"}
    assert main["description"]["en"] == "EN T_Quest.main1.2"
    assert main["requirements"] == [{"type": "mob", "monster": "2", "count": 5}]
    assert main["rewards"] == [
        {"type": "exp", "amount": 81},
        {"type": "meso", "amount": 550},
        {"type": "item", "item": "etc:1", "count": 3},
    ]
    assert [(r["id"], r["map"]) for r in main["given_by"]] == [("0", "0-0")]
    assert [(r["id"], r["map"]) for r in main["completed_by"]] == [("0", "0-0")]
    assert main["needs_kills"] == [{"kind": "monster", "id": "2", "count": 5}]
    assert {"kind": "quest", "id": "0-main-1", "count": 3} in kb["item"]["etc:1"][
        "rewarded_by_quests"
    ]
    assert main["started"] and main["reachable"]
    assert main["prerequisites"] == [[]]  # its own state is left out
    assert main["scripts"] == ["common-Lee"]


def test_chapter_override_and_quest_chains(kb: dict):
    q = kb["quest"]
    # chapter 1 runs chapter1/Lee, not common/Lee: no 1-main-1
    assert q["1-sub-1"]["scripts"] == ["chapter1-Lee"]
    assert q["1-sub-2"]["requirements"] == [{"type": "meso", "amount": 100}]
    assert [c["text"] for c in q["1-sub-2"]["prerequisites"][0]] == ["sub quest 1 is done"]
    assert q["1-sub-2"]["requires"] == [{"kind": "quest", "id": "1-sub-1", "op": "==", "value": 2}]
    assert q["1-sub-1"]["unlocks"] == [{"kind": "quest", "id": "1-sub-2", "op": "==", "value": 2}]
    s = kb["script"]
    assert s["common-Lee"]["chapters"] == [0]
    assert s["chapter1-Lee"]["chapters"] == [1]
    assert s["common-Lee2"]["chapters"] == [0, 1]


def test_recipes_trades_advancements_and_shops(kb: dict):
    (rid,) = kb["recipe"]
    r = kb["recipe"][rid]
    assert rid == "1-equip:0"
    assert (r["meso"], r["result_item"], r["quest"]) == (300, "equip:0", [1, 77])
    assert r["material_items"] == [{"type": "item", "item": "etc:1", "count": 4}]
    assert [(x["id"], x["map"]) for x in r["crafted_by"]] == [("1", "0-6")]
    assert sorted(kb["trade"]) == ["1-etc:0-etc:1"]
    assert kb["trade"]["1-etc:0-etc:1"]["result_items"] == [
        {"type": "item", "item": "etc:1", "count": 2}
    ]
    (jid,) = kb["job_advancement"]
    j = kb["job_advancement"][jid]
    assert jid == "1-1-1"
    assert [[c["text"] for c in alt] for alt in j["conditions"]] == [["level >= 10"]]
    assert [x["id"] for x in j["given_by"]] == ["1"]
    assert [(x["id"], x["map"]) for x in kb["npc"]["0"]["opens_shop"]] == [("0", "0-0")]
    assert [(x["id"], x["map"]) for x in kb["npc"]["1"]["opens_shop"]] == [("3", "0-6")]
    assert [x["id"] for x in kb["shop"]["3"]["opened_by"]] == ["1"]
    # Lee2 runs in chapters 0 and 1: one link, both chapters
    assert kb["shop"]["0"]["opened_by"] == [
        {"kind": "npc", "id": "0", "map": "0-0", "script": "common-Lee2", "chapters": [0, 1]}
    ]
    assert kb["shop"]["0"]["unopened_reason"] is None
    # the recipe and the trade are found through who offers them
    assert [x["id"] for x in kb["npc"]["1"]["crafts"]] == ["1-equip:0"]
    assert [x["id"] for x in kb["npc"]["1"]["trades"]] == ["1-etc:0-etc:1"]


def test_warps_events_and_map_exits(kb: dict):
    w = kb["warp"]
    end = w["common-TestMap-1"]
    assert (end["target_map"], end["chapter_end"], end["chapters"], end["used"]) == (
        "0-0",
        True,
        [0],
        True,
    )
    gated = w["chapter1-portal100-1"]
    assert (gated["target_map"], gated["conditional"], gated["chapters"]) == ("0-6", True, [1])
    assert [[c["text"] for c in alt] for alt in gated["conditions"]] == [["main quest 1 is done"]]
    exits = kb["map"]["0-1"]["exits"]
    assert {
        "kind": "map",
        "id": "0-0",
        "portal": None,
        "target_portal": 0,
        "script": "common/TestMap",
        "gated": True,
    } in exits
    e = kb["event"]
    assert sorted(e) == [
        "common-Lee2-boss",
        "common-TestMap-chapter_end",
        "common-TestMap-scene-5",
        "common-portal999-carnival-1",
    ]
    assert e["common-Lee2-boss"]["used"] and not e["common-portal999-carnival-1"]["used"]
    assert [m["id"] for m in e["common-TestMap-scene-5"]["maps"]] == ["0-1"]


def test_unused_scripts_say_why(kb: dict):
    s = kb["script"]
    assert s["common-portal999"]["used"] is False
    assert s["common-portal999"]["unused_reason"] == "no map object names it"
    assert s["common-Guard"]["unused_reason"] == (
        "in chapter 1, where a map that names it is reachable, the game runs chapter1/Guard instead"
    )
    assert s["chapter1-Guard"]["used"] and s["chapter1-Guard"]["roles"] == ["npc"]
    assert "script common-portal999 is unused: no map object names it" in kb["_notes"]


def test_dialogue_text_in_both_languages(kb: dict):
    d = kb["dialogue"]["common-Lee"]
    assert d["text"]["T_Lee.dialog.1"] == {"en": "EN T_Lee.dialog.1", "ko": "KO T_Lee.dialog.1"}
    assert d["missing_text"] == [NO_KOREAN]
    assert [x["id"] for x in d["npcs"]] == ["0"]
    assert f"dialogue common-Lee: key {NO_KOREAN} lacks a language" in kb["_notes"]
    root = d["nodes"][d["tree"]]
    offer = [p for p in root["paths"] if p["next"]]
    assert len(offer) == 1
    after = d["nodes"][offer[0]["next"]]
    assert (after["function"], after["answers"]) == ("onOffer", [0, 1])
    # the NPC-state pass: the quest icon for each state
    assert sorted(row["notify"] for row in d["state"]) == [0, 1]
    # the map script speaks as the NPC it names (setNPC)
    assert [x["id"] for x in kb["dialogue"]["common-TestMap"]["npcs"]] == ["0"]
