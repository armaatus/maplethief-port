"""The game's saves out of an iPhone or iPod backup (#149, docs/formats/ios-backup.md).

The game does not share its ``Documents/`` folder with iTunes or the Files app (its
``Info.plist`` has no ``UIFileSharingEnabled``), so the way to get an original save off a
device without a jailbreak is an **unencrypted** Finder or iTunes backup
(``docs/play/iphone-saves.md`` says how to make one). This module reads such a backup, finds
the files of ``AppDomain-com.nexonmobile.maplestory.thief`` under ``Documents/``, checks them
with :mod:`mtport.savecheck` and hands them to the port's own import (#148, ADR 0078): it
writes a folder or a zip that ``MapleThief --import-saves`` takes, or runs that import.

Two manifest formats:

- ``Manifest.db`` (iOS 10 and later): SQLite, table ``Files(fileID, domain, relativePath,
  flags, file)``; a file's data is at ``<fileID[:2]>/<fileID>``.
- ``Manifest.mbdb`` (iOS 4 to 9): a list of binary records; a file's data is at
  ``<fileID>``, flat. In both, ``fileID`` is the SHA-1 of ``<domain>-<relativePath>``.

A backup is untrusted data: it is parsed, never executed. The SQLite file is opened read-only
and immutable, every mbdb read is bounds-checked, the names written come from the game's own
list (never from a path in the backup), fileIDs must be 40 hex digits, links are refused and
each file is capped at the import's 16 MiB. An encrypted backup is refused: its file data is
encrypted, and so (since iOS 10.2) is ``Manifest.db``.

Command line::

    python3 -m mtport.iosbackup find [DIR]                # the backups in DIR, or the usual places
    python3 -m mtport.iosbackup BACKUP                    # the game's files in it, checked
    python3 -m mtport.iosbackup extract BACKUP OUT        # to a new folder, or OUT.zip
    python3 -m mtport.iosbackup import BACKUP --program EXE [--save-dir DIR]
    python3 -m mtport.iosbackup fixture OUT [--format db|mbdb] [--encrypted]   # synthetic
    python3 -m mtport.iosbackup selftest --program EXE [--work DIR]

Exit status: 0; 1 when the backup cannot be used (missing, encrypted, no saves of the game);
2 for a usage error; 3 when a save fails the checks (as ``--check-saves``); ``import`` exits
with the program's status.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import os
import plistlib
import re
import shutil
import sqlite3
import stat
import struct
import subprocess
import sys
import tempfile
import zipfile
from collections.abc import Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from mtport import savecheck as sc

APP_ID = "com.nexonmobile.maplestory.thief"  # CFBundleIdentifier (ANALYSIS.md)
DOCUMENTS = "Documents/"  # the game's files: Documents/<name>, no subfolders (save.md)

MAX_FILE_BYTES = 16 * 1024 * 1024  # kMaxSaveSourceFileBytes: the import's cap per file
MAX_PLIST_BYTES = 64 * 1024 * 1024  # Info.plist holds app icons; a few MB on a big phone
MAX_MBDB_BYTES = 512 * 1024 * 1024  # one record per file of the device: tens of MB at most
MAX_APP_ROWS = 100_000  # rows of the game's domain read from Manifest.db

SQLITE_MAGIC = b"SQLite format 3\x00"
MBDB_MAGIC = b"mbdb"
MBDB_VERSIONS = {(5, 0): "iOS 5-9", (4, 0): "iOS 4 (with Manifest.mbdx)"}
MBDB_NULL = 0xFFFF  # a string length meaning "no string"

# Manifest.db Files.flags
FLAG_FILE, FLAG_DIRECTORY, FLAG_SYMLINK = 1, 2, 4
# mbdb mode, the st_mode type bits
S_IFMT, S_IFREG, S_IFDIR, S_IFLNK = 0xF000, 0x8000, 0x4000, 0xA000

_FILE_ID = re.compile(r"[0-9a-f]{40}")


class IosBackupError(Exception):
    """The backup cannot be used; the message says why in plain words."""


class EncryptedBackupError(IosBackupError):
    pass


ENCRYPTED_HOWTO = (
    "This backup is encrypted, and an encrypted backup's files cannot be read without its "
    "password. Make an unencrypted one: in Finder (or Apple Devices / iTunes) select the "
    'device, untick "Encrypt local backup" (it asks for the backup password), then click '
    '"Back Up Now". See docs/play/iphone-saves.md.'
)


def file_id(domain: str, relative_path: str) -> str:
    """The name a backup gives a file's data: SHA-1 of ``<domain>-<relativePath>``, hex."""
    return hashlib.sha1(f"{domain}-{relative_path}".encode()).hexdigest()


# -- reading what the backup says about itself ----------------------------------------------


