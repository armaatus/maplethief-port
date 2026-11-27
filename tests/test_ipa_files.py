"""The per-file hash list of the bundle (#350, tools/mtport/ipa_files.py), on synthetic bundles:
what it lists and leaves out, its include-file form, and that it is rewritten only on a change."""

from __future__ import annotations

import hashlib
from pathlib import Path

import pytest

from mtport import ipa_files


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def make_bundle(root: Path, files: dict[str, bytes]) -> Path:
    for name, data in files.items():
        p = root / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
    return root


BUNDLE = {
    "MapleThief": b"\xca\xfe\xba\xbe the executable",
    "MapleThief_armv6": b"",
    "_CodeSignature/CodeResources": b"<plist/>",
    "CodeResources": b"<plist/>",
    "ResourceRules.plist": b"<plist/>",
    "PkgInfo": b"APPL????",
    "README.md": b"# a stray SDK note",
    "SC_Info/MapleThief.sinf": b"x",
    "embedded.mobileprovision": b"x",
    "Info.plist": b"bplist00",
    "map/m1.map": b"a map",
    "English/data/item.dat": b"items",
    "sprite/mob/res/a b.png": b"art",
}


def test_lists_the_files_the_game_reads_sorted(tmp_path):
    app = make_bundle(tmp_path / "MapleThief.app", BUNDLE)
    assert ipa_files.file_hashes(app) == [
        ("English/data/item.dat", sha(b"items")),
        ("Info.plist", sha(b"bplist00")),
        ("map/m1.map", sha(b"a map")),
        ("sprite/mob/res/a b.png", sha(b"art")),
    ]


def test_renders_cxx_initializers_with_only_hashes(tmp_path):
    app = make_bundle(tmp_path / "MapleThief.app", {"map/m1.map": b"a map"})
    text = ipa_files.render(ipa_files.file_hashes(app))
    assert text == ipa_files.HEADER + f'{{"map/m1.map", "{sha(b"a map")}"}},\n'
    assert "a map" not in text.replace("map/m1.map", "")


def test_write_only_on_change(tmp_path):
    app = make_bundle(tmp_path / "MapleThief.app", {"Info.plist": b"1"})
    out = tmp_path / "gen" / "ipa_files.inc"
    assert ipa_files.write(app, out) == 1
    first = out.stat().st_mtime_ns
    assert ipa_files.write(app, out) == 1
    assert out.stat().st_mtime_ns == first
    (app / "Info.plist").write_bytes(b"2")
    ipa_files.write(app, out)
    assert sha(b"2") in out.read_text(encoding="utf-8")
    assert not list(out.parent.glob(".*.tmp"))


def test_refuses_no_bundle_an_empty_one_and_unlistable_names(tmp_path):
    with pytest.raises(ipa_files.IpaFilesError, match="not a folder"):
        ipa_files.file_hashes(tmp_path / "absent")
    app = make_bundle(tmp_path / "a.app", {"MapleThief": b"x", "PkgInfo": b"x"})
    with pytest.raises(ipa_files.IpaFilesError, match="no file the game reads"):
        ipa_files.file_hashes(app)
    app = make_bundle(tmp_path / "b.app", {'say "hi".txt': b"x"})
    with pytest.raises(ipa_files.IpaFilesError, match="cannot hold"):
        ipa_files.file_hashes(app)


def test_main(tmp_path, capsys):
    app = make_bundle(tmp_path / "MapleThief.app", {"Info.plist": b"1"})
    out = tmp_path / "ipa_files.inc"
    assert ipa_files.main([str(app), str(out)]) == 0
    assert "1 file hashes" in capsys.readouterr().out
    assert ipa_files.main([str(tmp_path / "none"), str(out)]) == 1
