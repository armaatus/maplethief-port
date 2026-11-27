"""The Linux player AppImage and Flatpak bundle (#349, ADR 0193), made from the player tarball.

``scripts/linux-game player`` builds the player tarball (ADR 0125) in the portable build image
and then calls this module there, which repacks the tarball's files twice:

* ``appimage``: an AppDir (the tarball's folder, plus ``AppRun``, a ``.desktop`` entry and the
  icon under the entry's name) packed by ``mksquashfs`` (zlib, owner root, every time
  ``SOURCE_DATE_EPOCH`` or 0) and appended to the pinned type-2 runtime of the image
  (``/opt/appimage/runtime``). No appimagetool: the image is the runtime's bytes followed by the
  SquashFS image, which is all a type-2 AppImage is. The game's data folder is next to the
  ``.AppImage`` file (the runtime sets ``APPIMAGE``; platform/src/data_location.cpp).
* ``flatpak``: a Flatpak build directory (``metadata`` against the Freedesktop runtime, the
  tarball's files under ``files/``, the menu entry and icon under ``files/share`` and
  ``export/share``), committed to a scratch OSTree repository by ``flatpak build-export`` and
  written as a single-file bundle by ``flatpak build-bundle``, which names Flathub as the
  runtime's source. The data folder is the sandbox's per-user folder
  (``~/.var/app/io.github.armaatus.maplethief/data/MapleThief``).

Both hold exactly the tarball's files (none of the game's): ``mtport.release guard --mode
player`` looks inside them (mtport.squashfs, mtport.flatpak_bundle).

CLI::

    python3 -m mtport.linux_package appimage TARBALL --runtime FILE [--out DIR]
    python3 -m mtport.linux_package flatpak TARBALL [--out DIR] [--branch B]
    python3 -m mtport.linux_package names TARBALL     the AppImage's and the bundle's names
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from mtport import linux_game

APP_ID = "io.github.armaatus.maplethief"
TARBALL = re.compile(r"(MapleThief-\d+\.\d+\.\d+(?:-rc\.\d+)?-linux-(x86_64|aarch64))\.tar\.gz")
# The Freedesktop runtime the Flatpak runs on: 25.08 is supported until 2027-08.
FLATPAK_RUNTIME = "org.freedesktop.Platform"
FLATPAK_SDK = "org.freedesktop.Sdk"
FLATPAK_RUNTIME_BRANCH = "25.08"
FLATPAK_BRANCH = "stable"
FLATHUB_REPO = "https://dl.flathub.org/repo/flathub.flatpakrepo"
ICON_DIR = "share/icons/hicolor/256x256/apps"
FLATPAK_ARCH = {"x86_64": "x86_64", "aarch64": "aarch64"}
VALIDATE_ICON = "/usr/libexec/flatpak-validate-icon"

DESKTOP = """\
[Desktop Entry]
Type=Application
Name=MapleThief
Comment=MapleStory Thief Edition, played from your own copy
Exec={exec}
Icon={icon}
Terminal=false
Categories=Game;
"""

# The Flatpak's permissions: a window (X11 or Wayland, and the GPU), sound, gamepads (all
# devices: Flatpak's "input" needs 1.15), and Downloads read-only for the IPA when the file
# dialog portal is missing. No network: the game has no online part.
FLATPAK_METADATA = """\
[Application]
name={app_id}
runtime={runtime}/{arch}/{branch}
sdk={sdk}/{arch}/{branch}
command=/app/MapleThief.sh

