"""Bootstrap: verify and extract the IPA, split the main binary, describe ``work/``.

The whole bootstrap lives here; ``scripts/bootstrap``, ``scripts/bootstrap.ps1`` and
``scripts/verify-original.sh`` only find a Python 3.10+ and run this module::

    PYTHONPATH=tools python3 -B -m mtport.bootstrap [--repo DIR] [--no-lipo-check]
                                                    [--verify-only | --no-split]

What it does, in order (each stage stops with an ``error:`` line on the first failed check):

1. **verify**: checks that ``original/MapleThief-1.5.ipa`` is not a Git LFS pointer, has
   exactly :data:`IPA_SIZE` bytes and matches ``original/SHA256SUMS``. ``--verify-only``
   stops here and writes nothing.
2. **extract**: extracts the IPA into a temporary directory next to ``work/ipa/``, checks
   ``Payload/MapleThief.app/MapleThief`` against ``original/SHA256SUMS``, then replaces
   ``work/ipa/`` with it. The IPA is untrusted data: every entry name is checked before
   anything is written (no absolute paths, ``..``, backslashes, drive letters, names Windows
   can't store, duplicates or case-only collisions), symbolic links and other non-regular
   entries are rejected, and nothing extracted is ever executed. Files get the zip entry's
   modification time (as ``unzip`` does) and default permissions. ``--no-split`` stops here.
3. **split**: splits the fat binary with :mod:`mtport.fat` (no ``lipo`` needed) and checks
   each slice against its ``work/slices/MapleThief.<arch>`` entry in ``original/SHA256SUMS``.
   On macOS with ``lipo`` on PATH, it cross-checks every slice against ``lipo -thin`` output.
   Only then does it write ``work/slices/MapleThief.armv6``, ``work/slices/MapleThief.armv7``,
   ``work/slices/SHA256SUMS`` and ``work/bootstrap.json``.

Outputs are deterministic (no timestamps, sorted keys), so two runs produce byte-identical
files. ``work/bootstrap.json`` is the contract for other tools. All paths in it are
POSIX-style and relative to the repo root::

    {
      "schema_version": 1,
      "tool": {"name": "mtport.bootstrap", "version": "0.1.0"},
      "ipa":    {"path": "original/MapleThief-1.5.ipa", "size": ..., "sha256": "..."},
      "app_dir": "work/ipa/Payload/MapleThief.app",
      "binary": {"path": "work/ipa/Payload/MapleThief.app/MapleThief",
                 "size": ..., "sha256": "..."},
      "primary_slice": "armv7",
      "slices": {
        "armv6": {"path": "work/slices/MapleThief.armv6", "size": ..., "sha256": "...",
                  "fat_offset": 4096, "align": 12, "cputype": 12, "cpusubtype": 6},
        "armv7": {...}
      }
    }
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import zipfile
from pathlib import Path

from . import __version__, fat

TOOL_NAME = "mtport.bootstrap"
SCHEMA_VERSION = 1

IPA = "original/MapleThief-1.5.ipa"
IPA_SIZE = 17872861  # bytes; the only place the expected size is written down
SUMS = "original/SHA256SUMS"
IPA_DIR = "work/ipa"
APP_DIR = f"{IPA_DIR}/Payload/MapleThief.app"
BINARY = f"{APP_DIR}/MapleThief"
SLICES_DIR = "work/slices"
SLICE_SUMS = f"{SLICES_DIR}/SHA256SUMS"
BOOTSTRAP_JSON = "work/bootstrap.json"
ARCHS = ("armv6", "armv7")
PRIMARY_ARCH = "armv7"

# Stages, in order; main() stops after the one its flags name.
VERIFY, EXTRACT, SPLIT = "verify", "extract", "split"

LFS_HINT = "run 'git lfs install && git lfs pull' first"

# Characters no file system we target can store in a name (Windows is the strictest).
_BAD_NAME_CHARS = re.compile(r'[\x00-\x1f<>:"|?*\\]')
# Windows device names, with or without an extension.
_RESERVED_NAME = re.compile(r"(con|prn|aux|nul|com[0-9]|lpt[0-9])(\..*)?", re.IGNORECASE)


class BootstrapError(Exception):
    pass


def slice_path(arch: str) -> str:
    return f"{SLICES_DIR}/MapleThief.{arch}"


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def read_sums(path: Path) -> dict[str, str]:
    """Parse a ``shasum``/``sha256sum`` style file into ``{path: hex}``."""
    sums: dict[str, str] = {}
    for lineno, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        digest, sep, name = line.partition(" ")
        name = name.lstrip(" ").removeprefix("*")
        if not sep or len(digest) != 64 or not name:
            raise BootstrapError(f"{path}:{lineno}: malformed line")
        sums[name] = digest.lower()
    return sums


def expected(sums: dict[str, str], rel: str) -> str:
    try:
        return sums[rel]
    except KeyError:
        raise BootstrapError(f"no entry for {rel} in {SUMS}") from None


def check_hash(sums: dict[str, str], rel: str, got: str) -> None:
    want = expected(sums, rel)
    if got != want:
        raise BootstrapError(f"{rel}: SHA-256 mismatch (got {got}, want {want})")


def is_lfs_pointer(path: Path) -> bool:
    with open(path, "rb") as f:
        return b"git-lfs.github.com/spec" in f.read(64)


# --- Stage 1: verify -------------------------------------------------------------------------


def load_sums(repo: Path) -> dict[str, str]:
    path = repo / SUMS
    if not path.is_file():
        raise BootstrapError(f"{SUMS} missing")
    return read_sums(path)


def verify_ipa(repo: Path, sums: dict[str, str]) -> tuple[int, str]:
    """Check the IPA; return its size and SHA-256."""
    path = repo / IPA
    if not path.is_file():
        raise BootstrapError(f"{IPA} missing")
    # A clone without Git LFS leaves a ~130-byte pointer file instead of the IPA.
    if is_lfs_pointer(path):
        raise BootstrapError(f"{IPA} is a Git LFS pointer; {LFS_HINT}")
    size = path.stat().st_size
    if size != IPA_SIZE:
        raise BootstrapError(f"{IPA}: size {size}, expected {IPA_SIZE}")
    digest = sha256_file(path)
    check_hash(sums, IPA, digest)
    return size, digest


# --- Stage 2: extract ------------------------------------------------------------------------


def entry_parts(name: str) -> tuple[str, ...]:
    """The path components of a zip entry name; raise if the name is unsafe to extract."""

    def unsafe(why: str) -> BootstrapError:
        return BootstrapError(f"{IPA}: unsafe entry {name!r}: {why}")

    if name.startswith("/"):
        raise unsafe("absolute path")
    parts = name.removesuffix("/").split("/")  # a trailing '/' marks a directory entry
    for part in parts:
        if part in ("", ".", ".."):
            raise unsafe("empty, '.' or '..' path component")
        if _BAD_NAME_CHARS.search(part):
            raise unsafe("backslash, drive letter, control or reserved character")
        if part.endswith((".", " ")) or _RESERVED_NAME.fullmatch(part):
            raise unsafe("name Windows cannot store")
    return tuple(parts)


def safe_members(archive: zipfile.ZipFile) -> list[tuple[zipfile.ZipInfo, tuple[str, ...]]]:
    """Every entry with its path components, after checking the whole archive."""
    members = []
    seen: dict[str, str] = {}  # case-folded path -> entry name
    files: set[str] = set()
    parents: set[str] = set()
    for info in archive.infolist():
        # The name as stored: .filename has '\' turned into '/' on Windows only.
        parts = entry_parts(info.orig_filename)
        mode = stat.S_IFMT(info.external_attr >> 16)  # Unix file type, 0 for MS-DOS entries
        if stat.S_ISLNK(mode):
            raise BootstrapError(f"{IPA}: unsafe entry {info.orig_filename!r}: symbolic link")
        if mode not in (0, stat.S_IFREG, stat.S_IFDIR):
            raise BootstrapError(f"{IPA}: unsafe entry {info.orig_filename!r}: not a regular file")
        if info.flag_bits & 0x1:
            raise BootstrapError(f"{IPA}: entry {info.orig_filename!r} is encrypted")
        key = "/".join(parts).casefold()
        if key in seen:
            raise BootstrapError(
                f"{IPA}: entries {seen[key]!r} and {info.orig_filename!r} name the same file"
                " (on a case-insensitive file system)"
            )
        seen[key] = info.orig_filename
        if not info.is_dir():
            files.add(key)
        parents.update("/".join(parts[:i]).casefold() for i in range(1, len(parts)))
        members.append((info, parts))
    if clash := sorted(files & parents):
        raise BootstrapError(f"{IPA}: {clash[0]!r} is both a file and a directory")
    return members


def _zip_mtime(info: zipfile.ZipInfo) -> float | None:
    """The entry's timestamp as a POSIX time. Zip times are local time, as unzip reads them."""
    try:
        return time.mktime((*info.date_time, 0, 0, -1))
    except (OverflowError, ValueError):
        return None


