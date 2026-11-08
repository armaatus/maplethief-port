"""The saves out of an iPhone or iPod backup (tools/mtport/iosbackup.py, #149,
docs/formats/ios-backup.md), on synthetic backups of both manifest formats. No real backup is
ever used or committed: a "parse every shipped file" test does not apply (the IPA holds none).
CTest saves.ios_backup_import runs the same backups through the program's --import-saves."""

from __future__ import annotations

import hashlib
import os
import plistlib
import random
import sqlite3
import sys
import zipfile
from pathlib import Path

import pytest

from mtport import iosbackup as ib
from mtport import savecheck as sc
from repo_paths import REPO

FORMATS = ("db", "mbdb")
APP = f"AppDomain-{ib.APP_ID}"


def backup(tmp: Path, fmt: str, files: dict[str, bytes] | None = None, **kw: object) -> Path:
    files = sc.full_set() if files is None else files
    return ib.write_synthetic_backup(tmp / f"b-{fmt}", ib.synthetic_entries(files), fmt, **kw)  # type: ignore[arg-type]


def test_file_id_is_sha1_of_domain_and_path() -> None:
    # The issue's example: SHA-1 of "AppDomain-com.nexonmobile.maplestory.thief-Documents/theif0".
    want = hashlib.sha1(b"AppDomain-com.nexonmobile.maplestory.thief-Documents/theif0").hexdigest()
    assert ib.file_id(APP, "Documents/theif0") == want
    assert len(want) == 40


@pytest.mark.parametrize("fmt", FORMATS)
def test_finds_the_games_documents_in_both_formats(tmp_path: Path, fmt: str) -> None:
    want = sc.full_set()
    docs = ib.read_app_documents(backup(tmp_path, fmt, dict(want, notes=b"x")))
    assert docs.files == want  # byte for byte; not the other app's theif0 nor Library/
    assert docs.ignored == ["notes"]
    assert docs.info.manifest == fmt
    assert docs.info.device == "Test iPod"
    assert docs.info.has_app is True
    assert docs.info.finished is True
    assert not docs.info.encrypted
    assert ib.check(docs.files) == []


@pytest.mark.parametrize("fmt", FORMATS)
def test_a_new_character_backup_is_found_too(tmp_path: Path, fmt: str) -> None:
    want = sc.new_character_set()
    assert ib.read_app_documents(backup(tmp_path, fmt, want)).files == want


@pytest.mark.parametrize("fmt", FORMATS)
def test_extract_writes_a_set_the_import_reads(tmp_path: Path, fmt: str) -> None:
    b = backup(tmp_path, fmt)
    out = tmp_path / "out"
    assert ib.main(["extract", str(b), str(out)]) == 0
    files, ignored = sc.read_set(out)
    assert files == sc.full_set() and ignored == []
    assert ib.main(["extract", str(b), str(out)]) == 1  # not into a folder with files in it
    z = tmp_path / "saves.zip"
    assert ib.main(["extract", str(b), str(z)]) == 0
    with zipfile.ZipFile(z) as zf:
        assert {i.filename: zf.read(i) for i in zf.infolist()} == sc.full_set()
        assert all(i.compress_type == zipfile.ZIP_STORED for i in zf.infolist())


@pytest.mark.parametrize("fmt", FORMATS)
def test_the_bare_form_shows_each_files_verdict(
    tmp_path: Path, fmt: str, capsys: pytest.CaptureFixture[str]
) -> None:
    assert ib.main([str(backup(tmp_path, fmt))]) == 0
    out = capsys.readouterr().out
    assert "theif0: ok" in out and "quest2: ok" in out and "Test iPod" in out


@pytest.mark.parametrize("fmt", FORMATS)
def test_a_save_that_fails_the_checks_is_refused_and_nothing_written(
    tmp_path: Path, fmt: str, capsys: pytest.CaptureFixture[str]
) -> None:
    files = sc.full_set()
    files["theif1"] = files["theif1"][:-1]
    b = backup(tmp_path, fmt, files)
    out = tmp_path / "out"
    assert ib.main(["extract", str(b), str(out)]) == 3
    assert not out.exists()
    assert "theif1: bad_size" in capsys.readouterr().out
    del files["theif1"]
    assert ib.main(["show", str(backup(tmp_path / "pair", fmt, files))]) == 3  # bad_pair


