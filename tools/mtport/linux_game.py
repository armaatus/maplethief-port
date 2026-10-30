"""The Linux game folder (#84, ADR 0065): a local, personal-use-only copy of the game.

``scripts/linux-game build`` compiles ``MapleThief`` with the recompiler's generated code
(``linux-release`` preset) and then calls :func:`stage`. That writes, into ``build/linux-game/``
(git-ignored, never uploaded)::

    MapleThief-game-<version>-linux-<arch>/
        MapleThief.sh        the launcher (platform/linux/MapleThief.sh): sets
                             MAPLETHIEF_BUNDLE_DIR to game/, a 1280x800 window under gamescope,
                             --install-desktop writes a .desktop entry
        bin/MapleThief       the executable, linked with the generated code
        game/MapleThief.app/ a copy of your extracted bundle (work/ipa/Payload/MapleThief.app)
        icon.png             the bundle's Icon@2x.png as a standard PNG (mtport.cgbi), for the
                             .desktop entry and Steam's shortcut
        README.txt           how to run it, and that it is for your own machines only
    MapleThief-game-<version>-linux-<arch>.tar.gz   the same folder as one file, to copy to
                                                    another machine (a Steam Deck)

It holds the game, so it is everything a release kit must never be: ``mtport.release guard``
rejects it (tests/test_linux_game.py checks that). The tarball is deterministic: sorted
members, owner root, the modes above, and every timestamp ``SOURCE_DATE_EPOCH`` (else 0).

CLI::

    python3 -m mtport.linux_game stage --program FILE --bundle DIR --version V --out DIR
                                       [--arch ARCH] [--launcher FILE]
    python3 -m mtport.linux_game unpack TARBALL DIR   extract a game tarball (as data: no
                                                      links, no absolute or '..' names)

The bundle is untrusted data: it is copied, never executed; links in it are refused.
"""

from __future__ import annotations

import argparse
import gzip
import io
import os
import re
import shutil
import sys
import tarfile
from pathlib import Path, PurePosixPath

from mtport import cgbi, release

REPO = Path(__file__).resolve().parents[2]
DEFAULT_LAUNCHER = REPO / "platform" / "linux" / "MapleThief.sh"
ICON_CANDIDATES = ("Icon@2x.png", "Icon.png")
ELF_MAGIC = b"\x7fELF"
VERSION = re.compile(r"\d+\.\d+\.\d+(-rc\.\d+)?")
ARCH = re.compile(r"[a-z0-9_]+")

README = """\
MapleThief {version} for Linux ({arch}): your own build
{underline}

This folder was built on your machine from your own copy of MapleStory Thief Edition
(scripts/linux-game, issue #84). It contains the game: keep it to your own machines and
never share or upload it.

Run it:            ./MapleThief.sh
Menu entry:        ./MapleThief.sh --install-desktop
Saves, settings:   ~/.local/share/MapleThief/ (settings.ini: scale_mode, volumes, ...)

Steam Deck: copy the .tar.gz to the Deck, unpack it in Desktop Mode (for example to
~/Games/), run ./MapleThief.sh --install-desktop, then in Steam use "Add a Non-Steam Game"
and pick MapleThief (or browse to MapleThief.sh). Use the "Gamepad" controller layout.
The full notes: docs/linux-and-steam-deck.md in the repository.
"""


class StageError(Exception):
    pass


def folder_name(version: str, arch: str) -> str:
    return f"MapleThief-game-{version}-linux-{arch}"


def _check_program(program: Path) -> None:
    data = program.read_bytes()
    if not data.startswith(ELF_MAGIC):
        raise StageError(f"{program} is not an ELF executable (build it on Linux)")
    if not release.GENERATED_SYMBOL.search(data):
        raise StageError(
            f"{program} is the placeholder shell, not linked with the generated code: run "
            "`python3 -m recomp all`, then configure and build linux-release again"
        )


def _copy_bundle(bundle: Path, dest: Path) -> int:
    """Copy ``bundle`` to ``dest`` (files 0644, directories 0755); refuse links. Returns the
    number of files."""
    if not bundle.is_dir():
        raise StageError(f"{bundle} is not a directory (run scripts/bootstrap)")
    if not (bundle / "MapleThief").is_file():
        raise StageError(f"{bundle} has no MapleThief executable: not the extracted bundle")
    count = 0
    dest.mkdir(parents=True)
    for root, dirs, files in os.walk(bundle):
        rel = Path(root).relative_to(bundle)
        for name in sorted(dirs + files):
            src = Path(root) / name
            if src.is_symlink():
                raise StageError(f"{src} is a link; the bundle must hold only files")
        for name in sorted(dirs):
            (dest / rel / name).mkdir(mode=0o755)
        for name in sorted(files):
            target = dest / rel / name
            shutil.copyfile(Path(root) / name, target)
            target.chmod(0o644)
            count += 1
    return count


def _write_icon(bundle: Path, dest: Path) -> bool:
    for name in ICON_CANDIDATES:
        src = bundle / name
        if src.is_file():
            try:
                dest.write_bytes(cgbi.convert(src.read_bytes(), name))
            except Exception as e:  # a broken icon only loses the icon
                print(f"linux_game: no icon.png ({name}: {e})", file=sys.stderr)
                return False
            dest.chmod(0o644)
            return True
    return False


