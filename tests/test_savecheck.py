"""The save checks in Python (tools/mtport/savecheck.py, #148, ADR 0078), and that they and the
C checks (mt_saves_check, runtime/src/foundation/saves.cpp) use the same limits, codes and
order. The two run on the same fixtures through the program in CTest saves.check_parity."""

import re
import zipfile

import pytest

from mtport import savecheck as sc
from mtport import savedata as sd
from repo_paths import REPO

HEADER = REPO / "runtime" / "include" / "maplethief" / "saves.h"
SOURCE = REPO / "runtime" / "src" / "foundation" / "saves.cpp"
SPEC = REPO / "docs" / "formats" / "save.md"


@pytest.mark.parametrize("fx", sc.fixtures(), ids=lambda f: f.name)
def test_each_fixture_gets_the_verdict_it_was_built_for(fx: sc.Fixture) -> None:
    verdicts = sc.check_set(fx.files)
    assert sc.set_verdict(verdicts) == fx.expect
    broken = {name for name, v in verdicts.items() if not v.ok}
    assert broken == ({fx.file} if fx.expect != "ok" else set())
    for v in verdicts.values():
        assert v.code in sc.VERDICTS + sc.SET_VERDICTS
        assert v.ok or v.why


def test_the_fixtures_are_the_same_every_time() -> None:
    a, b = sc.fixtures(), sc.fixtures()
    assert [(f.name, f.files) for f in a] == [(f.name, f.files) for f in b]
    assert len({f.name for f in a}) == len(a)
    # Every rule has a fixture that breaks it.
    assert {f.expect for f in a} == {"ok", *sc.VERDICTS[2:], *sc.SET_VERDICTS}


def test_the_valid_sets_parse_and_round_trip_through_savedata() -> None:
    for files in (sc.full_set(), sc.new_character_set()):
        for name, data in files.items():
            if name.startswith(sc.COUNTERS_PREFIX):
                continue  # the port's text file, not one savedata.py parses
            assert sd.encode_file(name, sd.parse_file(name, data)) == data


def test_the_c_checks_use_the_same_limits() -> None:
    header = HEADER.read_text(encoding="utf-8")

    def define(name: str) -> int:
        m = re.search(rf"#define MT_SAVES_{name} \(?(\d+)u(?: \* (\d+)u\))?", header)
        assert m is not None, name
        return int(m.group(1)) * (int(m.group(2)) if m.group(2) else 1)

    assert define("EQUIP_IDS") == sc.EQUIP_IDS
    assert define("ETC_IDS") == sc.ETC_IDS
    assert define("MAX_LEVEL") == sc.MAX_LEVEL
    assert define("MAX_BRANCH") == sc.MAX_BRANCH
    assert define("MAX_JOB_TIER") == sc.MAX_JOB_TIER
    assert define("MAX_SKILL_LEVEL") == sc.MAX_SKILL_LEVEL
    assert define("AREAS") == len(sc.MAPS_IN_AREA)
    assert define("MAX_RECEIPTS_BYTES") == sc.MAX_RECEIPTS_BYTES
    assert define("MAX_COUNTERS_BYTES") == sc.MAX_COUNTERS_BYTES
    assert f'#define MT_SAVES_COUNTERS_PREFIX "{sc.COUNTERS_PREFIX}"' in header
    source = SOURCE.read_text(encoding="utf-8")
    m = re.search(r"k_maps_in_area\[MT_SAVES_AREAS\] = \{([\d, ]+)\};", source)
    assert m is not None
    assert tuple(int(n) for n in m.group(1).split(",")) == sc.MAPS_IN_AREA


def test_the_c_checks_use_the_same_codes() -> None:
    header = HEADER.read_text(encoding="utf-8")
    enum = re.search(r"typedef enum mt_saves_verdict \{(.*?)\} mt_saves_verdict;", header, re.S)
    assert enum is not None
    values = dict(re.findall(r"MT_SAVES_(OK|BAD_\w+) = (\d+)", enum.group(1)))
    codes = {name.lower(): int(v) for name, v in values.items()}
    assert sorted(codes, key=codes.get) == list(sc.VERDICTS)
    source = SOURCE.read_text(encoding="utf-8")
    names = re.findall(r"case MT_SAVES_(OK|BAD_\w+):\s+return \"(\w+)\";", source)
    assert {k.lower(): v for k, v in names} == {v: v for v in sc.VERDICTS}


def test_the_spec_lists_every_rule() -> None:
    spec = SPEC.read_text(encoding="utf-8")
    section = spec.split("## Checking a save", 1)[1].split("\n## ", 1)[0]
    for code in sc.VERDICTS[1:] + sc.SET_VERDICTS:
        assert f"`{code}`" in section, code


def test_pairs_and_reading_a_folder(tmp_path) -> None:
    files = sc.new_character_set()
    docs = tmp_path / "copy" / "Documents"
    docs.mkdir(parents=True)
    for name, data in files.items():
        (docs / name).write_bytes(data)
    (docs / "theif0.mt-tmp").write_bytes(b"half")
    (docs / "notes.txt").write_bytes(b"x")
    got, ignored = sc.read_set(tmp_path / "copy")
    assert got == files
    assert ignored == ["notes.txt"]
    del files["quest0"]
    v = sc.check_set(files)
    assert v["set"] == sc.Verdict("bad_pair", "theif0 comes without quest0")


def test_the_command_line(tmp_path, capsys) -> None:
    sc.write_fixtures(tmp_path)
    assert (tmp_path / "full.zip").is_file()
    with zipfile.ZipFile(tmp_path / "map.zip") as z:
        assert "Documents/theif0" in z.namelist()
    assert sc.main(["check", str(tmp_path / "full")]) == 0
    assert sc.main(["check", str(tmp_path / "config_speed_3")]) == 1
    out = capsys.readouterr().out
    assert "config: bad_config: config: game speed is 3" in out
    assert "theif0: ok" in out
