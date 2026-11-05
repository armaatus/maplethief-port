"""Text packs (#189, ADR 0108, tools/mtport/textpack.py): the .strings parser with the
runtime's rules and reasons, check, and export from a synthetic bundle; the real bundle's
export runs locally only (it needs work/ipa) and writes to a temporary folder."""

from __future__ import annotations

import dataclasses
import plistlib
from pathlib import Path

import pytest

from mtport import datatables, textpack
from mtport.textpack import StringsError, parse_strings

ROOT = Path(__file__).resolve().parents[1]


def test_parse_comments_escapes_bom_and_duplicates() -> None:
    data = (
        "﻿/* a comment\n over two lines */\n"
        '"a" = "x";  // to the end of the line\n'
        '"b"="q\\"uote\\\\ \\n\\t\\r \\\'";\n'
        '"c" = "\\U00e9\\u00F6 \\UD83D\\UDE00";\n'
        '"a" = "again";\n'
        '"multi" = "one\ntwo";\n'
    ).encode()
    f = parse_strings(data)
    assert f.entries == {
        "a": "again",
        "b": "q\"uote\\ \n\t\r '",
        "c": "éö \U0001f600",
        "multi": "one\ntwo",
    }
    assert f.duplicates == 1
    assert parse_strings(b"").entries == {}
    assert parse_strings('"Pötion" = "Hénésys";'.encode()).entries == {"Pötion": "Hénésys"}


@pytest.mark.parametrize(
    ("text", "reason"),
    [
        ("a = 1;", "line 1: expected a quoted key"),
        ('"a" "b";', "line 1: expected '=' after the key"),
        ('"a" = b;', "line 1: expected a quoted value after '='"),
        ('"a" = "b"\n"c" = "d";', "line 2: expected ';' after the value"),
        ('\n"a" = "b;\n', "line 2: a string without its closing quote"),
        ('/* open\n"a" = "b";', "line 1: a comment without its end (*/)"),
        ('"a" = "\\q";', "line 1: an unknown escape \\q"),
        ('"a" = "\\U12";', "line 1: \\U needs four hex digits"),
        ('"a" = "\\UD800";', "line 1: a lone surrogate (\\UD800)"),
        ('"a" = "\\UDC00";', "line 1: a lone surrogate (\\UDC00)"),
        ('"a" = "\\U0000";', "line 1: a NUL character"),
        ('"a" = "x\0";', "line 1: a NUL character"),
        ('"" = "b";', "line 1: an empty key"),
        ('"' + "k" * 257 + '" = "b";', "line 1: a key longer than 256 bytes"),
        ('"a" = "' + "v" * 4097 + '";', "line 1: a value longer than 4096 bytes"),
        ('"a" =\n"b"\n;\n"" = \n"x";', "line 5: an empty key"),  # the value's line
    ],
)
def test_refused_with_the_runtime_reason(text: str, reason: str) -> None:
    with pytest.raises(StringsError) as e:
        parse_strings(text.encode())
    assert str(e.value) == reason


def test_invalid_utf8_names_the_byte_and_offset() -> None:
    with pytest.raises(StringsError) as e:
        parse_strings(b'"a" = "ok";\n"b" = "P\xf6tion";')
    assert str(e.value) == "line 2: invalid UTF-8 (byte 0xF6 at offset 20)"
    with pytest.raises(StringsError, match="invalid UTF-8"):
        parse_strings(b'"a" = "\xed\xa0\x80";')  # an encoded surrogate
    with pytest.raises(StringsError, match="larger than 4 MiB"):
        parse_strings(b" " * (textpack.MAX_FILE + 1))


def test_entry_limit() -> None:
    many = "".join(f'"k{i}" = "v";\n' for i in range(textpack.MAX_ENTRIES)).encode()
    assert len(parse_strings(many).entries) == textpack.MAX_ENTRIES
    with pytest.raises(StringsError, match=r"line 65537: more than 65536 entries"):
        parse_strings(many + b'"one more" = "v";')


