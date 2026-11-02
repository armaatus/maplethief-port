"""tools/mtport/doctestctest.py: one CTest per doctest case, whatever its name (#171)."""

from __future__ import annotations

import json
import re
import shutil
import subprocess
from pathlib import Path

import pytest

from mtport import doctestctest as dc
from repo_paths import REPO

# Every character the old CMake-list discovery cut or mangled, and doctest's own specials.
AWKWARD = [
    "plain name",
    "semicolon; then more",
    "brackets [x] and an open [ alone",
    "a bracket close ]] and ]==] and ]=]",
    "comma, and backslash \\ and \\, together",
    'quote " dollar ${VAR} hash # paren ) (',
    "non-ASCII: ⅓ speed é",
]


def listing(names: list[str]) -> str:
    """doctest 2.5's ``--list-test-cases --no-colors`` output."""
    rule = "=" * 79
    return "\n".join(
        [
            "[doctest] listing all test case names",
            rule,
            *names,
            rule,
            f"[doctest] unskipped test cases passing the current filters: {len(names)}",
            "",
        ]
    )


def test_parse_list_reads_the_names_between_the_rules() -> None:
    names = [*AWKWARD, "[doctest] a name that looks like a header"]
    assert dc.parse_list(listing(names)) == names
    assert dc.parse_list(listing(names).replace("\n", "\r\n")) == names  # Windows


def test_parse_list_checks_the_count() -> None:
    with pytest.raises(dc.DiscoveryError):
        dc.parse_list(listing(["a", "b"]).replace("filters: 2", "filters: 3"))
    with pytest.raises(dc.DiscoveryError):
        dc.parse_list("no rules here")


@pytest.mark.parametrize("name", AWKWARD)
def test_each_filter_selects_exactly_its_case(name: str) -> None:
    assert dc.matches(AWKWARD, dc.case_filter(name)) == [name]


def test_filters_escape_like_doctest() -> None:
    assert dc.case_filter("a,b\\c") == "a\\,b\\\\c"
    assert dc.unescape_filter("a\\,b\\\\c") == ["a,b\\c"]
    assert dc.unescape_filter("a,b") == ["a", "b"]
    assert dc.unescape_filter("a\\x\\") == ["a\\x\\"]
    assert dc.case_filter("x ⅓é y") == "x * y"


def test_wildcmp_is_doctests() -> None:
    assert dc.wildcmp("Hello World", "hello world")
    assert dc.wildcmp("hello world", "h*d")
    assert dc.wildcmp("hello", "h?llo")
    assert not dc.wildcmp("hello world", "hello")
    assert dc.wildcmp("a.b", "a.b") and not dc.wildcmp("axb", "a.b")


def test_ambiguous_names_fail_the_build() -> None:
    with pytest.raises(dc.DiscoveryError, match="selects"):
        dc.check_unique(["a * b", "a long b"])
    with pytest.raises(dc.DiscoveryError, match="selects"):
        dc.check_unique(["Case", "case"])
    with pytest.raises(dc.DiscoveryError, match="duplicate"):
        dc.check_unique(["same", "same"])
    dc.check_unique(AWKWARD)


def test_bracket_never_closes_early() -> None:
    assert dc.bracket("a;b") == "[[a;b]]"
    assert dc.bracket("x]]y") == "[=[x]]y]=]"
    assert dc.bracket("x]]y]=]z") == "[==[x]]y]=]z]==]"


def test_compare_finds_missing_and_stray_tests() -> None:
    names = ["one; two", "three"]
    good = [(f"area.{n}", dc.case_filter(n)) for n in names]
    assert dc.compare("t", names, good) == []
    cut = [("area.one", "one"), ("area.three", "three")]  # the old discovery's cut name
    problems = dc.compare("t", names, cut)
    assert any("selects 0 cases" in p for p in problems)
    assert any("'one; two' has 0 CTests" in p for p in problems)
    twice = [*good, good[1]]
    assert any("has 2 CTests" in p for p in dc.compare("t", names, twice))


@pytest.mark.skipif(shutil.which("ctest") is None, reason="needs CMake's ctest")
def test_ctest_reads_every_name_and_filter_back(tmp_path: Path) -> None:
    """The written script, included by a CTestTestfile, gives CTest the names and arguments
    unchanged, and ``check``'s reading of ``--show-only=json-v1`` finds them."""
    fake = tmp_path / "fake_test"
    fake.write_text("#!/bin/sh\n", encoding="utf-8")
    fake.chmod(0o755)
    exe = str(fake)
    script = dc.render(AWKWARD, exe, "area.", str(tmp_path), "_props")
    (tmp_path / "tests.cmake").write_text(script, encoding="utf-8")
    (tmp_path / "CTestTestfile.cmake").write_text(
        "function(_props test)\n"
        '  set_tests_properties("${test}" PROPERTIES LABELS [==[cpp]==] TIMEOUT [==[60]==])\n'
        "endfunction()\n"
        f"include({dc.bracket((tmp_path / 'tests.cmake').as_posix())})\n",
        encoding="utf-8",
    )
    r = subprocess.run(["ctest", "--show-only=json-v1"], cwd=tmp_path, capture_output=True)
    assert r.returncode == 0, r.stderr.decode()
    shown = json.loads(r.stdout)
    assert [t["name"] for t in shown["tests"]] == [f"area.{n}" for n in AWKWARD]
    for t in shown["tests"]:
        props = {p["name"]: p["value"] for p in t["properties"]}
        assert props["LABELS"] == ["cpp"]
        assert props["ENVIRONMENT_MODIFICATION"] == [f"{dc.EXPECT_ENV}=set:1"]
    groups = dc.doctest_tests(shown)
    assert dc.compare(exe, AWKWARD, groups[exe]) == []


_CASE = re.compile(r'\bTEST_CASE\(\s*((?:"(?:[^"\\]|\\.)*"\s*)+)')
_LITERAL = re.compile(r'"((?:[^"\\]|\\.)*)"')


def source_case_names(path: Path) -> list[str]:
    """The TEST_CASE names in one C++ file (adjacent literals joined, simple escapes)."""
    names = []
    for m in _CASE.finditer(path.read_text(encoding="utf-8")):
        raw = "".join(_LITERAL.findall(m.group(1)))
        names.append(re.sub(r"\\(.)", r"\1", raw))
    return names


def test_every_source_case_name_is_selectable_alone() -> None:
    """Without a build: no TEST_CASE name in tests/cpp is a duplicate or matches another's
    filter (doctest's * and ? can't be escaped; names compare case-insensitively)."""
    names = [n for p in sorted((REPO / "tests/cpp").glob("*.cpp")) for n in source_case_names(p)]
    assert len(names) > 500
    assert any(";" in n for n in names)
    dc.check_unique(names)


def test_doctests_own_discovery_is_not_used() -> None:
    """doctest_discover_tests cut names at ';' and '[' (#171); maplethief_doctest_discover
    replaces it."""
    for p in REPO.glob("**/CMakeLists.txt"):
        if "build" in p.relative_to(REPO).parts or "work" in p.relative_to(REPO).parts:
            continue
        for line in p.read_text(encoding="utf-8").splitlines():
            code = line.split("#", 1)[0]
            assert "doctest_discover_tests(" not in code, f"{p}: use maplethief_doctest_discover"
