"""The bot's world tables (#297, mtport.botworld) from a synthetic knowledge base."""

import json
import os
import time

from mtport import botworld as bw


def warp(ident, chapters, source, portal, target, conditions=((),), role="portal", used=True):
    return {
        "id": ident,
        "used": used,
        "chapters": list(chapters),
        "from": [{"map": source, "role": role, "portal": portal, "npc": None}],
        "target_map": target,
        "conditions": [list(alt) for alt in conditions],
    }


def quest_cond(kind, number, op, value):
    return {
        "native": "getQuestState",
        "args": [kind, number],
        "quest": [kind, number],
        "op": op,
        "value": value,
        "term": f"getQuestState({kind}, {number})",
    }


def other_cond(kind, number):
    return {"native": "checkRequire", "args": [kind, number], "op": "==", "value": 0}


def entities():
    warps = [
        warp("common-portal000-1", [1, 2], "0-0", 0, "0-1"),
        warp("common-portal010-1", [1, 2], "0-1", 0, "0-0"),
        warp("chapter1-portal201-1", [1], "2-0", 1, "2-2", ([quest_cond(0, 9, "==", 2)],)),
        warp(
            "chapter2-portal942-1",
            [2],
            "9-4",
            2,
            "9-5",
            ([other_cond(0, 8), quest_cond(0, 8, "==", 1)],),
        ),
        warp(
            "common-portal280-1",
            [0],
            "2-8",
            1,
            "2-9",
            ([quest_cond(0, 4, "==", 1)], [other_cond(0, 5), quest_cond(0, 4, "!=", 1)]),
        ),
        warp("chapter1-Pirate-1", [1], "2-0", None, "4-1", role="npc"),
        warp("chapter1-unused-1", [1], "5-0", 0, "5-1", used=False),
        warp("chapter1-never-1", [1], "5-1", 0, "5-0", conditions=()),
    ]
    shops = [
        {
            "id": "3",
            "opened_by": [{"id": "2", "map": "2-0", "script": "common-Etc20", "chapters": [1]}],
            "sells": [{"id": "etc:3", "price": 15}, {"id": "etc:6", "price": 30}],
        },
        {
            "id": "4",
            "opened_by": [{"id": "1", "map": "3-0", "script": "common-Equip30", "chapters": [1]}],
            "sells": [{"id": "equip:53", "price": 3500}],
        },
        {"id": "0", "opened_by": [], "sells": [{"id": "equip:0", "price": 170}]},
    ]
    scripts = {
        "common-Etc20": {"id": "common-Etc20", "base_name": "Etc20"},
        "common-Equip30": {"id": "common-Equip30", "base_name": "Equip30"},
    }
    return {
        "warp": {w["id"]: w for w in warps},
        "shop": {s["id"]: s for s in shops},
        "script": scripts,
    }


def test_links_per_chapter_with_gates():
    links = bw.links(entities())
    lines = [lk.line() for lk in links]
    assert lines == [
        "link 0 2 8 1 2 9 q0.4==1 ?&q0.4!=1",
        "link 1 0 0 0 0 1",
        "link 1 0 1 0 0 0",
        "link 1 2 0 1 2 2 q0.9==2",
        "link 1 5 1 0 5 0 ?",
        "link 2 0 0 0 0 1",
        "link 2 0 1 0 0 0",
        "link 2 9 4 2 9 5 ?&q0.8==1",
    ]


def test_shops_per_item():
    assert [s.line() for s in bw.shops(entities())] == [
        "shop 1 2 0 Etc20 3 1 3 15",
        "shop 1 2 0 Etc20 3 1 6 30",
        "shop 1 3 0 Equip30 4 0 53 3500",
    ]


def test_render_parse_round_trip():
    ents = entities()
    text = bw.generate(ents)
    assert text.startswith("#")
    links, shops = bw.parse(text)
    assert links == bw.links(ents)
    assert shops == bw.shops(ents)
    assert bw.generate(ents) == text  # deterministic


def test_gate_opens_with_quest_states():
    by = {(lk.chapter, lk.area, lk.map): lk for lk in bw.links(entities())}
    perion = by[(1, 2, 0)]
    assert not bw.open_now(perion, {})
    assert not bw.open_now(perion, {(0, 9): 1})
    assert bw.open_now(perion, {(0, 9): 2})
    assert bw.open_now(by[(1, 0, 0)], {})
    # An alternative with a term the bot can't test is shut; the other one may hold.
    tutorial = by[(0, 2, 8)]
    assert bw.open_now(tutorial, {(0, 4): 1})
    assert not bw.open_now(tutorial, {(0, 4): 2})
    assert not bw.open_now(by[(2, 9, 4)], {(0, 8): 1})
    assert not bw.open_now(by[(1, 5, 1)], {})


def test_every_operator_parses():
    for op in bw.OPS:
        t = bw.Term((1, 25), op, 1)
        assert bw._parse_term(t.text()) == t


def test_parse_rejects_bad_lines():
    for bad in ("link 1 2", "shop 1 2 0 Etc20 3 1 3", "warp 1 2 3", "link 1 0 0 0 0 1 q0.9~2"):
        try:
            bw.parse(bad + "\n")
        except bw.BotWorldError as e:
            assert "line 1" in str(e)
        else:
            raise AssertionError(f"accepted {bad!r}")


def test_no_portal_warps_is_an_error():
    try:
        bw.generate({"warp": {}})
    except bw.BotWorldError:
        pass
    else:
        raise AssertionError("generated tables without links")


def write_kb(path, ents):
    path.mkdir()
    index = {"schema": 1, "kinds": {k: len(v) for k, v in ents.items()}, "relations": []}
    (path / "index.json").write_text(json.dumps(index))
    for kind, table in ents.items():
        doc = {"schema": 1, "kind": kind, "entities": list(table.values())}
        (path / f"{kind}.json").write_text(json.dumps(doc))


def test_cli_from_a_kb(tmp_path, capsys):
    kb = tmp_path / "kb"
    write_kb(kb, entities())
    out = tmp_path / "out" / "bot_world.txt"
    assert bw.main(["--kb", str(kb), "--out", str(out)]) == 0
    assert out.read_text() == bw.generate(entities())
    assert "8 links, 3 shop items" in capsys.readouterr().out


def test_cli_skips_without_the_bundle(tmp_path):
    assert bw.main(["--app", str(tmp_path / "none"), "--out", str(tmp_path / "o")]) == 77
    assert not (tmp_path / "o").exists()


def test_stale_follows_the_inputs(tmp_path):
    app = tmp_path / "app"
    (app / "script").mkdir(parents=True)
    (app / "map").mkdir()
    script = app / "script" / "a.out"
    script.write_bytes(b"x")
    out = tmp_path / "bot_world.txt"
    assert bw.stale(out, app)
    out.write_text("x")
    future = time.time() + 3600
    os.utime(out, (future, future))
    assert not bw.stale(out, app)
    os.utime(script, (future + 1, future + 1))
    assert bw.stale(out, app)