def _remove_tree(path: Path) -> None:
    if path.is_symlink() or path.is_file():
        path.unlink()
    elif path.exists():
        shutil.rmtree(path)


def extract_ipa(repo: Path, sums: dict[str, str], log=print) -> None:
    """Replace work/ipa/ with the IPA's contents, after checking every entry and the binary."""
    dest = repo / IPA_DIR
    # Not mkdtemp(): its 0700 mode would end up on work/ipa/. A leftover from a killed run
    # with the same pid is removed first.
    tmp = dest.parent / f".ipa-{os.getpid()}.tmp"
    try:
        dest.parent.mkdir(parents=True, exist_ok=True)
        _remove_tree(tmp)
        tmp.mkdir()
        root = os.path.realpath(tmp)
        with zipfile.ZipFile(repo / IPA) as archive:
            for info, parts in safe_members(archive):
                target = tmp.joinpath(*parts)
                # Belt and braces after entry_parts(): the target must stay below tmp.
                if os.path.commonpath([root, os.path.realpath(target)]) != root:
                    raise BootstrapError(f"{IPA}: entry {info.orig_filename!r} escapes {IPA_DIR}")
                if info.is_dir():
                    target.mkdir(parents=True, exist_ok=True)
                    continue
                target.parent.mkdir(parents=True, exist_ok=True)
                # "x": never write through anything that is already there.
                with archive.open(info) as src, open(target, "xb") as dst:
                    shutil.copyfileobj(src, dst, 1 << 20)
                if (mtime := _zip_mtime(info)) is not None:
                    os.utime(target, (mtime, mtime))
        binary = tmp / Path(BINARY).relative_to(IPA_DIR)
        if not binary.is_file():
            raise BootstrapError(f"extraction did not produce {BINARY}")
        check_hash(sums, BINARY, sha256_file(binary))
        _remove_tree(dest)
        os.replace(tmp, dest)
    except (zipfile.BadZipFile, zipfile.LargeZipFile, NotImplementedError, EOFError) as e:
        shutil.rmtree(tmp, ignore_errors=True)
        raise BootstrapError(f"{IPA}: cannot extract: {e}") from None
    except OSError as e:
        shutil.rmtree(tmp, ignore_errors=True)
        raise BootstrapError(f"extracting {IPA} to {IPA_DIR}/ failed: {e}") from None
    except BaseException:
        shutil.rmtree(tmp, ignore_errors=True)
        raise
    log(f"ok  {BINARY}")
    log(f"extracted to {IPA_DIR}/")