def _read_capped(path: Path, cap: int, what: str) -> bytes:
    """A regular file's bytes, refusing links, other kinds of file and files over ``cap``."""
    try:
        st = os.lstat(path)
    except FileNotFoundError:
        raise IosBackupError(f"{what} is missing ({path})") from None
    except PermissionError:
        raise IosBackupError(_permission_message(path)) from None
    if stat.S_ISLNK(st.st_mode):
        raise IosBackupError(f"{what} is a link ({path}); a backup never holds one")
    if not stat.S_ISREG(st.st_mode):
        raise IosBackupError(f"{what} is not a file ({path})")
    if st.st_size > cap:
        raise IosBackupError(f"{what} has {st.st_size} bytes, over the limit of {cap}")
    try:
        with open(path, "rb") as f:
            data = f.read(cap + 1)
    except PermissionError:
        raise IosBackupError(_permission_message(path)) from None
    if len(data) > cap:
        raise IosBackupError(f"{what} is over the limit of {cap} bytes")
    return data


def _permission_message(path: Path) -> str:
    msg = f"Cannot read {path}: permission denied."
    if sys.platform == "darwin" and "MobileSync" in str(path):
        msg += (
            " macOS protects the backup folder: give your terminal Full Disk Access (System "
            "Settings > Privacy & Security > Full Disk Access), or copy the backup's folder "
            "somewhere else in Finder and run this on the copy."
        )
    return msg


def read_plist(path: Path, what: str) -> dict[str, Any] | None:
    """A property list file as a dict, None if absent; IosBackupError if it is not one."""
    if not path.exists() and not path.is_symlink():
        return None
    data = _read_capped(path, MAX_PLIST_BYTES, what)
    try:
        value = plistlib.loads(data)
    except Exception as e:  # plistlib raises several kinds on broken input
        raise IosBackupError(f"{what} is not a property list: {e}") from None
    if not isinstance(value, dict):
        raise IosBackupError(f"{what} is not a dictionary")
    return value


def _text(value: object) -> str:
    return value if isinstance(value, str) else ""


@dataclass(frozen=True)
class BackupInfo:
    """What a backup's folder says about itself (Manifest.plist, Info.plist, Status.plist)."""

    path: Path
    manifest: str  # "db", "mbdb" or "" (none: not a backup this module reads)
    encrypted: bool
    device: str = ""
    ios: str = ""
    date: str = ""  # ISO 8601, UTC, if the backup has one
    has_app: bool | None = None  # Manifest.plist's Applications lists the game (None: no list)
    finished: bool | None = None  # Status.plist's SnapshotState is "finished"
    problem: str = ""  # why it cannot be read, if known up front

    def line(self) -> str:
        parts = [self.path.name]
        if self.device:
            parts.append(self.device)
        if self.ios:
            parts.append(f"iOS {self.ios}")
        if self.date:
            parts.append(self.date)
        if self.manifest:
            parts.append(f"Manifest.{self.manifest}")
        if self.problem:
            parts.append(self.problem)
        elif self.encrypted:
            parts.append("encrypted: cannot be read")
        elif self.has_app is False:
            parts.append("no MapleStory Thief Edition")
        elif self.has_app:
            parts.append("has MapleStory Thief Edition")
        if self.finished is False:
            parts.append("did not finish")
        return " | ".join(parts)