def test_quote_reads_back() -> None:
    for s in ["", 'q"\\', "a\nb\tc\rd", "\x01\x7f", "Pötion \U0001f600", "/* // */"]:
        assert parse_strings(f'"k" = {textpack.quote(s)};'.encode()).entries == {"k": s}


def test_utf8_cut_and_places() -> None:
    assert textpack.utf8_cut("Pötion", 2) == "P"
    assert textpack.utf8_cut("Pötion", 3) == "Pö"
    assert textpack.utf8_cut("abc", 63) == "abc"
    assert textpack.places("{0}: {1} and {0}") == {0, 1}


def _manifest(d: Path, extra: str = "") -> None:
    (d / "textpack.strings").write_text(
        '"format" = "1";\n"name" = "Test";\n"language" = "de";\n' + extra, encoding="utf-8"
    )


def test_check_refuses_ignores_and_cuts(tmp_path: Path) -> None:
    assert not textpack.check(tmp_path / "none").ok
    assert "has no textpack.strings" in textpack.check(tmp_path).refused[0]
    _manifest(tmp_path)
    (tmp_path / "TABLES.strings").write_text(
        '"item_etc.3.name" = "' + "ö" * 40 + '";\n'
        '"item_etc.3.price" = "1";\n'
        '"mob.x.name" = "a";\n'
        '"npc.0.name" = "Hénésys";\n',
        encoding="utf-8",
    )
    (tmp_path / "Port.strings").write_text(
        '"menu_title" = "EINSTELLUNGEN";\n"menu_not_in_build" = "fehlt";\n"nope" = "x";\n',
        encoding="utf-8",
    )
    (tmp_path / "Scripts.strings").write_bytes(b'"a" = "\xff";')
    (tmp_path / "Other.strings").write_text("", encoding="utf-8")
    r = textpack.check(tmp_path, textpack.port_english())
    assert r.refused == ["Scripts.strings: line 1: invalid UTF-8 (byte 0xFF at offset 7)"]
    assert r.entries == 7
    assert len(r.cut) == 1 and "80 bytes, its field holds 64" in r.cut[0]
    assert r.cut[0].endswith(repr("ö" * 32))
    assert [x.split(":")[0] for x in r.ignored] == [
        'Tables.strings "item_etc.3.price"',
        'Tables.strings "mob.x.name"',
        'Port.strings "menu_not_in_build"',
        'Port.strings "nope"',
    ]
    assert r.notes == ["Other.strings: not a text pack file: ignored"]

    bad = tmp_path / "bad"
    bad.mkdir()
    (bad / "textpack.strings").write_text(
        '"format" = "2";\n"language" = "x y";\n', encoding="utf-8"
    )
    assert textpack.check(bad).refused == [
        'textpack.strings: its "format" is not "1"',
        'textpack.strings: it has no "name"',
        'textpack.strings: its "language" is not a language tag (en, pt-BR)',
    ]


def test_port_english_has_the_table_the_registry_and_the_actions() -> None:
    port = textpack.port_english()
    assert port["menu_title"] == "SETTINGS"
    assert port["menu_set_in_ini"] == "Set {0} in settings.ini: {1}"
    assert port["setting.text_pack.label"] == "Text pack"
    assert port["setting.audio_pack.help"].startswith("A folder of music and sound files")
    assert port["action.quit.confirm"] == "Quit MapleThief?"
    assert port["action.backups.button"] == "List..."
    assert all(v.isascii() for v in port.values())
    assert sum(k.startswith("setting.") and k.endswith(".label") for k in port) > 50


def _record(cls: type, **values: object) -> object:
    blank = {f.name: "" if f.metadata["kind"] == "str" else 0 for f in dataclasses.fields(cls)}
    blank.update(values)
    return cls(**blank)