@pytest.mark.parametrize("fmt", FORMATS)
def test_an_encrypted_backup_is_refused_with_how_to_fix_it(tmp_path: Path, fmt: str) -> None:
    b = backup(tmp_path, fmt, encrypted=True)
    with pytest.raises(ib.EncryptedBackupError, match="Encrypt local backup"):
        ib.read_app_documents(b)
    # Without Manifest.plist's IsEncrypted, the encrypted Manifest.db or the mbdb's record keys
    # still give it away.
    (b / "Manifest.plist").unlink()
    with pytest.raises(ib.EncryptedBackupError, match="encrypted"):
        ib.read_app_documents(b)
    assert ib.main(["extract", str(b), str(tmp_path / "o")]) == 1
    assert not (tmp_path / "o").exists()


@pytest.mark.parametrize("fmt", FORMATS)
def test_a_backup_without_the_game_says_so(tmp_path: Path, fmt: str) -> None:
    b = ib.write_synthetic_backup(
        tmp_path / "b", ib.synthetic_entries({})[:4], fmt, applications=("com.example.other",)
    )
    with pytest.raises(ib.IosBackupError, match="still be installed"):
        ib.read_app_documents(b)
    assert ib.backup_info(b).has_app is False


def test_not_a_backup_and_an_ios3_backup(tmp_path: Path) -> None:
    with pytest.raises(ib.IosBackupError, match="not an iOS backup"):
        ib.read_app_documents(tmp_path)
    (tmp_path / "Manifest.plist").write_bytes(plistlib.dumps({"IsEncrypted": False}))
    (tmp_path / "0a1b.mdinfo").write_bytes(b"")
    with pytest.raises(ib.IosBackupError, match="iOS 3"):
        ib.read_app_documents(tmp_path)
    with pytest.raises(ib.IosBackupError, match="not a folder"):
        ib.read_app_documents(tmp_path / "missing")