[Context]
shared=ipc;
sockets=x11;wayland;pulseaudio;
devices=all;
filesystems=xdg-download:ro;
"""


class PackageError(Exception):
    pass


def names(tarball: Path) -> tuple[str, str, str]:
    """(folder name, AppImage file name, Flatpak bundle file name) of a player tarball."""
    m = TARBALL.fullmatch(tarball.name)
    if not m:
        raise PackageError(f"{tarball.name} is not MapleThief-<version>-linux-<arch>.tar.gz")
    stem = m.group(1)
    return stem, f"{stem}.AppImage", f"{stem}.flatpak"


def _arch(tarball: Path) -> str:
    m = TARBALL.fullmatch(tarball.name)
    assert m
    return m.group(2)


def _unpack(tarball: Path, dest: Path) -> Path:
    try:
        top = linux_game.unpack(tarball, dest)
    except linux_game.StageError as e:
        raise PackageError(str(e)) from e
    for need in ("bin/MapleThief", "MapleThief.sh", "icon.png"):
        if not (top / need).is_file():
            raise PackageError(f"{tarball.name} has no {need}: not a Linux player tarball")
    return top


def _normalise(root: Path, mtime: int) -> None:
    """Fixed modes and times below ``root``, so that the packed bytes depend on the files
    only."""
    for p in sorted(root.rglob("*"), reverse=True):
        if p.is_symlink():
            os.utime(p, (mtime, mtime), follow_symlinks=False)
            continue
        p.chmod(0o755 if p.is_dir() or p.stat().st_mode & 0o100 else 0o644)
        os.utime(p, (mtime, mtime))
    os.utime(root, (mtime, mtime))


def appdir(tarball: Path, dest: Path) -> Path:
    """The AppDir of the player tarball in ``dest`` (emptied first)."""
    top = _unpack(tarball, dest)
    (top / "AppRun").symlink_to("MapleThief.sh")
    desktop = DESKTOP.format(exec="MapleThief", icon=APP_ID)
    (top / f"{APP_ID}.desktop").write_text(desktop, encoding="utf-8")
    shutil.copyfile(top / "icon.png", top / f"{APP_ID}.png")
    (top / ".DirIcon").symlink_to("icon.png")
    _normalise(top, linux_game._mtime())
    return top


def appimage(tarball: Path, runtime: Path, out: Path, mksquashfs: str = "mksquashfs") -> Path:
    """Write the AppImage of the player tarball into ``out``; returns its path."""
    _, name, _ = names(tarball)
    head = runtime.read_bytes()
    if head[:4] != b"\x7fELF" or head[8:11] != b"AI\x02":
        raise PackageError(f"{runtime} is not a type-2 AppImage runtime")
    out.mkdir(parents=True, exist_ok=True)
    target = out / name
    with tempfile.TemporaryDirectory(prefix="appdir-") as tmp:
        top = appdir(tarball, Path(tmp) / "unpacked")
        image = Path(tmp) / "image.squashfs"
        mtime = str(linux_game._mtime())
        cmd = [mksquashfs, str(top), str(image), "-comp", "gzip", "-noappend", "-all-root"]
        cmd += ["-no-xattrs", "-no-progress", "-quiet", "-mkfs-time", mtime, "-all-time", mtime]
        subprocess.run(cmd, check=True)
        with target.open("wb") as f:
            f.write(head)
            f.write(image.read_bytes())
    target.chmod(0o755)
    print(f"linux_package: {target} ({target.stat().st_size} bytes)")
    return target


def flatpak_dir(tarball: Path, dest: Path, branch: str = FLATPAK_RUNTIME_BRANCH) -> Path:
    """The Flatpak build directory of the player tarball in ``dest`` (emptied first)."""
    arch = FLATPAK_ARCH[_arch(tarball)]
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    top = _unpack(tarball, dest / "unpacked")
    files = dest / "files"
    top.rename(files)
    (dest / "unpacked").rmdir()
    desktop = DESKTOP.format(exec="/app/MapleThief.sh", icon=APP_ID)
    for root in (files / "share", dest / "export" / "share"):
        (root / "applications").mkdir(parents=True)
        (root / "applications" / f"{APP_ID}.desktop").write_text(desktop, encoding="utf-8")
        icon_dir = root / ICON_DIR.removeprefix("share/")
        icon_dir.mkdir(parents=True)
        shutil.copyfile(files / "icon.png", icon_dir / f"{APP_ID}.png")
    metadata = FLATPAK_METADATA.format(
        app_id=APP_ID, runtime=FLATPAK_RUNTIME, sdk=FLATPAK_SDK, arch=arch, branch=branch
    )
    (dest / "metadata").write_text(metadata, encoding="utf-8")
    _normalise(dest, linux_game._mtime())
    return dest


def flatpak(
    tarball: Path, out: Path, branch: str = FLATPAK_RUNTIME_BRANCH, tool: str = "flatpak"
) -> Path:
    """Write the Flatpak bundle of the player tarball into ``out``; returns its path."""
    _, _, name = names(tarball)
    arch = FLATPAK_ARCH[_arch(tarball)]
    out.mkdir(parents=True, exist_ok=True)
    target = out / name
    target.unlink(missing_ok=True)
    with tempfile.TemporaryDirectory(prefix="flatpak-") as tmp:
        build = flatpak_dir(tarball, Path(tmp) / "build", branch)
        repo = Path(tmp) / "repo"
        stamp = str(linux_game._mtime())
        env = dict(os.environ)
        validator = Path(VALIDATE_ICON)
        if validator.is_file():
            # build-export checks the exported icon with flatpak-validate-icon in a bwrap
            # sandbox, which an unprivileged container can't create; the icon is our own
            # (platform/icon/icon.png), so it is checked without the sandbox.
            wrapper = Path(tmp) / "validate-icon"
            wrapper.write_text(
                f'#!/bin/sh\n[ "$1" = --sandbox ] && shift\nexec {validator} "$@"\n',
                encoding="utf-8",
            )
            wrapper.chmod(0o755)
            env["FLATPAK_VALIDATE_ICON"] = str(wrapper)
        subprocess.run(
            [tool, "build-export", f"--arch={arch}", f"--timestamp={stamp}", "--no-update-summary",
             f"--subject=MapleThief {names(tarball)[0]}", str(repo), str(build), FLATPAK_BRANCH],
            check=True,
            env=env,
        )  # fmt: skip
        subprocess.run(
            [tool, "build-bundle", f"--arch={arch}", f"--runtime-repo={FLATHUB_REPO}",
             str(repo), str(target), APP_ID, FLATPAK_BRANCH],
            check=True,
        )  # fmt: skip
    print(f"linux_package: {target} ({target.stat().st_size} bytes)")
    return target


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.linux_package", description=__doc__.split("\n")[0]
    )
    sub = parser.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("appimage", help="write the AppImage of a player tarball")
    a.add_argument("tarball", type=Path)
    a.add_argument("--runtime", type=Path, required=True)
    a.add_argument("--out", type=Path)
    f = sub.add_parser("flatpak", help="write the Flatpak bundle of a player tarball")
    f.add_argument("tarball", type=Path)
    f.add_argument("--out", type=Path)
    f.add_argument("--branch", default=FLATPAK_RUNTIME_BRANCH, help="the runtime's branch")
    n = sub.add_parser("names", help="the AppImage's and the bundle's file names")
    n.add_argument("tarball", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.cmd == "names":
            print("\n".join(names(args.tarball)[1:]))
        elif args.cmd == "appimage":
            appimage(args.tarball, args.runtime, args.out or args.tarball.parent)
        else:
            flatpak(args.tarball, args.out or args.tarball.parent, args.branch)
    except (PackageError, OSError, subprocess.CalledProcessError) as e:
        print(f"linux_package: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
