"""Bootstrap step 2: split the extracted main binary and describe ``work/``.

Runs after ``scripts/verify-original.sh`` (or its PowerShell equivalent in
``scripts/bootstrap.ps1``) has extracted the IPA to ``work/ipa/``. Normally
invoked through ``scripts/bootstrap``; it can be re-run on its own::

    PYTHONPATH=tools python3 -m mtport.bootstrap [--repo DIR] [--no-lipo-check]

What it does, in order (nothing is written unless every check passes):

1. Re-checks the IPA and ``work/ipa/Payload/MapleThief.app/MapleThief``
   against ``original/SHA256SUMS``.
2. Splits the fat binary with :mod:`mtport.fat` (no ``lipo`` needed) and
   checks each slice against its ``work/slices/MapleThief.<arch>`` entry in
   ``original/SHA256SUMS``.
3. On macOS with ``lipo`` on PATH, cross-checks every slice against
   ``lipo -thin`` output.
4. Writes ``work/slices/MapleThief.armv6``, ``work/slices/MapleThief.armv7``,
   ``work/slices/SHA256SUMS`` and ``work/bootstrap.json``.

Outputs are deterministic (no timestamps, sorted keys), so two runs produce
byte-identical files. ``work/bootstrap.json`` is the contract for other tools.
All paths in it are POSIX-style and relative to the repo root::

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
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from . import __version__, fat

TOOL_NAME = "mtport.bootstrap"
SCHEMA_VERSION = 1

IPA = "original/MapleThief-1.5.ipa"
SUMS = "original/SHA256SUMS"
APP_DIR = "work/ipa/Payload/MapleThief.app"
BINARY = f"{APP_DIR}/MapleThief"
SLICES_DIR = "work/slices"
SLICE_SUMS = f"{SLICES_DIR}/SHA256SUMS"
BOOTSTRAP_JSON = "work/bootstrap.json"
ARCHS = ("armv6", "armv7")
PRIMARY_ARCH = "armv7"

LFS_HINT = "run 'git lfs install && git lfs pull' first"


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


def is_lfs_pointer(path: Path) -> bool:
    with open(path, "rb") as f:
        return b"git-lfs.github.com/spec" in f.read(64)


def check_file(repo: Path, sums: dict[str, str], rel: str) -> tuple[int, str]:
    path = repo / rel
    if not path.is_file():
        raise BootstrapError(f"{rel} missing")
    got = sha256_file(path)
    want = expected(sums, rel)
    if got != want:
        raise BootstrapError(f"{rel}: SHA-256 mismatch (got {got}, want {want})")
    return path.stat().st_size, got


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


def bootstrap(repo: Path, lipo_check: bool = True, log=print) -> dict:
    """Run the split step against ``repo``; return the bootstrap.json document."""
    sums_path = repo / SUMS
    if not sums_path.is_file():
        raise BootstrapError(f"{SUMS} missing")
    sums = read_sums(sums_path)

    ipa_path = repo / IPA
    if ipa_path.is_file() and is_lfs_pointer(ipa_path):
        raise BootstrapError(f"{IPA} is a Git LFS pointer; {LFS_HINT}")
    ipa_size, ipa_sha = check_file(repo, sums, IPA)
    log(f"ok  {IPA}")
    if not (repo / BINARY).is_file():
        raise BootstrapError(
            f"{BINARY} missing; extract the IPA first (scripts/bootstrap does this)"
        )
    bin_size, bin_sha = check_file(repo, sums, BINARY)
    log(f"ok  {BINARY}")

    data = (repo / BINARY).read_bytes()
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
        got, want = sha256_bytes(blob), expected(sums, rel)
        if got != want:
            raise BootstrapError(f"{rel}: SHA-256 mismatch (got {got}, want {want})")
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

    doc = {
        "schema_version": SCHEMA_VERSION,
        "tool": {"name": TOOL_NAME, "version": __version__},
        "ipa": {"path": IPA, "size": ipa_size, "sha256": ipa_sha},
        "app_dir": APP_DIR,
        "binary": {"path": BINARY, "size": bin_size, "sha256": bin_sha},
        "primary_slice": PRIMARY_ARCH,
        "slices": entries,
    }
    write_atomic(
        repo / BOOTSTRAP_JSON, (json.dumps(doc, indent=2, sort_keys=True) + "\n").encode("ascii")
    )
    log(f"wrote {SLICE_SUMS} and {BOOTSTRAP_JSON}")
    return doc


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
    args = parser.parse_args(argv)
    try:
        bootstrap(args.repo.resolve(), lipo_check=not args.no_lipo_check)
    except BootstrapError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
