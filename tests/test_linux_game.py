"""The Linux game folder (mtport.linux_game, scripts/linux-game, platform/linux/MapleThief.sh;
#84, ADR 0065), on synthetic inputs: a fake ELF "linked with the generated code" and a fake
bundle. Nothing here is derived from the IPA.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import tarfile
from pathlib import Path

import pytest

from mtport import linux_game as lg
from mtport import png
from mtport import release as rel
from pngfixtures import make_cgbi, random_rgba
from repo_paths import REPO

needs_sh = pytest.mark.skipif(os.name == "nt" or not shutil.which("sh"), reason="needs sh")
needs_bash = pytest.mark.skipif(os.name == "nt" or not shutil.which("bash"), reason="needs bash")

GAME_PROGRAM = b"\x7fELF\x02\x01\x01\0" + b"\0" * 56 + b"f_0002A3C0\0mt_fn_first\0"
SHELL_PROGRAM = b"\x7fELF\x02\x01\x01\0" + b"\0" * 56 + b"mt_fn_table\0MapleThief 0.0.1\0"


def make_bundle(root: Path, icon: bytes | None = None) -> Path:
    bundle = root / "Payload" / "MapleThief.app"
    (bundle / "English.lproj").mkdir(parents=True)
    (bundle / "MapleThief").write_bytes(b"\xca\xfe\xba\xbe fat Mach-O stand-in")
    (bundle / "Info.plist").write_bytes(b"<plist/>")
    (bundle / "English.lproj" / "Strings.strings").write_bytes(b"x")
    (bundle / "map").mkdir()
    (bundle / "map" / "m0.map").write_bytes(b"\x01\x02")
    if icon is not None:
        (bundle / "Icon@2x.png").write_bytes(icon)
    return bundle


def make_program(root: Path, data: bytes = GAME_PROGRAM) -> Path:
    p = root / "MapleThief"
    p.write_bytes(data)
    return p


@pytest.fixture
def staged(tmp_path):
    icon = make_cgbi(4, 3, random_rgba(4, 3, seed=7))
    bundle = make_bundle(tmp_path / "in", icon)
    program = make_program(tmp_path)
    folder, tarball = lg.stage(program, bundle, "1.2.3-rc.4", tmp_path / "out")
    return folder, tarball, tmp_path


def test_stage_layout(staged):
    folder, tarball, _ = staged
    assert folder.name == "MapleThief-game-1.2.3-rc.4-linux-x86_64"
    assert tarball.name == folder.name + ".tar.gz"
    assert (folder / "bin" / "MapleThief").read_bytes() == GAME_PROGRAM
    assert (folder / "MapleThief.sh").read_bytes() == lg.DEFAULT_LAUNCHER.read_bytes()
    app = folder / "game" / "MapleThief.app"
    assert (app / "map" / "m0.map").read_bytes() == b"\x01\x02"
    assert (app / "English.lproj" / "Strings.strings").is_file()
    readme = (folder / "README.txt").read_text()
    assert "never share or upload it" in readme
    assert "1.2.3-rc.4" in readme
    if os.name != "nt":
        assert os.access(folder / "MapleThief.sh", os.X_OK)
        assert os.access(folder / "bin" / "MapleThief", os.X_OK)
        assert not os.access(app / "MapleThief", os.X_OK)


def test_icon_is_a_standard_png(staged):
    folder, _, _ = staged
    data = (folder / "icon.png").read_bytes()
    image = png.decode(data)
    assert (image.width, image.height) == (4, 3)
    assert b"CgBI" not in data


def test_tarball_members_and_modes(staged):
    folder, tarball, _ = staged
    with tarfile.open(tarball) as t:
        infos = {i.name: i for i in t.getmembers()}
    top = folder.name
    assert top in infos and infos[top].isdir()
    assert all(name == top or name.startswith(top + "/") for name in infos)
    assert infos[f"{top}/MapleThief.sh"].mode == 0o755
    assert infos[f"{top}/bin/MapleThief"].mode == 0o755
    assert infos[f"{top}/game/MapleThief.app/MapleThief"].mode == 0o644
    assert infos[f"{top}/game/MapleThief.app"].mode == 0o755
    for info in infos.values():
        assert (info.uid, info.gid, info.uname, info.gname) == (0, 0, "root", "root")
        assert info.mtime == 0
        assert info.isdir() or info.isfile()
    assert sorted(infos) == list(infos)


def test_tarball_is_deterministic(tmp_path, monkeypatch):
    monkeypatch.delenv("SOURCE_DATE_EPOCH", raising=False)
    bundle = make_bundle(tmp_path / "in")
    program = make_program(tmp_path)
    _, first = lg.stage(program, bundle, "0.0.1", tmp_path / "a")
    os.utime(bundle / "Info.plist", (1_000_000, 1_000_000))
    _, second = lg.stage(program, bundle, "0.0.1", tmp_path / "b")
    assert first.read_bytes() == second.read_bytes()
    monkeypatch.setenv("SOURCE_DATE_EPOCH", "1700000000")
    _, third = lg.stage(program, bundle, "0.0.1", tmp_path / "c")
    with tarfile.open(third) as t:
        assert {i.mtime for i in t.getmembers()} == {1700000000}


def test_restage_replaces(tmp_path):
    bundle = make_bundle(tmp_path / "in")
    program = make_program(tmp_path)
    folder, _ = lg.stage(program, bundle, "0.0.1", tmp_path / "out")
    (folder / "stale.txt").write_text("old")
    folder, tarball = lg.stage(program, bundle, "0.0.1", tmp_path / "out")
    assert not (folder / "stale.txt").exists()
    with tarfile.open(tarball) as t:
        assert not any(n.endswith("stale.txt") for n in t.getnames())


def test_no_icon_without_one(tmp_path):
    folder, _ = lg.stage(make_program(tmp_path), make_bundle(tmp_path / "in"), "0.0.1", tmp_path)
    assert not (folder / "icon.png").exists()


@pytest.mark.parametrize(
    "program, message",
    [
        (b"#!/bin/sh\necho hi\n", "not an ELF executable"),
        (SHELL_PROGRAM, "placeholder shell"),
    ],
)
def test_stage_rejects_program(tmp_path, program, message):
    with pytest.raises(lg.StageError, match=message):
        lg.stage(make_program(tmp_path, program), make_bundle(tmp_path / "in"), "0.0.1", tmp_path)


def test_stage_rejects_bad_inputs(tmp_path):
    program = make_program(tmp_path)
    bundle = make_bundle(tmp_path / "in")
    with pytest.raises(lg.StageError, match="not X.Y.Z"):
        lg.stage(program, bundle, "1.0", tmp_path / "out")
    with pytest.raises(lg.StageError, match="architecture"):
        lg.stage(program, bundle, "1.0.0", tmp_path / "out", arch="x86/64")
    with pytest.raises(lg.StageError, match="not a directory"):
        lg.stage(program, tmp_path / "missing", "1.0.0", tmp_path / "out")
    (bundle / "MapleThief").unlink()
    with pytest.raises(lg.StageError, match="no MapleThief executable"):
        lg.stage(program, bundle, "1.0.0", tmp_path / "out")


@pytest.mark.skipif(os.name == "nt", reason="symbolic links need privileges on Windows")
def test_stage_rejects_links_in_bundle(tmp_path):
    bundle = make_bundle(tmp_path / "in")
    (bundle / "link").symlink_to("/etc/passwd")
    with pytest.raises(lg.StageError, match="is a link"):
        lg.stage(make_program(tmp_path), bundle, "1.0.0", tmp_path / "out")


def test_release_guard_rejects_the_game_folder(staged):
    """The game folder is the opposite of a release kit: the guard must refuse it, both the
    tarball and the folder, for the generated code in bin/MapleThief alone."""
    folder, tarball, _ = staged
    for kit in (tarball, folder):
        report = rel.guard(kit, {}, [])
        assert not report.ok
        found = {(Path(f.member).name, f.rule) for f in report.findings}
        assert ("MapleThief", "symbols") in found
        assert ("icon.png", "derived") in found


def test_unpack_round_trip(staged):
    folder, tarball, tmp_path = staged
    top = lg.unpack(tarball, tmp_path / "unpacked")
    assert top.name == folder.name
    before = sorted(p.relative_to(folder).as_posix() for p in folder.rglob("*"))
    after = sorted(p.relative_to(top).as_posix() for p in top.rglob("*"))
    assert before == after
    if os.name != "nt":
        assert os.access(top / "MapleThief.sh", os.X_OK)
        assert not os.access(top / "README.txt", os.X_OK)


@pytest.mark.parametrize(
    "name, kind, message",
    [
        ("../evil", tarfile.REGTYPE, "unsafe member name"),
        ("/abs", tarfile.REGTYPE, "unsafe member name"),
        ("top/link", tarfile.SYMTYPE, "not a file or directory"),
    ],
)
def test_unpack_rejects(tmp_path, name, kind, message):
    tarball = tmp_path / "bad.tar.gz"
    with tarfile.open(tarball, "w:gz") as t:
        info = tarfile.TarInfo("top")
        info.type = tarfile.DIRTYPE
        t.addfile(info)
        info = tarfile.TarInfo(name)
        info.type = kind
        info.linkname = "/etc/passwd" if kind == tarfile.SYMTYPE else ""
        t.addfile(info)
    with pytest.raises(lg.StageError, match=message):
        lg.unpack(tarball, tmp_path / "out")


def test_cli_stage_errors(tmp_path, capsys):
    status = lg.main(
        [
            "stage",
            "--program",
            str(make_program(tmp_path, SHELL_PROGRAM)),
            "--bundle",
            str(make_bundle(tmp_path / "in")),
            "--version",
            "0.0.1",
            "--out",
            str(tmp_path / "out"),
        ]
    )
    assert status == 1
    assert "placeholder shell" in capsys.readouterr().err


# --- The launcher, with a stand-in bin/MapleThief that prints its arguments and bundle -------


@pytest.fixture
def launcher_folder(tmp_path):
    folder = tmp_path / "My Games" / "MapleThief-game-0.0.1-linux-x86_64"
    (folder / "bin").mkdir(parents=True)
    shutil.copyfile(lg.DEFAULT_LAUNCHER, folder / "MapleThief.sh")
    (folder / "MapleThief.sh").chmod(0o755)
    fake = folder / "bin" / "MapleThief"
    fake.write_text('#!/bin/sh\necho "bundle=$MAPLETHIEF_BUNDLE_DIR"\necho "args=$*"\n')
    fake.chmod(0o755)
    (folder / "icon.png").write_bytes(b"png")
    return folder


def launch(folder: Path, *args: str, **env: str) -> subprocess.CompletedProcess:
    base = {k: v for k, v in os.environ.items() if not k.startswith(("GAMESCOPE", "MAPLETHIEF"))}
    base.pop("XDG_CURRENT_DESKTOP", None)
    base.update(env)
    return subprocess.run(
        ["sh", str(folder / "MapleThief.sh"), *args],
        capture_output=True,
        text=True,
        env=base,
        timeout=30,
    )


@needs_sh
def test_launcher_desktop(launcher_folder):
    out = launch(launcher_folder, "--frames", "5")
    assert out.returncode == 0, out.stderr
    real = launcher_folder.resolve()
    assert f"bundle={real}/game/MapleThief.app" in out.stdout
    assert "args=--frames 5" in out.stdout
    assert "gamescope" not in out.stderr


@needs_sh
@pytest.mark.parametrize(
    "env", [{"GAMESCOPE_WAYLAND_DISPLAY": "gamescope-0"}, {"XDG_CURRENT_DESKTOP": "gamescope"}]
)
def test_launcher_gamescope_window(launcher_folder, env):
    out = launch(launcher_folder, "--scale-mode", "sharp", **env)
    assert out.returncode == 0, out.stderr
    assert "args=--window 1280x800 --scale-mode sharp" in out.stdout
    assert "gamescope: --window 1280x800" in out.stderr
    out = launch(launcher_folder, MAPLETHIEF_GAMESCOPE_WINDOW="1920x1080", **env)
    assert "args=--window 1920x1080" in out.stdout
    out = launch(launcher_folder, "--window", "640x400", **env)
    assert "args=--window 640x400" in out.stdout


@needs_sh
def test_launcher_keeps_bundle_from_environment(launcher_folder):
    out = launch(launcher_folder, MAPLETHIEF_BUNDLE_DIR="/elsewhere/MapleThief.app")
    assert "bundle=/elsewhere/MapleThief.app" in out.stdout


@needs_sh
def test_launcher_install_desktop(launcher_folder, tmp_path):
    data = tmp_path / "xdg"
    out = launch(launcher_folder, "--install-desktop", XDG_DATA_HOME=str(data))
    assert out.returncode == 0, out.stderr
    entry = (data / "applications" / "io.github.armaatus.maplethief.desktop").read_text()
    real = launcher_folder.resolve()
    assert f'Exec="{real}/MapleThief.sh"' in entry
    assert f"Icon={real}/icon.png" in entry
    assert "Categories=Game;" in entry


@needs_sh
def test_launcher_install_desktop_refuses_unquotable_path(tmp_path):
    folder = tmp_path / "a$b"
    folder.mkdir()
    shutil.copyfile(lg.DEFAULT_LAUNCHER, folder / "MapleThief.sh")
    out = launch(folder, "--install-desktop", XDG_DATA_HOME=str(tmp_path / "xdg"))
    assert out.returncode == 1
    assert "can't quote" in out.stderr


# --- scripts/linux-game: argument handling only (building needs Docker and real assets) -----


def run_script(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["bash", "scripts/linux-game", *args], cwd=REPO, capture_output=True, text=True, timeout=60
    )


@needs_bash
def test_script_help():
    out = run_script("--help")
    assert out.returncode == 0
    for text in ("build [REV]", "check [TARBALL]", "--native", "--glibc-floor", "own machines"):
        assert text in out.stderr


@needs_bash
@pytest.mark.parametrize(
    "args, message",
    [
        ((), "Usage: scripts/linux-game"),
        (("run",), "unknown command: run"),
        (("build", "--bogus"), "unknown option: --bogus"),
        (("build", "--arch", "riscv"), "--arch must be amd64 or arm64"),
        (("check", "--frames", "many"), "--frames must be a number"),
        (("build", "--glibc-floor", "new"), "--glibc-floor must be a version"),
    ],
)
def test_script_rejects(args, message):
    out = run_script(*args)
    assert out.returncode == 2, out.stdout + out.stderr
    assert message in out.stderr