def _iso(value: object) -> str:
    if isinstance(value, dt.datetime):
        if value.tzinfo is None:
            value = value.replace(tzinfo=dt.timezone.utc)
        return value.astimezone(dt.timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    return ""


def _is_old_format(path: Path) -> bool:
    try:
        return any(p.suffix in (".mdinfo", ".mddata") for p in path.iterdir())
    except OSError:
        return False


def backup_info(path: Path) -> BackupInfo:
    """What the backup in ``path`` is. Raises IosBackupError only for unreadable plists."""
    manifest = read_plist(path / "Manifest.plist", "Manifest.plist") or {}
    info = read_plist(path / "Info.plist", "Info.plist") or {}
    status = read_plist(path / "Status.plist", "Status.plist") or {}
    lockdown = manifest.get("Lockdown")
    lockdown = lockdown if isinstance(lockdown, dict) else {}
    apps = manifest.get("Applications")
    if (path / "Manifest.db").is_file():
        kind = "db"
    elif (path / "Manifest.mbdb").is_file():
        kind = "mbdb"
    else:
        kind = ""
    problem = ""
    if not kind:
        if _is_old_format(path):
            problem = "an iOS 3 backup (.mdinfo files): not supported"
        else:
            problem = "no Manifest.db or Manifest.mbdb: not an iOS backup"
    snapshot = status.get("SnapshotState")
    return BackupInfo(
        path=path,
        manifest=kind,
        encrypted=manifest.get("IsEncrypted") is True,
        device=_text(info.get("Device Name")) or _text(lockdown.get("DeviceName")),
        ios=_text(info.get("Product Version")) or _text(lockdown.get("ProductVersion")),
        date=_iso(info.get("Last Backup Date")) or _iso(manifest.get("Date")),
        has_app=(APP_ID in apps) if isinstance(apps, dict) else None,
        finished=(snapshot == "finished") if isinstance(snapshot, str) else None,
        problem=problem,
    )


def default_backup_roots() -> list[Path]:
    """Where Finder, iTunes and Apple Devices keep backups on this computer."""
    home = Path.home()
    if sys.platform == "darwin":
        return [home / "Library" / "Application Support" / "MobileSync" / "Backup"]
    if sys.platform == "win32":
        roots = [home / "Apple" / "MobileSync" / "Backup"]  # Apple Devices, Store iTunes
        appdata = os.environ.get("APPDATA")
        if appdata:
            roots.append(Path(appdata) / "Apple Computer" / "MobileSync" / "Backup")
        return roots
    return []


def _looks_like_backup(path: Path) -> bool:
    return any((path / n).is_file() for n in ("Manifest.db", "Manifest.mbdb", "Manifest.plist"))


def find_backups(roots: Sequence[Path]) -> list[BackupInfo]:
    """Every backup in ``roots``: a root that is one, or each folder in it that is one."""
    out: list[BackupInfo] = []
    for root in roots:
        try:
            if _looks_like_backup(root):
                candidates = [root]
            elif root.is_dir():
                candidates = sorted(p for p in root.iterdir() if p.is_dir() and not p.is_symlink())
            else:
                continue
        except PermissionError:
            raise IosBackupError(_permission_message(root)) from None
        for p in candidates:
            if not _looks_like_backup(p):
                continue
            try:
                out.append(backup_info(p))
            except IosBackupError as e:
                out.append(BackupInfo(path=p, manifest="", encrypted=False, problem=str(e)))
    return out


# -- Manifest.mbdb ------------------------------------------------------------------------------


@dataclass(frozen=True)
class MbdbRecord:
    """One record of Manifest.mbdb: a file, folder or link of the device."""

    domain: str
    path: str
    link_target: str = ""
    data_hash: bytes = b""  # SHA-1 of the contents (empty for folders)
    encryption_key: bytes = b""  # empty: not encrypted
    mode: int = S_IFREG | 0o644
    inode: int = 0
    uid: int = 501
    gid: int = 501
    mtime: int = 0
    atime: int = 0
    ctime: int = 0
    size: int = 0
    protection: int = 0
    properties: tuple[tuple[str, bytes], ...] = ()

    @property
    def file_id(self) -> str:
        return file_id(self.domain, self.path)

    @property
    def is_file(self) -> bool:
        return self.mode & S_IFMT == S_IFREG


class _Reader:
    def __init__(self, data: bytes) -> None:
        self.data = data
        self.pos = 0

    def take(self, n: int, what: str) -> bytes:
        end = self.pos + n
        if end > len(self.data):
            raise IosBackupError(
                f"Manifest.mbdb is cut short: {what} at byte {self.pos} needs {n} bytes, "
                f"{len(self.data) - self.pos} are left"
            )
        out = self.data[self.pos : end]
        self.pos = end
        return out

    def uint(self, n: int, what: str) -> int:
        return int.from_bytes(self.take(n, what), "big")

    def blob(self, what: str) -> bytes | None:
        n = self.uint(2, f"the length of {what}")
        if n == MBDB_NULL:
            return None
        return self.take(n, what)

    def string(self, what: str) -> str:
        raw = self.blob(what)
        if raw is None:
            return ""
        try:
            return raw.decode("utf-8")
        except UnicodeDecodeError:
            raise IosBackupError(f"Manifest.mbdb: {what} at byte {self.pos} is not UTF-8") from None


def parse_mbdb(data: bytes) -> list[MbdbRecord]:
    """The records of a Manifest.mbdb (docs/formats/ios-backup.md); IosBackupError if broken."""
    r = _Reader(data)
    if r.take(4, "the magic") != MBDB_MAGIC:
        raise IosBackupError("Manifest.mbdb does not start with 'mbdb'")
    version = tuple(r.take(2, "the version"))
    if version not in MBDB_VERSIONS:
        raise IosBackupError(f"Manifest.mbdb has version {version[0]}.{version[1]}, not 5.0 or 4.0")
    out: list[MbdbRecord] = []
    while r.pos < len(data):
        n = len(out)
        domain = r.string(f"record {n}'s domain")
        path = r.string(f"record {n}'s path")
        link = r.string(f"record {n}'s link target")
        data_hash = r.blob(f"record {n}'s data hash") or b""
        key = r.blob(f"record {n}'s encryption key") or b""
        mode = r.uint(2, "mode")
        inode = r.uint(8, "inode")
        uid, gid = r.uint(4, "uid"), r.uint(4, "gid")
        mtime, atime, ctime = (r.uint(4, "a time") for _ in range(3))
        size = r.uint(8, "size")
        protection = r.uint(1, "protection class")
        props = []
        for _ in range(r.uint(1, "property count")):
            name = r.string(f"record {n}'s property name")
            props.append((name, r.blob(f"record {n}'s property {name!r}") or b""))
        out.append(
            MbdbRecord(
                domain=domain,
                path=path,
                link_target=link,
                data_hash=data_hash,
                encryption_key=key,
                mode=mode,
                inode=inode,
                uid=uid,
                gid=gid,
                mtime=mtime,
                atime=atime,
                ctime=ctime,
                size=size,
                protection=protection,
                properties=tuple(props),
            )
        )
    return out


def _put_blob(value: bytes | None) -> bytes:
    if value is None:
        return struct.pack(">H", MBDB_NULL)
    if len(value) >= MBDB_NULL:
        raise ValueError("an mbdb string has at most 65,534 bytes")
    return struct.pack(">H", len(value)) + value


def _put_str(value: str) -> bytes:
    return _put_blob(value.encode() if value else None)


def encode_mbdb(records: Sequence[MbdbRecord], version: tuple[int, int] = (5, 0)) -> bytes:
    """A Manifest.mbdb holding ``records`` (the synthetic backups; parse_mbdb's inverse)."""
    out = bytearray(MBDB_MAGIC + bytes(version))
    for rec in records:
        out += _put_str(rec.domain) + _put_str(rec.path) + _put_str(rec.link_target)
        out += _put_blob(rec.data_hash or None) + _put_blob(rec.encryption_key or None)
        times = (rec.mtime, rec.atime, rec.ctime)
        out += struct.pack(">HQII", rec.mode, rec.inode, rec.uid, rec.gid)
        out += struct.pack(">IIIQBB", *times, rec.size, rec.protection, len(rec.properties))
        for name, value in rec.properties:
            out += _put_str(name) + _put_blob(value)
    return bytes(out)


# -- the game's files -------------------------------------------------------------------------


@dataclass
class AppDocuments:
    """The game's files found in a backup's ``Documents/``, and what was left out."""

    info: BackupInfo
    files: dict[str, bytes] = field(default_factory=dict)  # name -> bytes, game files only
    ignored: list[str] = field(default_factory=list)  # other names in Documents/
    warnings: list[str] = field(default_factory=list)


@dataclass(frozen=True)
class _Entry:
    relative_path: str
    file_id: str
    kind: str  # "file", "dir", "link", "other"
    size: int | None = None  # the manifest's size (mbdb), if it has one
    encrypted: bool = False


def _db_entries(path: Path, domain: str) -> list[_Entry]:
    db = path / "Manifest.db"
    try:
        with open(db, "rb") as f:
            head = f.read(len(SQLITE_MAGIC))
    except PermissionError:
        raise IosBackupError(_permission_message(db)) from None
    if head != SQLITE_MAGIC:
        # Since iOS 10.2 an encrypted backup's Manifest.db is encrypted too.
        raise EncryptedBackupError(
            "Manifest.db is not an SQLite database, so the backup is encrypted or broken. "
            + ENCRYPTED_HOWTO
        )
    uri = db.resolve().as_uri() + "?mode=ro&immutable=1"
    try:
        con = sqlite3.connect(uri, uri=True)
    except sqlite3.Error as e:
        raise IosBackupError(f"Manifest.db cannot be opened: {e}") from None
    try:
        for pragma in ("PRAGMA query_only = ON", "PRAGMA trusted_schema = OFF"):
            try:
                con.execute(pragma)
            except sqlite3.Error:
                pass  # an older SQLite without trusted_schema: the query below is plain
        rows = con.execute(
            "SELECT fileID, relativePath, flags FROM Files WHERE domain = ? LIMIT ?",
            (domain, MAX_APP_ROWS + 1),
        ).fetchall()
    except sqlite3.Error as e:
        raise IosBackupError(f"Manifest.db cannot be read: {e}") from None
    finally:
        con.close()
    if len(rows) > MAX_APP_ROWS:
        raise IosBackupError(f"Manifest.db lists over {MAX_APP_ROWS} files for {domain}")
    kinds = {FLAG_FILE: "file", FLAG_DIRECTORY: "dir", FLAG_SYMLINK: "link"}
    out = []
    for fid, rel, flags in rows:
        if not isinstance(fid, str) or not isinstance(rel, str):
            continue  # not a row iOS writes; nothing to read for it
        kind = kinds.get(flags, "other") if isinstance(flags, int) else "other"
        out.append(_Entry(rel, fid, kind))
    return out


def _mbdb_entries(path: Path, domain: str) -> list[_Entry]:
    records = parse_mbdb(_read_capped(path / "Manifest.mbdb", MAX_MBDB_BYTES, "Manifest.mbdb"))
    kinds = {S_IFREG: "file", S_IFDIR: "dir", S_IFLNK: "link"}
    return [
        _Entry(
            rec.path,
            rec.file_id,
            kinds.get(rec.mode & S_IFMT, "other"),
            rec.size,
            bool(rec.encryption_key),
        )
        for rec in records
        if rec.domain == domain
    ]


def _data_path(path: Path, manifest: str, fid: str) -> Path:
    return path / fid[:2] / fid if manifest == "db" else path / fid


def read_app_documents(path: Path, app_id: str = APP_ID) -> AppDocuments:
    """The game's files in the backup at ``path``. IosBackupError if it cannot be used."""
    if not path.is_dir():
        raise IosBackupError(f"{path} is not a folder")
    if not _looks_like_backup(path):
        found = find_backups([path])
        if found:
            names = "\n".join(f"  {b.path}  ({b.line()})" for b in found)
            raise IosBackupError(
                f"{path} is a folder of backups, not a backup; name one of them:\n{names}"
            )
    info = backup_info(path)
    if info.encrypted:
        raise EncryptedBackupError(ENCRYPTED_HOWTO)
    if info.problem:
        raise IosBackupError(f"{path}: {info.problem}")
    domain = f"AppDomain-{app_id}"
    entries = _db_entries(path, domain) if info.manifest == "db" else _mbdb_entries(path, domain)
    out = AppDocuments(info)
    if info.finished is False:
        out.warnings.append("Status.plist says this backup did not finish")
    seen: set[str] = set()
    for e in entries:
        if not e.relative_path.startswith(DOCUMENTS) or e.kind == "dir":
            continue
        name = e.relative_path[len(DOCUMENTS) :]
        if "/" in name or "\\" in name or name in ("", ".", ".."):
            out.ignored.append(e.relative_path)  # the game keeps no subfolders
            continue
        if not sc.is_game_file(name):
            if not name.endswith(sc.TMP_SUFFIX):
                out.ignored.append(name)
            continue
        if e.kind != "file":
            raise IosBackupError(f"Documents/{name} is a {e.kind} in the backup, not a file")
        if e.encrypted:
            raise EncryptedBackupError(ENCRYPTED_HOWTO)
        if name in seen:
            raise IosBackupError(f"the backup lists Documents/{name} twice")
        seen.add(name)
        if not _FILE_ID.fullmatch(e.file_id):
            raise IosBackupError(f"Documents/{name} has a malformed file ID {e.file_id!r}")
        where = _data_path(path, info.manifest, e.file_id)
        if where.parent != path and where.parent.is_symlink():
            raise IosBackupError(f"{where.parent} is a link; a backup never holds one")
        data = _read_capped(
            where,
            MAX_FILE_BYTES,
            f"Documents/{name}'s data (file {e.file_id})",
        )
        if e.size is not None and e.size != len(data):
            raise IosBackupError(
                f"Documents/{name} has {len(data)} bytes, the manifest says {e.size}"
            )
        out.files[name] = data
    if not out.files:
        if info.has_app is False or not entries:
            raise IosBackupError(
                f"This backup has no files of MapleStory Thief Edition ({domain}). The game "
                "must still be installed on the device when it is backed up."
            )
        raise IosBackupError(f"This backup has no saves of the game in {domain}'s Documents/")
    out.ignored.sort()
    return out


# -- handing the files to the port's import -------------------------------------------------


def check(files: Mapping[str, bytes]) -> list[str]:
    """savecheck's broken rules for the set, one line each (empty: the import takes it)."""
    return [f"{name}: {v.code}: {v.why}" for name, v in sc.check_set(files).items() if not v.ok]


def write_set(files: Mapping[str, bytes], out: Path) -> None:
    """The files as a zip (``out`` ends in .zip; stored, at its root) or a new, empty folder."""
    if out.suffix.lower() == ".zip":
        out.parent.mkdir(parents=True, exist_ok=True)
        tmp = out.with_name(out.name + ".part")
        with zipfile.ZipFile(tmp, "w", zipfile.ZIP_STORED) as z:
            for name, data in sorted(files.items()):
                z.writestr(name, data)
        os.replace(tmp, out)
        return
    if out.exists() and (not out.is_dir() or any(out.iterdir())):
        raise IosBackupError(f"{out} exists and is not an empty folder")
    out.mkdir(parents=True, exist_ok=True)
    for name, data in sorted(files.items()):
        (out / name).write_bytes(data)


def program_command(program: str) -> list[str]:
    """The executable: ``program``, or the one inside it when it is MapleThief.app."""
    p = Path(program)
    if p.suffix == ".app" and p.is_dir():
        return [str(p / "Contents" / "MacOS" / "MapleThief")]
    return [program]


def import_files(
    files: Mapping[str, bytes], program: Sequence[str], save_dir: str | None = None
) -> subprocess.CompletedProcess[str]:
    """``program --import-saves`` on a folder of ``files``: the port's checked import."""
    with tempfile.TemporaryDirectory(prefix="maplethief-iosbackup-") as tmp:
        folder = Path(tmp) / "Documents"
        write_set(files, folder)
        cmd = [*program, "--import-saves", str(folder)]
        if save_dir:
            cmd += ["--save-dir", save_dir]
        return subprocess.run(cmd, capture_output=True, text=True)


# -- synthetic backups (tests) --------------------------------------------------------------

_DATE = dt.datetime(2014, 6, 1, 12, 0, 0)


def _mbfile(size: int, mode: int) -> bytes:
    """A Files.file blob like iOS's: an NSKeyedArchiver plist of an MBFile (never read here)."""
    archive = {
        "$version": 100000,
        "$archiver": "NSKeyedArchiver",
        "$top": {"root": plistlib.UID(1)},
        "$objects": [
            "$null",
            {
                "$class": plistlib.UID(2),
                "Size": size,
                "Mode": mode,
                "UserID": 501,
                "GroupID": 501,
                "LastModified": 1401624000,
                "ProtectionClass": 3,
            },
            {"$classname": "MBFile", "$classes": ["MBFile", "NSObject"]},
        ],
    }
    return plistlib.dumps(archive, fmt=plistlib.FMT_BINARY)


@dataclass(frozen=True)
class SyntheticFile:
    domain: str
    relative_path: str
    data: bytes | None  # None: a folder
    link: bool = False


def synthetic_entries(documents: Mapping[str, bytes], app_id: str = APP_ID) -> list[SyntheticFile]:
    """The game's folders and ``documents``, with files of other domains around them."""
    app = f"AppDomain-{app_id}"
    out = [
        SyntheticFile("HomeDomain", "Library/Preferences", None),
        SyntheticFile("HomeDomain", "Library/Preferences/com.apple.springboard.plist", b"<x/>"),
        SyntheticFile("AppDomain-com.example.other", "Documents", None),
        SyntheticFile("AppDomain-com.example.other", "Documents/theif0", b"not the game's"),
        SyntheticFile(app, "", None),
        SyntheticFile(app, "Documents", None),
        SyntheticFile(app, "Library", None),
        SyntheticFile(app, f"Library/Preferences/{app_id}.plist", b"chartboost"),
    ]
    out += [SyntheticFile(app, DOCUMENTS + n, d) for n, d in sorted(documents.items())]
    return out


def write_synthetic_backup(
    out: Path,
    entries: Sequence[SyntheticFile],
    manifest: str = "db",
    *,
    encrypted: bool = False,
    applications: Sequence[str] = (APP_ID,),
) -> Path:
    """A backup folder like Finder's (``manifest`` "db") or iTunes 9-12's ("mbdb")."""
    out.mkdir(parents=True, exist_ok=True)
    ios = "12.5.7" if manifest == "db" else "6.1.6"
    plist: dict[str, Any] = {
        "Version": "10.0" if manifest == "db" else "9.1",
        "IsEncrypted": encrypted,
        "Date": _DATE,
        "Lockdown": {"DeviceName": "Test iPod", "ProductVersion": ios, "ProductType": "iPod5,1"},
        "Applications": {
            a: {"CFBundleIdentifier": a, "CFBundleVersion": "1.5"} for a in applications
        },
        "WasPasscodeSet": False,
    }
    (out / "Manifest.plist").write_bytes(plistlib.dumps(plist, fmt=plistlib.FMT_BINARY))
    info = {
        "Device Name": "Test iPod",
        "Display Name": "Test iPod",
        "Last Backup Date": _DATE,
        "Product Version": ios,
        "Product Type": "iPod5,1",
        "Target Identifier": "0" * 40,
    }
    (out / "Info.plist").write_bytes(plistlib.dumps(info))
    status = {"IsFullBackup": False, "Version": "3.3", "SnapshotState": "finished", "Date": _DATE}
    (out / "Status.plist").write_bytes(plistlib.dumps(status, fmt=plistlib.FMT_BINARY))

    def scramble(data: bytes) -> bytes:  # stands in for AES: not the plain bytes
        return bytes(b ^ 0x5A for b in data) + bytes(16 - len(data) % 16)

    records = []
    for e in entries:
        fid = file_id(e.domain, e.relative_path)
        if e.link:
            mode = S_IFLNK | 0o755
        else:
            mode = (S_IFDIR | 0o755) if e.data is None else (S_IFREG | 0o644)
        if e.data is not None and not e.link:
            stored = scramble(e.data) if encrypted else e.data
            target = out / fid[:2] / fid if manifest == "db" else out / fid
            target.parent.mkdir(exist_ok=True)
            target.write_bytes(stored)
        records.append((e, fid, mode))
    if manifest == "db":
        db = out / "Manifest.db"
        if db.exists():
            db.unlink()
        con = sqlite3.connect(db)
        con.executescript(
            "CREATE TABLE Files (fileID TEXT PRIMARY KEY, domain TEXT, relativePath TEXT, "
            "flags INTEGER, file BLOB);"
            "CREATE INDEX FilesDomainIdx ON Files(domain);"
            "CREATE INDEX FilesRelativePathIdx ON Files(relativePath);"
            "CREATE INDEX FilesFlagsIdx ON Files(flags);"
            "CREATE TABLE Properties (key TEXT PRIMARY KEY, value BLOB);"
        )
        for e, fid, mode in records:
            flags = FLAG_SYMLINK if e.link else FLAG_DIRECTORY if e.data is None else FLAG_FILE
            size = len(e.data) if e.data is not None else 0
            con.execute(
                "INSERT INTO Files VALUES (?, ?, ?, ?, ?)",
                (fid, e.domain, e.relative_path, flags, _mbfile(size, mode)),
            )
        con.commit()
        con.close()
        if encrypted:  # iOS 10.2+: the database itself is encrypted
            db.write_bytes(scramble(db.read_bytes()))
    else:
        mbdb = [
            MbdbRecord(
                domain=e.domain,
                path=e.relative_path,
                link_target="/var/mobile/x" if e.link else "",
                data_hash=hashlib.sha1(e.data).digest() if e.data is not None else b"",
                encryption_key=bytes(range(44)) if encrypted and e.data is not None else b"",
                mode=mode,
                inode=i + 1,
                mtime=1401624000,
                atime=1401624000,
                ctime=1401624000,
                size=len(e.data) if e.data is not None else 0,
                protection=3 if e.data is not None else 0,
            )
            for i, (e, _, mode) in enumerate(records)
        ]
        (out / "Manifest.mbdb").write_bytes(encode_mbdb(mbdb))
    return out


# -- selftest: both formats through the program's import -------------------------------------


def _env(data_dir: Path) -> dict[str, str]:
    env = dict(os.environ)
    env["MAPLETHIEF_DATA_DIR"] = str(data_dir)  # never the player's own
    env.setdefault("SDL_VIDEO_DRIVER", "dummy")
    return env


def selftest(program: Sequence[str], work: Path) -> list[str]:
    """Synthetic backups of both formats through ``import`` and the program; the problems."""
    problems: list[str] = []
    want = sc.full_set()
    before = {"theif0": b"old character", "config": b"\x03\x01\x00\x01\x01\x01"}
    bad = dict(want, theif1=want["theif1"] + b"\0")
    cases = [
        (fmt, enc, files, expect)
        for fmt in ("db", "mbdb")
        for enc, files, expect in ((False, want, 0), (True, want, 1), (False, bad, 3))
    ]
    for fmt, encrypted, files, expect in cases:
        name = f"{fmt}{'-encrypted' if encrypted else ''}{'-bad' if files is bad else ''}"
        backup = work / "backups" / name
        if backup.exists():
            shutil.rmtree(backup)
        write_synthetic_backup(backup, synthetic_entries(files), fmt, encrypted=encrypted)
        data = work / "data"
        if data.exists():
            shutil.rmtree(data)
        docs = data / "Documents"
        docs.mkdir(parents=True)
        for n, b in before.items():
            (docs / n).write_bytes(b)
        cmd = [sys.executable, "-m", "mtport.iosbackup", "import", str(backup)]
        res = subprocess.run(
            [*cmd, "--program", *program],
            capture_output=True,
            text=True,
            env=_env(data),
            cwd=Path(__file__).resolve().parent.parent,  # tools/, for mtport
        )
        after = {p.name: p.read_bytes() for p in docs.iterdir() if p.is_file()}
        said = f"(exit {res.returncode}) {res.stdout!r} {res.stderr[-400:]!r}"
        if res.returncode != expect:
            problems.append(f"{name}: exit {res.returncode}, expected {expect}: {said}")
            continue
        if expect != 0:
            if after != before:
                problems.append(f"{name}: a refused import changed Documents/ {said}")
            continue
        if after != {**before, **want}:
            diff = sorted(n for n in set(after) | set(want) if after.get(n) != want.get(n))
            problems.append(f"{name}: Documents/ differs from the backup's saves in {diff}")
        imports = data / "backups" / "import"
        backups = sorted(imports.iterdir()) if imports.is_dir() else []
        held = {p.name: p.read_bytes() for p in backups[-1].iterdir()} if backups else {}
        if held != before:
            problems.append(
                f"{name}: the import's backup holds {sorted(held)}, not {sorted(before)}"
            )
    return problems


# -- command line -----------------------------------------------------------------------------

COMMANDS = ("find", "show", "extract", "import", "fixture", "selftest")


def _load(backup: Path) -> AppDocuments | None:
    try:
        docs = read_app_documents(backup)
    except IosBackupError as e:
        print(f"error: {e}", file=sys.stderr)
        return None
    print(docs.info.line())
    for w in docs.warnings:
        print(f"warning: {w}")
    verdicts = sc.check_set(docs.files)
    for name, v in verdicts.items():
        print(f"{name}: {v.code}" + (f": {v.why}" if v.why else ""))
    for name in docs.ignored:
        print(f"ignored: {name}")
    return docs


def main(argv: Sequence[str] | None = None) -> int:
    args_in = list(sys.argv[1:] if argv is None else argv)
    if args_in and args_in[0] not in COMMANDS and not args_in[0].startswith("-"):
        args_in.insert(0, "show")  # python3 -m mtport.iosbackup BACKUP
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.iosbackup", description=__doc__.splitlines()[0]
    )
    sub = parser.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("find", help="list the backups in DIR (default: where Finder keeps them)")
    p.add_argument("dir", nargs="?", type=Path)
    p = sub.add_parser("show", help="the game's files in a backup, each with its check")
    p.add_argument("backup", type=Path)
    p = sub.add_parser("extract", help="write the saves to a new folder or a .zip")
    p.add_argument("backup", type=Path)
    p.add_argument("out", type=Path)
    p = sub.add_parser("import", help="extract and run PROGRAM --import-saves on the saves")
    p.add_argument("backup", type=Path)
    p.add_argument("--program", required=True, nargs="+", help="MapleThief or MapleThief.app")
    p.add_argument("--save-dir", help="passed to the program (its save_dir setting)")
    p = sub.add_parser("fixture", help="write a synthetic backup with savecheck's full set")
    p.add_argument("out", type=Path)
    p.add_argument("--format", choices=("db", "mbdb"), default="db")
    p.add_argument("--encrypted", action="store_true")
    p = sub.add_parser("selftest", help="synthetic backups through PROGRAM's import")
    p.add_argument("--program", required=True, nargs="+", help="the MapleThief executable")
    p.add_argument("--work", type=Path, help="scratch folder (default: a temporary one)")
    args = parser.parse_args(args_in)

    if args.cmd == "find":
        roots = [args.dir] if args.dir else default_backup_roots()
        try:
            found = find_backups(roots)
        except IosBackupError as e:
            print(f"error: {e}", file=sys.stderr)
            return 1
        for b in found:
            print(f"{b.path}\n    {b.line()}")
        if not found:
            where = ", ".join(str(r) for r in roots) or "(no usual place on this system)"
            print(f"no backups in {where}", file=sys.stderr)
            return 1
        return 0
    if args.cmd == "fixture":
        files = sc.full_set()
        write_synthetic_backup(
            args.out, synthetic_entries(files), args.format, encrypted=args.encrypted
        )
        print(f"{args.out}: a synthetic {args.format} backup with {len(files)} save files")
        return 0
    if args.cmd == "selftest":
        with tempfile.TemporaryDirectory(prefix="maplethief-iosbackup-") as tmp:
            problems = selftest(
                program_command(args.program[0]) + args.program[1:], args.work or Path(tmp)
            )
        for line in problems:
            print(line, file=sys.stderr)
        print(f"selftest: 6 backups, {len(problems)} problems")
        return 1 if problems else 0

    docs = _load(args.backup)
    if docs is None:
        return 1
    problems = check(docs.files)
    if problems:
        print(
            "The saves in this backup fail the checks, so the port would refuse them; "
            "nothing was written.",
            file=sys.stderr,
        )
        return 3
    if args.cmd == "show":
        return 0
    if args.cmd == "extract":
        try:
            write_set(docs.files, args.out)
        except (IosBackupError, OSError) as e:
            print(f"error: {e}", file=sys.stderr)
            return 1
        print(
            f"wrote {len(docs.files)} files to {args.out}; "
            f"now: MapleThief --import-saves {args.out}"
        )
        return 0
    program = program_command(args.program[0]) + args.program[1:]
    try:
        res = import_files(docs.files, program, args.save_dir)
    except OSError as e:
        print(f"error: cannot run {program[0]}: {e}", file=sys.stderr)
        return 1
    sys.stdout.write(res.stdout)
    sys.stderr.write(res.stderr)
    return res.returncode


__all__ = [
    "APP_ID",
    "AppDocuments",
    "BackupInfo",
    "EncryptedBackupError",
    "IosBackupError",
    "MbdbRecord",
    "SyntheticFile",
    "backup_info",
    "check",
    "default_backup_roots",
    "encode_mbdb",
    "file_id",
    "find_backups",
    "import_files",
    "parse_mbdb",
    "program_command",
    "read_app_documents",
    "selftest",
    "synthetic_entries",
    "write_set",
    "write_synthetic_backup",
]

if __name__ == "__main__":
    sys.exit(main())