# --- Stage 3: split --------------------------------------------------------------------------


def lipo_cross_check(binary: Path, slices: dict[str, bytes]) -> bool:
    """Compare each slice with ``lipo -thin``; return False when lipo is unavailable."""
    lipo = shutil.which("lipo")
    if sys.platform != "darwin" or lipo is None:
        return False
    with tempfile.TemporaryDirectory(prefix="mtport-lipo-") as tmp:
        for arch, data in slices.items():
            out = Path(tmp) / arch
            proc = subprocess.run(
                [lipo, str(binary), "-thin", arch, "-output", str(out)],
                capture_output=True,
                text=True,
            )
            if proc.returncode != 0:
                raise BootstrapError(f"lipo -thin {arch} failed: {proc.stderr.strip()}")
            got, want = sha256_file(out), sha256_bytes(data)
            if got != want:
                raise BootstrapError(f"{arch}: lipo -thin gives {got}, splitter gives {want}")
    return True


def write_atomic(path: Path, data: bytes) -> None:
    """Write via a temp file + rename so an interrupted run never leaves a half file."""
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=path.parent, prefix=f".{path.name}.", suffix=".tmp")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        os.replace(tmp, path)
    except BaseException:
        Path(tmp).unlink(missing_ok=True)
        raise


def split_slices(
    repo: Path, sums: dict[str, str], ipa: tuple[int, str], lipo_check: bool = True, log=print
) -> dict:
    """Split work/ipa's main binary into work/slices/; return the bootstrap.json document."""
    if not (repo / BINARY).is_file():
        raise BootstrapError(f"{BINARY} missing")
    data = (repo / BINARY).read_bytes()
    bin_sha = sha256_bytes(data)
    check_hash(sums, BINARY, bin_sha)
    try:
        parts = fat.split(data)
    except fat.FatError as e:
        raise BootstrapError(f"{BINARY}: {e}") from None

    slices: dict[str, bytes] = {}
    entries: dict[str, dict] = {}
    for arch in ARCHS:
        if arch not in parts:
            raise BootstrapError(f"{BINARY}: no {arch} slice (have: {', '.join(parts)})")
        entry, blob = parts[arch]
        rel = slice_path(arch)
        got = sha256_bytes(blob)
        check_hash(sums, rel, got)
        slices[arch] = blob
        entries[arch] = {
            "path": rel,
            "size": len(blob),
            "sha256": got,
            "fat_offset": entry.offset,
            "align": entry.align,
            "cputype": entry.cputype,
            "cpusubtype": entry.cpusubtype,
        }

    if lipo_check and lipo_cross_check(repo / BINARY, slices):
        log("ok  slices match lipo -thin")

    for arch, blob in slices.items():
        write_atomic(repo / slice_path(arch), blob)
        log(f"ok  {slice_path(arch)}")
    sums_text = "".join(f"{entries[a]['sha256']}  {entries[a]['path']}\n" for a in ARCHS)
    write_atomic(repo / SLICE_SUMS, sums_text.encode("ascii"))

    ipa_size, ipa_sha = ipa
    doc = {
        "schema_version": SCHEMA_VERSION,
        "tool": {"name": TOOL_NAME, "version": __version__},
        "ipa": {"path": IPA, "size": ipa_size, "sha256": ipa_sha},
        "app_dir": APP_DIR,
        "binary": {"path": BINARY, "size": len(data), "sha256": bin_sha},
        "primary_slice": PRIMARY_ARCH,
        "slices": entries,
    }
    write_atomic(
        repo / BOOTSTRAP_JSON, (json.dumps(doc, indent=2, sort_keys=True) + "\n").encode("ascii")
    )
    log(f"wrote {SLICE_SUMS} and {BOOTSTRAP_JSON}")
    return doc