def _bundle(root: Path) -> Path:
    app = root / "MapleThief.app"
    (app / "en.lproj").mkdir(parents=True)
    with open(app / "en.lproj" / "Scripts.strings", "wb") as fh:
        plistlib.dump({"Q_1": "Welcome to Town$", "Q_2": 'Say "hi"'}, fh, fmt=plistlib.FMT_BINARY)
    with open(app / "en.lproj" / "ScriptExtra.strings", "wb") as fh:
        plistlib.dump({"X": "extra"}, fh, fmt=plistlib.FMT_BINARY)
    data = app / "English" / "data"
    data.mkdir(parents=True)
    names = {
        "item_etc": [_record(datatables.ItemEtc, id=1, name="Tonic", description="Heals$a bit")],
        "npc": [_record(datatables.Npc, id=1, name="Guide")],
        "mob": [_record(datatables.Mob, id=1, name="Blob"), _record(datatables.Mob, id=2)],
    }
    for name, spec in datatables.TABLES.items():
        records = names.get(name, [_record(spec.record)])
        (data / f"{name}.dat").write_bytes(datatables.encode(name, records))
    return app


def test_export_from_a_synthetic_bundle(tmp_path: Path) -> None:
    app = _bundle(tmp_path)
    pack = tmp_path / "pack"
    counts = textpack.export(pack, app)
    assert counts["Scripts"] == 2 and counts["ScriptExtra"] == 1
    assert counts["Tables"] == 4  # the empty name of mob 2 and the blank rows are left out
    scripts = parse_strings((pack / "Scripts.strings").read_bytes()).entries
    assert scripts == {"Q_1": "Welcome to Town$", "Q_2": 'Say "hi"'}
    tables_text = (pack / "Tables.strings").read_text(encoding="utf-8")
    assert '/* at most 64 bytes */\n"item_etc.0.name" = "Tonic";' in tables_text
    assert parse_strings(tables_text.encode()).entries == {
        "mob.0.name": "Blob",
        "npc.0.name": "Guide",
        "item_etc.0.name": "Tonic",
        "item_etc.0.description": "Heals$a bit",
    }
    report = textpack.check(pack, textpack.port_english())
    assert report.ok and not report.cut and not report.ignored, report
    assert report.entries == 2 + 1 + 4 + counts["Port"]


def test_cli(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    app = _bundle(tmp_path)
    pack = tmp_path / "pack"
    assert textpack.main(["export", str(pack), "--bundle", str(app)]) == 0
    assert "keep this folder outside the repository" in capsys.readouterr().out
    assert textpack.main(["check", str(pack)]) == 0
    (pack / "Port.strings").write_bytes(b'"menu_title" = "\xc3";')
    assert textpack.main(["check", str(pack)]) == 1
    assert "refused: Port.strings: line 1: invalid UTF-8" in capsys.readouterr().out
    assert textpack.main(["export", str(pack), "--bundle", str(tmp_path / "no")]) == 2


@pytest.mark.skipif(
    not (ROOT / textpack.DEFAULT_BUNDLE).is_dir(),
    reason="needs work/ipa (scripts/bootstrap)",
)
@pytest.mark.parametrize("language", ["en", "ko"])
def test_export_of_the_real_bundle(tmp_path: Path, language: str) -> None:
    """Every text of your own copy exports to a pack the runtime opens whole: English uncut
    (its longest names fill their fields exactly); Korean in UTF-8 takes 3 bytes a syllable
    where the tables' CP949 takes 2, so some names would be cut, as check reports."""
    counts = textpack.export(tmp_path, ROOT / textpack.DEFAULT_BUNDLE, language)
    assert counts["Scripts"] > 1000 and counts["Tables"] > 300
    report = textpack.check(tmp_path, textpack.port_english())
    assert report.ok, report.refused
    assert not report.ignored
    if language == "en":
        assert not report.cut
    else:
        assert 0 < len(report.cut) < counts["Tables"] // 4
