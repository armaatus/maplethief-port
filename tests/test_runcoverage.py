"""The run coverage's merge and report tool (mtport.runcoverage, #295): the coverage file's
parser and writer, merging runs, the report over a synthetic universe (tags, scripts, maps,
natives, imports), the comparison with a baseline, the choice and order of the CTest tests
with their fixtures, and a run against a stand-in for CTest. No game file is needed."""

from __future__ import annotations

import json
import subprocess
from pathlib import Path

import pytest

from mtport import runcoverage as rc

RUN_A = """\
# maplethief coverage 1: 3 guest functions entered (pointer, entries, name)
0x0000280D 1 main
0x000225E5 3 f_000225E5
0x0009B37D 7 Lua::callNative
hit import 12 _memcpy
hit import 1 _objc_msgSend
hit selector 4 -[EAGLView drawView]
hit script 1 script/common/portal200.out
hit native 2 38 changeMap
hit native 5 15 regMsgNPC
hit map 1 2.8
"""

RUN_B = """\
# maplethief coverage 1: 2 guest functions entered (pointer, entries, name)
0x000225E5 2 CGame::Update
0x00002001 1 f_00002001
hit import 3 _memcpy
hit map 2 2.9
hit map 1 10.3
"""


def write(path: Path, text: str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return path


def test_parse_reads_functions_hits_and_runs():
    cov = rc.parse(RUN_A, "a")
    assert cov.functions == {0x280D: 1, 0x225E5: 3, 0x9B37D: 7}
    assert cov.names[0x9B37D] == "Lua::callNative"
    assert cov.hits["import"] == {"_memcpy": 12, "_objc_msgSend": 1}
    assert cov.hits["selector"] == {"-[EAGLView drawView]": 4}
    assert cov.hits["native"] == {"38 changeMap": 2, "15 regMsgNPC": 5}
    assert cov.runs == {"a": 3}


@pytest.mark.parametrize(
    ("text", "message"),
    [
        ("", "not a maplethief coverage file"),
        ("0x00001000 1 f\n", "not a maplethief coverage file"),
        ("# maplethief coverage 1: 1 guest functions entered\nbad line\n", "line 2"),
        ("# maplethief coverage 1: 1 guest functions entered\nhit frob 1 x\n", "unknown hit kind"),
        ("# maplethief coverage 1: 2 guest functions entered\n0x00001001 1 f\n", "header says 2"),
    ],
)
def test_parse_refuses_malformed_files(text, message):
    with pytest.raises(ValueError, match=message):
        rc.parse(text)


def test_merge_sums_entries_and_hits_and_keeps_the_best_name():
    total = rc.Coverage()
    total.add(rc.parse(RUN_A, "a"))
    total.add(rc.parse(RUN_B, "b"))
    assert total.functions[0x225E5] == 5
    assert total.names[0x225E5] == "CGame::Update"  # a symbol beats f_XXXXXXXX
    assert total.hits["import"]["_memcpy"] == 15
    assert total.hits["map"] == {"2.8": 1, "2.9": 2, "10.3": 1}
    assert total.runs == {"a": 3, "b": 2}


def test_render_round_trips_through_parse():
    total = rc.Coverage()
    total.add(rc.parse(RUN_A, "a"))
    total.add(rc.parse(RUN_B, "b"))
    text = rc.render(total)
    assert text.splitlines()[0].startswith("# maplethief coverage 1: 4 guest functions entered")
    assert "# run a 3" in text and "# run b 2" in text
    again = rc.parse(text)
    assert again.functions == total.functions
    assert again.hits == total.hits
    assert again.runs == total.runs


def test_load_names_runs_by_directory_and_reports_the_bad_file(tmp_path):
    runs = tmp_path / "runs"
    write(runs / "play.regress" / "coverage-11.txt", RUN_A)
    write(runs / "play.regress" / "coverage-12.txt", RUN_B)
    write(runs / "menus.title" / "coverage-13.txt", RUN_B)
    write(runs / "menus.title" / "notes.txt", "not coverage")
    cov = rc.load([runs])
    assert set(cov.runs) == {"play.regress", "menus.title"}
    assert cov.runs["play.regress"] == 3  # the largest file of the run
    assert cov.functions[0x225E5] == 3 + 2 + 2
    write(runs / "broken" / "coverage-1.txt", "# maplethief coverage 1: 5 guest functions\n")
    with pytest.raises(ValueError, match="coverage-1.txt"):
        rc.load([runs])


def test_merge_command_writes_a_merged_file(tmp_path, capsys):
    a = write(tmp_path / "a.txt", RUN_A)
    b = write(tmp_path / "b.txt", RUN_B)
    out = tmp_path / "out" / "merged.txt"
    assert rc.main(["merge", str(out), str(a), str(b)]) == 0
    merged = rc.parse(out.read_text(encoding="utf-8"))
    assert merged.runs == {"a": 3, "b": 2}
    assert "merged 2 runs: 4 functions" in capsys.readouterr().out
    assert rc.main(["merge", str(out), str(write(tmp_path / "bad.txt", "x\n"))]) == 1


@pytest.mark.parametrize(
    ("stem", "key"),
    [
        ("00", "0.0"),
        ("28", "2.8"),
        ("100", "10.0"),
        ("105", "10.5"),
        ("210", "2.10"),
        ("418", "4.18"),
        ("910", "9.10"),
        ("0", None),
        ("1000", None),
        ("x1", None),
    ],
)
def test_map_key_follows_the_bundle_names(stem, key):
    assert rc.map_key(stem) == key


def universe(tmp_path: Path) -> tuple[Path, Path, Path]:
    tags = write(
        tmp_path / "function-tags.json",
        json.dumps(
            {
                "functions": [
                    {"address": "0x280D", "tag": "app-objc", "name": "main"},
                    {"address": "0x2001", "tag": "engine", "name": "CGame::Update"},
                    {"address": "0x225E5", "tag": "engine", "name": "FUN_000225e4"},
                    {"address": "0x30001", "tag": "engine", "name": "CHero::Never"},
                    {"address": "0xC2D61", "tag": "lib:chartboost", "name": "-[CB a]"},
                    {"address": "0xC2E01", "tag": "lib:chartboost", "name": "-[CB b]"},
                ]
            }
        ),
    )
    app = tmp_path / "MapleThief.app"
    for rel in (
        "script/common/portal200.out",
        "script/chapter1/Manji.out",
        "map/28.map",
        "map/29.map",
        "map/103.map",
        "map/0.mapprj",
    ):
        write(app / rel, "")
    imports = write(
        tmp_path / "imports.json",
        json.dumps(
            {
                "imports": [
                    {"name": "_memcpy", "kind": "function", "class": "game"},
                    {"name": "_objc_msgSend", "kind": "function", "class": "both"},
                    {"name": "_free", "kind": "function", "class": "game"},
                    {"name": "_cbOnly", "kind": "function", "class": "chartboost"},
                    {"name": "_kData", "kind": "address", "class": "game"},
                ]
            }
        ),
    )
    return tags, app, imports


def test_universe_reads_tags_bundle_natives_and_imports(tmp_path):
    u = rc.load_universe(*universe(tmp_path))
    assert len(u.functions) == 6 and u.functions[0x225E5].tag == "engine"
    assert u.scripts == ["script/chapter1/Manji.out", "script/common/portal200.out"]
    assert u.maps == ["2.8", "2.9", "10.3"]
    assert len(u.natives) == 65 and u.natives[38] == "38 changeMap"
    assert u.imports == ["_free", "_memcpy", "_objc_msgSend"]
    empty = rc.load_universe(tmp_path / "none.json", tmp_path / "none", None)
    assert empty.functions == {} and empty.scripts == [] and empty.maps == []


def test_report_counts_game_code_without_chartboost(tmp_path):
    total = rc.Coverage()
    total.add(rc.parse(RUN_A, "a"))
    total.add(rc.parse(RUN_B, "b"))
    r = rc.build_report(total, rc.load_universe(*universe(tmp_path)))
    fn = r["functions"]
    assert (fn["game_executed"], fn["game_total"]) == (3, 4)
    assert (fn["executed"], fn["total"]) == (4, 6)
    assert fn["by_tag"]["engine"] == {"executed": 2, "total": 3}
    assert fn["by_tag"]["lib:chartboost"] == {"executed": 0, "total": 2}
    assert fn["untagged_executed"] == ["0x9B37D"]
    assert [f["address"] for f in r["unexecuted"]] == ["0x30001", "0xC2D61", "0xC2E01"]
    assert r["unexecuted"][0] == {"address": "0x30001", "tag": "engine", "name": "CHero::Never"}
    assert r["scripts"]["used_known"] == 1 and r["scripts"]["total"] == 2
    assert r["scripts"]["unused"] == ["script/chapter1/Manji.out"]
    assert list(r["natives"]["hits"]) == ["15 regMsgNPC", "38 changeMap"]
    assert r["natives"]["total"] == 65 and len(r["natives"]["unused"]) == 63
    assert list(r["maps"]["hits"]) == ["2.8", "2.9", "10.3"]
    assert r["maps"]["unused"] == []
    assert r["imports"]["unused"] == ["_free"]
    assert r["selectors"] == {"used": 1, "hits": {"-[EAGLView drawView]": 4}}


def test_report_names_unnamed_functions_from_the_tags(tmp_path):
    cov = rc.parse(RUN_A, "a")
    r = rc.build_report(cov, rc.load_universe(*universe(tmp_path)))
    md = rc.render_markdown(r)
    assert "**Game code: 2 of 4 functions executed (50.0%)**" in md
    assert "| `0x2001` | `engine` | `CGame::Update` |" in md  # unexecuted, named by the tags
    assert "-[CB a]" not in md  # Chartboost is only in the JSON
    assert "| `a` | 3 |" in md
    assert "Not used (1): `script/chapter1/Manji.out`." in md


def test_baseline_comparison_lists_what_is_new(tmp_path):
    u = rc.load_universe(*universe(tmp_path))
    old = rc.build_report(rc.parse(RUN_A, "a"), u)
    total = rc.parse(RUN_A, "a")
    total.add(rc.parse(RUN_B, "b"))
    new = rc.build_report(total, u, baseline=old)
    s = new["since_baseline"]
    assert s["game_executed"] == 1
    assert s["functions"] == ["0x2001"]
    assert s["maps"] == ["2.9", "10.3"]
    assert s["imports"] == [] and s["natives"] == []
    assert "## Since the baseline" in rc.render_markdown(new)


def test_report_command_writes_both_files(tmp_path, capsys):
    tags, app, imports = universe(tmp_path)
    a = write(tmp_path / "runs" / "x" / "coverage-1.txt", RUN_A)
    out = tmp_path / "report"
    args = [
        "report",
        str(a.parent.parent),
        "--out",
        str(out),
        "--tags",
        str(tags),
        "--app",
        str(app),
        "--imports",
        str(imports),
    ]
    assert rc.main(args) == 0
    r = json.loads((out / "run-coverage.json").read_text(encoding="utf-8"))
    assert r["runs"] == {"x": 3}
    assert (out / "run-coverage.md").read_text(encoding="utf-8").startswith("# Run coverage")
    assert "game code 2 of 4 functions (50.0%)" in capsys.readouterr().out
    assert rc.main([*args, "--baseline", str(out / "run-coverage.json")]) == 0
    r = json.loads((out / "run-coverage.json").read_text(encoding="utf-8"))
    assert r["since_baseline"]["game_executed"] == 0


CTEST = {
    "tests": [
        {"name": "runtime.unit", "properties": []},
        {
            "name": "play.save_quit",
            "properties": [{"name": "FIXTURES_SETUP", "value": ["play_save"]}],
        },
        {
            "name": "input.menu_x",
            "properties": [{"name": "FIXTURES_REQUIRED", "value": ["play_save"]}],
        },
        {
            "name": "play.chapter_1_a",
            "properties": [{"name": "FIXTURES_SETUP", "value": ["play_chapter_1_a"]}],
        },
        {
            "name": "play.chapter_1_b",
            "properties": [
                {"name": "FIXTURES_REQUIRED", "value": ["play_chapter_1_a"]},
                {"name": "FIXTURES_SETUP", "value": ["play_chapter_1_b"]},
            ],
        },
        {
            "name": "play.chapter_1_bx",
            "properties": [{"name": "FIXTURES_REQUIRED", "value": ["play_chapter_1_b"]}],
        },
        {"name": "menus.title", "properties": []},
        {"name": "play.regress", "properties": []},
    ]
}


def test_select_adds_fixture_setups_before_their_tests():
    tests = rc.ctest_tests(CTEST)
    names = [t.name for t in rc.select(tests, rc.BASELINE_TESTS)]
    assert names == [
        "play.save_quit",
        "input.menu_x",
        "play.chapter_1_a",
        "play.chapter_1_b",
        "menus.title",
        "play.regress",
    ]
    names = [t.name for t in rc.select(tests, [r"play\.chapter_1_bx"])]
    assert names == ["play.chapter_1_a", "play.chapter_1_b", "play.chapter_1_bx"]
    assert rc.select(tests, ["nothing"]) == []


def test_run_tests_gives_each_test_its_own_coverage_directory(tmp_path):
    calls = []

    def runner(cmd, env):
        calls.append((cmd, env["MAPLETHIEF_COVERAGE"]))
        Path(env["MAPLETHIEF_COVERAGE"].replace("%p", "7")).write_text(RUN_B, encoding="utf-8")
        return subprocess.CompletedProcess(cmd, 0 if "menus" in cmd[4] else 8)

    sym = write(tmp_path / "maplethief.sym", "# maplethief symbols 1\n")
    tests = rc.select(rc.ctest_tests(CTEST), [r"menus\..*", r"input\..*"])
    stale = write(tmp_path / "runs" / "menus.title" / "coverage-1.txt", "old")
    status = rc.run_tests(tmp_path / "build", tests, tmp_path / "runs", sym, runner=runner)
    assert status == {"play.save_quit": 8, "input.menu_x": 8, "menus.title": 0}
    assert not stale.exists()
    cmd, path = calls[2]
    assert cmd[cmd.index("-R") + 1] == r"^menus\.title$" and "-FA" in cmd
    assert Path(path).parts[-3:] == ("runs", "menus.title", "coverage-%p.txt")
    cov = rc.load([tmp_path / "runs"])
    assert set(cov.runs) == {"play.save_quit", "input.menu_x", "menus.title"}