# --- Driver ----------------------------------------------------------------------------------


def bootstrap(repo: Path, lipo_check: bool = True, log=print, until: str = SPLIT) -> dict | None:
    """Run the stages up to and including ``until``; return bootstrap.json's document."""
    if until not in (VERIFY, EXTRACT, SPLIT):
        raise ValueError(f"unknown stage {until!r}")
    sums = load_sums(repo)
    ipa = verify_ipa(repo, sums)
    log(f"ok  {IPA}")
    if until == VERIFY:
        return None
    extract_ipa(repo, sums, log)
    if until == EXTRACT:
        return None
    return split_slices(repo, sums, ipa, lipo_check, log)


def default_repo() -> Path:
    return Path(__file__).resolve().parents[2]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.bootstrap", description=__doc__.splitlines()[0]
    )
    parser.add_argument(
        "--repo", type=Path, default=default_repo(), help="repository root (default: this checkout)"
    )
    parser.add_argument(
        "--no-lipo-check", action="store_true", help="skip the lipo -thin cross-check on macOS"
    )
    stop = parser.add_mutually_exclusive_group()
    stop.add_argument(
        "--verify-only",
        dest="until",
        action="store_const",
        const=VERIFY,
        default=SPLIT,
        help="only check the IPA; write nothing (scripts/verify-original.sh --no-extract)",
    )
    stop.add_argument(
        "--no-split",
        dest="until",
        action="store_const",
        const=EXTRACT,
        help="check and extract the IPA, don't split the slices (scripts/verify-original.sh)",
    )
    args = parser.parse_args(argv)
    try:
        bootstrap(args.repo.resolve(), lipo_check=not args.no_lipo_check, until=args.until)
    except BootstrapError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