def test_a_folder_of_backups_lists_them(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    root = tmp_path / "Backup"
    ib.write_synthetic_backup(root / "aaaa", ib.synthetic_entries(sc.full_set()), "db")
    ib.write_synthetic_backup(
        root / "bbbb", ib.synthetic_entries(sc.full_set()), "mbdb", encrypted=True
    )
    (root / "not-a-backup").mkdir()
    found = ib.find_backups([root])
    assert [b.path.name for b in found] == ["aaaa", "bbbb"]
    assert [b.encrypted for b in found] == [False, True]
    assert "has MapleStory Thief Edition" in found[0].line()
    assert "encrypted" in found[1].line()
    assert "Manifest.db" in found[0].line() and "Manifest.mbdb" in found[1].line()
    with pytest.raises(ib.IosBackupError, match="folder of backups"):
        ib.read_app_documents(root)
    assert ib.main(["find", str(root)]) == 0
    assert "aaaa" in capsys.readouterr().out
    assert ib.main(["find", str(tmp_path / "none")]) == 1


def test_default_backup_roots_are_the_usual_places(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(sys, "platform", "darwin")
    assert str(ib.default_backup_roots()[0]).endswith(
        os.path.join("Library", "Application Support", "MobileSync", "Backup")
    )
    monkeypatch.setattr(sys, "platform", "win32")
    monkeypatch.setenv("APPDATA", "C:\\Users\\x\\AppData\\Roaming")
    roots = [str(r) for r in ib.default_backup_roots()]
    assert roots[0].endswith(os.path.join("Apple", "MobileSync", "Backup"))
    assert "Apple Computer" in roots[1]
    monkeypatch.setattr(sys, "platform", "linux")
    assert ib.default_backup_roots() == []


def test_a_status_plist_that_did_not_finish_warns(tmp_path: Path) -> None:
    b = backup(tmp_path, "db")
    (b / "Status.plist").write_bytes(plistlib.dumps({"SnapshotState": "uploading"}))
    assert ib.read_app_documents(b).warnings == ["Status.plist says this backup did not finish"]


# -- hostile backups ------------------------------------------------------------------------


def link(path: Path, target: Path) -> None:
    try:
        path.symlink_to(target)
    except OSError as e:  # Windows without Developer Mode or admin: WinError 1314
        pytest.skip(f"cannot create a symbolic link here: {e}")


def db_rows(b: Path, sql: str, *args: object) -> None:
    con = sqlite3.connect(b / "Manifest.db")
    con.execute(sql, args)
    con.commit()
    con.close()


def test_paths_outside_documents_never_become_names(tmp_path: Path) -> None:
    b = backup(tmp_path, "db")
    evil = {
        "Documents/../../../etc/theif0": b"x",
        "Documents/sub/theif0": b"x",
        "Documents/..": b"x",
        "Library/theif0": b"x",
        "Documents/..\\theif0": b"x",
    }
    for rel, data in evil.items():
        fid = ib.file_id(APP, rel)
        (b / fid[:2]).mkdir(exist_ok=True)
        (b / fid[:2] / fid).write_bytes(data)
        db_rows(b, "INSERT INTO Files VALUES (?, ?, ?, 1, NULL)", fid, APP, rel)
    docs = ib.read_app_documents(b)
    assert docs.files == sc.full_set()
    assert set(docs.ignored) == {
        "Documents/../../../etc/theif0",
        "Documents/sub/theif0",
        "Documents/..",
        "Documents/..\\theif0",
    }


@pytest.mark.parametrize("bad_id", ["../../../../etc/passwd", "ABCDEF" + "0" * 34, "0" * 39, ""])
def test_a_malformed_file_id_is_refused(tmp_path: Path, bad_id: str) -> None:
    b = backup(tmp_path, "db")
    db_rows(b, "UPDATE Files SET fileID = ? WHERE relativePath = 'Documents/config'", bad_id)
    with pytest.raises(ib.IosBackupError, match="malformed file ID"):
        ib.read_app_documents(b)


def test_a_missing_or_huge_data_file_or_a_folder_is_refused(tmp_path: Path) -> None:
    b = backup(tmp_path, "db")
    fid = ib.file_id(APP, "Documents/config")
    data = b / fid[:2] / fid
    data.unlink()
    with pytest.raises(ib.IosBackupError, match="missing"):
        ib.read_app_documents(b)
    with open(data, "wb") as f:
        f.truncate(ib.MAX_FILE_BYTES + 1)  # sparse
    with pytest.raises(ib.IosBackupError, match="over the limit"):
        ib.read_app_documents(b)
    data.unlink()
    data.mkdir()
    with pytest.raises(ib.IosBackupError, match="not a file"):
        ib.read_app_documents(b)


def test_a_linked_data_file_is_refused(tmp_path: Path) -> None:
    b = backup(tmp_path, "db")
    fid = ib.file_id(APP, "Documents/config")
    data = b / fid[:2] / fid
    data.unlink()
    secret = tmp_path / "secret"
    secret.write_bytes(bytes(6))
    link(data, secret)
    with pytest.raises(ib.IosBackupError, match="is a link"):
        ib.read_app_documents(b)


def test_a_linked_data_folder_is_refused(tmp_path: Path) -> None:
    b = backup(tmp_path, "db")
    fid = ib.file_id(APP, "Documents/config")
    real = tmp_path / "elsewhere"
    (b / fid[:2]).rename(real)
    link(b / fid[:2], real)
    with pytest.raises(ib.IosBackupError, match="is a link"):
        ib.read_app_documents(b)


def test_a_game_file_listed_as_a_link_or_folder_or_twice_is_refused(tmp_path: Path) -> None:
    b = backup(tmp_path, "db")
    db_rows(b, "UPDATE Files SET flags = 4 WHERE relativePath = 'Documents/config'")
    with pytest.raises(ib.IosBackupError, match="is a link in the backup"):
        ib.read_app_documents(b)
    b = backup(tmp_path / "2", "mbdb")
    records = ib.parse_mbdb((b / "Manifest.mbdb").read_bytes())
    twice = [r for r in records if r.path == "Documents/cupon"]
    (b / "Manifest.mbdb").write_bytes(ib.encode_mbdb(records + twice))
    with pytest.raises(ib.IosBackupError, match="twice"):
        ib.read_app_documents(b)


def test_an_mbdb_size_that_differs_from_the_data_is_refused(tmp_path: Path) -> None:
    b = backup(tmp_path, "mbdb")
    fid = ib.file_id(APP, "Documents/theif0")
    (b / fid).write_bytes((b / fid).read_bytes()[:100])
    with pytest.raises(ib.IosBackupError, match="the manifest says 1124"):
        ib.read_app_documents(b)


def test_manifest_db_that_is_not_the_schema_is_an_error(tmp_path: Path) -> None:
    b = backup(tmp_path, "db")
    db_rows(b, "DROP TABLE Files")
    with pytest.raises(ib.IosBackupError, match="cannot be read"):
        ib.read_app_documents(b)


def test_manifest_db_is_opened_read_only(tmp_path: Path) -> None:
    b = backup(tmp_path, "db")
    before = sorted(p.name for p in b.iterdir())
    stamp = (b / "Manifest.db").read_bytes()
    ib.read_app_documents(b)
    assert sorted(p.name for p in b.iterdir()) == before  # no -journal, -wal or -shm
    assert (b / "Manifest.db").read_bytes() == stamp


def test_broken_plists_are_errors(tmp_path: Path) -> None:
    b = backup(tmp_path, "db")
    (b / "Manifest.plist").write_bytes(b"bplist00\xff\xff")
    with pytest.raises(ib.IosBackupError, match="not a property list"):
        ib.read_app_documents(b)
    (b / "Manifest.plist").write_bytes(plistlib.dumps([1, 2]))
    with pytest.raises(ib.IosBackupError, match="not a dictionary"):
        ib.read_app_documents(b)
    entity = (
        b'<?xml version="1.0"?><!DOCTYPE plist [<!ENTITY a "aaaa">]>'
        b"<plist><dict><key>IsEncrypted</key><string>&a;</string></dict></plist>"
    )
    (b / "Manifest.plist").write_bytes(entity)
    with pytest.raises(ib.IosBackupError, match="not a property list"):
        ib.read_app_documents(b)


# -- Manifest.mbdb ------------------------------------------------------------------------------


def sample_records() -> list[ib.MbdbRecord]:
    return [
        ib.MbdbRecord(APP, "", mode=ib.S_IFDIR | 0o755),
        ib.MbdbRecord(
            APP,
            "Documents/theif0",
            data_hash=bytes(20),
            size=1124,
            inode=7,
            mtime=1,
            atime=2,
            ctime=3,
            protection=3,
            properties=(("com.apple.backup.x", b"\x01\x02"), ("empty", b"")),
        ),
        ib.MbdbRecord("HomeDomain", "Library/x", link_target="/y", mode=ib.S_IFLNK | 0o777),
        ib.MbdbRecord("HomeDomain", "Media/ñ.txt", encryption_key=b"k" * 44, size=5),
    ]


def test_mbdb_round_trips_and_lays_out_like_the_spec() -> None:
    records = sample_records()
    data = ib.encode_mbdb(records)
    assert data[:6] == b"mbdb\x05\x00"
    assert ib.parse_mbdb(data) == records
    # The first record, byte by byte (docs/formats/ios-backup.md).
    first = ib.encode_mbdb(records[:1])[6:]
    domain = APP.encode()
    assert first[:2] == len(domain).to_bytes(2, "big") and first[2 : 2 + len(domain)] == domain
    rest = first[2 + len(domain) :]
    assert rest[:8] == b"\xff\xff" * 4  # path "" (null), link, hash and key all null
    assert rest[8:10] == (ib.S_IFDIR | 0o755).to_bytes(2, "big")
    assert len(rest) == 8 + 2 + 8 + 4 * 5 + 8 + 1 + 1
    assert ib.parse_mbdb(b"mbdb\x04\x00" + ib.encode_mbdb(records)[6:]) == records


def test_mbdb_rejects_bad_headers() -> None:
    for data, msg in (
        (b"", "cut short"),
        (b"mbdx\x05\x00", "does not start"),
        (b"mbdb\x06\x00", "version 6.0"),
        (b"mbdb", "cut short"),
    ):
        with pytest.raises(ib.IosBackupError, match=msg):
            ib.parse_mbdb(data)
    assert ib.parse_mbdb(b"mbdb\x05\x00") == []


def test_mbdb_never_reads_past_its_end() -> None:
    data = ib.encode_mbdb(sample_records())
    boundaries = {len(ib.encode_mbdb(sample_records()[:n])) for n in range(5)}
    for cut in range(len(data)):
        if cut in boundaries:
            assert len(ib.parse_mbdb(data[:cut])) < len(sample_records())
            continue
        with pytest.raises(ib.IosBackupError):
            ib.parse_mbdb(data[:cut])


def test_mbdb_random_bytes_raise_only_backup_errors() -> None:
    rng = random.Random(149)
    good = ib.encode_mbdb(sample_records())
    for _ in range(2000):
        data = bytearray(good)
        for _ in range(rng.randint(1, 8)):
            data[rng.randrange(6, len(data))] = rng.randrange(256)
        try:
            ib.parse_mbdb(bytes(data))
        except ib.IosBackupError:
            pass
    for _ in range(300):
        junk = b"mbdb\x05\x00" + rng.randbytes(rng.randint(0, 400))
        try:
            ib.parse_mbdb(junk)
        except ib.IosBackupError:
            pass


def test_mbdb_text_that_is_not_utf8_is_an_error() -> None:
    data = b"mbdb\x05\x00" + b"\x00\x02\xff\xfe"
    with pytest.raises(ib.IosBackupError, match="not UTF-8"):
        ib.parse_mbdb(data)


# -- the program's side ---------------------------------------------------------------------


def fake_program(tmp: Path) -> list[str]:
    """A stand-in for MapleThief: records its arguments and the folder's files."""
    script = tmp / "fake_maplethief.py"
    script.write_text(
        "import pathlib, sys\n"
        "args = sys.argv[1:]\n"
        "src = pathlib.Path(args[args.index('--import-saves') + 1])\n"
        "out = pathlib.Path(sys.argv[0]).with_name('got')\n"
        "out.mkdir(exist_ok=True)\n"
        "for p in src.iterdir():\n"
        "    (out / p.name).write_bytes(p.read_bytes())\n"
        "(out.parent / 'args.txt').write_text(' '.join(args[2:]), encoding='utf-8')\n"
        "print('imported')\n"
        "sys.exit(0)\n",
        encoding="utf-8",
    )
    return [sys.executable, str(script)]


@pytest.mark.parametrize("fmt", FORMATS)
def test_import_hands_the_saves_to_the_programs_import(
    tmp_path: Path, fmt: str, capsys: pytest.CaptureFixture[str]
) -> None:
    prog = fake_program(tmp_path)
    b = backup(tmp_path, fmt)
    assert ib.main(["import", str(b), "--program", *prog, "--save-dir", "/x/y"]) == 0
    got = {p.name: p.read_bytes() for p in (tmp_path / "got").iterdir()}
    assert got == sc.full_set()
    assert (tmp_path / "args.txt").read_text(encoding="utf-8") == "--save-dir /x/y"
    assert "imported" in capsys.readouterr().out


def test_program_command_finds_the_executable_inside_the_app(tmp_path: Path) -> None:
    app = tmp_path / "MapleThief.app"
    app.mkdir()
    assert ib.program_command(str(app)) == [str(app / "Contents" / "MacOS" / "MapleThief")]
    assert ib.program_command("/bin/MapleThief") == ["/bin/MapleThief"]


def test_the_fixture_command_writes_a_backup(tmp_path: Path) -> None:
    assert ib.main(["fixture", str(tmp_path / "f"), "--format", "mbdb"]) == 0
    assert ib.read_app_documents(tmp_path / "f").files == sc.full_set()


def test_selftest_with_a_fake_program_that_imports(tmp_path: Path) -> None:
    """selftest's own logic, with a stand-in that copies into MAPLETHIEF_DATA_DIR like the
    real import (CTest saves.ios_backup_import runs it on the real program)."""
    script = tmp_path / "fake.py"
    script.write_text(
        "import os, pathlib, sys\n"
        "src = pathlib.Path(sys.argv[sys.argv.index('--import-saves') + 1])\n"
        "data = pathlib.Path(os.environ['MAPLETHIEF_DATA_DIR'])\n"
        "docs = data / 'saves'\n"
        "docs.mkdir(parents=True, exist_ok=True)\n"
        "bk = data / 'backups' / 'import' / '20260101-000000-000'\n"
        "bk.mkdir(parents=True)\n"
        "for p in src.iterdir():\n"
        "    if (docs / p.name).exists():\n"
        "        (bk / p.name).write_bytes((docs / p.name).read_bytes())\n"
        "    (docs / p.name).write_bytes(p.read_bytes())\n",
        encoding="utf-8",
    )
    assert ib.selftest([sys.executable, str(script)], tmp_path / "work") == []


def test_the_docs_describe_the_tool() -> None:
    spec = (REPO / "docs/formats/ios-backup.md").read_text(encoding="utf-8")
    index = (REPO / "docs/formats/README.md").read_text(encoding="utf-8")
    howto = (REPO / "docs/play/iphone-saves.md").read_text(encoding="utf-8")
    assert "](ios-backup.md" in index
    for text in ("Manifest.db", "Manifest.mbdb", "IsEncrypted", "SHA-1"):
        assert text in spec
    assert "Encrypt local backup" in howto
    for cmd in ("find", "extract", "import"):
        assert f"mtport.iosbackup {cmd}" in howto
    saves = (REPO / "docs/play/saves.md").read_text(encoding="utf-8")
    assert "](iphone-saves.md)" in saves