def _mtime() -> int:
    value = os.environ.get("SOURCE_DATE_EPOCH", "")
    return int(value) if value.isdigit() else 0


def write_tarball(folder: Path, tarball: Path) -> int:
    """Write ``folder`` (with itself as the top-level directory) to ``tarball``,
    deterministically. Returns the number of members."""
    mtime = _mtime()
    paths = [folder] + sorted(folder.rglob("*"), key=lambda p: p.relative_to(folder).as_posix())
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w", format=tarfile.PAX_FORMAT) as tar:
        for p in paths:
            name = (PurePosixPath(folder.name) / p.relative_to(folder).as_posix()).as_posix()
            info = tarfile.TarInfo(name)
            info.uid = info.gid = 0
            info.uname = info.gname = "root"
            info.mtime = mtime
            if p.is_symlink():
                raise StageError(f"{p} is a link")
            if p.is_dir():
                info.type = tarfile.DIRTYPE
                info.mode = 0o755
                tar.addfile(info)
            else:
                info.mode = 0o755 if os.access(p, os.X_OK) else 0o644
                info.size = p.stat().st_size
                with p.open("rb") as f:
                    tar.addfile(info, f)
    with (
        tarball.open("wb") as out,
        gzip.GzipFile(filename="", mode="wb", fileobj=out, mtime=mtime) as gz,
    ):
        gz.write(raw.getvalue())
    return len(paths)


def stage(
    program: Path,
    bundle: Path,
    version: str,
    out: Path,
    arch: str = "x86_64",
    launcher: Path = DEFAULT_LAUNCHER,
) -> tuple[Path, Path]:
    """Write the game folder and its tarball into ``out``; returns both paths. An existing
    folder or tarball of the same name is replaced."""
    if not VERSION.fullmatch(version):
        raise StageError(f"version {version!r} is not X.Y.Z or X.Y.Z-rc.N")
    if not ARCH.fullmatch(arch):
        raise StageError(f"bad architecture {arch!r}")
    _check_program(program)
    if not launcher.is_file():
        raise StageError(f"no launcher {launcher}")
    name = folder_name(version, arch)
    out.mkdir(parents=True, exist_ok=True)
    folder = out / name
    tarball = out / f"{name}.tar.gz"
    if folder.exists():
        shutil.rmtree(folder)
    tarball.unlink(missing_ok=True)
    folder.mkdir()
    (folder / "bin").mkdir()
    shutil.copyfile(program, folder / "bin" / "MapleThief")
    (folder / "bin" / "MapleThief").chmod(0o755)
    shutil.copyfile(launcher, folder / "MapleThief.sh")
    (folder / "MapleThief.sh").chmod(0o755)
    files = _copy_bundle(bundle, folder / "game" / "MapleThief.app")
    icon = _write_icon(bundle, folder / "icon.png")
    title = f"MapleThief {version} for Linux ({arch}): your own build"
    (folder / "README.txt").write_text(
        README.format(version=version, arch=arch, underline="=" * len(title)), encoding="utf-8"
    )
    (folder / "README.txt").chmod(0o644)
    members = write_tarball(folder, tarball)
    print(
        f"linux_game: {folder} ({files} bundle files{', icon.png' if icon else ', no icon'}); "
        f"{tarball} ({members} members, {tarball.stat().st_size} bytes)"
    )
    return folder, tarball


def unpack(tarball: Path, dest: Path) -> Path:
    """Extract a game tarball into ``dest`` (emptied first). Only directories and regular
    files with safe names are accepted; modes keep their executable bits. Returns the
    top-level directory."""
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    top = None
    with tarfile.open(tarball, "r:gz") as tar:
        for info in tar:
            parts = PurePosixPath(info.name).parts
            if not parts or info.name.startswith("/") or ".." in parts:
                raise StageError(f"{tarball}: unsafe member name {info.name!r}")
            if top is None:
                top = parts[0]
            elif parts[0] != top:
                raise StageError(f"{tarball}: more than one top-level directory")
            target = dest.joinpath(*parts)
            if info.isdir():
                target.mkdir(parents=True, exist_ok=True)
                target.chmod(0o755)
            elif info.isfile():
                target.parent.mkdir(parents=True, exist_ok=True)
                src = tar.extractfile(info)
                assert src is not None
                with target.open("wb") as f:
                    shutil.copyfileobj(src, f)
                target.chmod(0o755 if info.mode & 0o100 else 0o644)
            else:
                raise StageError(f"{tarball}: {info.name} is not a file or directory")
    if top is None:
        raise StageError(f"{tarball} is empty")
    return dest / top


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python3 -m mtport.linux_game", description=__doc__)
    sub = parser.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("stage", help="write the game folder and tarball")
    p.add_argument("--program", type=Path, required=True)
    p.add_argument("--bundle", type=Path, required=True)
    p.add_argument("--version", required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--arch", default="x86_64")
    p.add_argument("--launcher", type=Path, default=DEFAULT_LAUNCHER)
    u = sub.add_parser("unpack", help="extract a game tarball")
    u.add_argument("tarball", type=Path)
    u.add_argument("dest", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.cmd == "stage":
            stage(args.program, args.bundle, args.version, args.out, args.arch, args.launcher)
        else:
            print(unpack(args.tarball, args.dest))
    except (StageError, OSError, tarfile.TarError) as e:
        print(f"linux_game: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
