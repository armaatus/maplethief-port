"""The one-command build (#141, ADR 0133): mtport.build's arguments, stamps, staging, clean and
doctor, on synthetic repositories, and that scripts/build, scripts\\build.ps1 and the justfile
are thin callers of it. Nothing here touches the IPA; the real run is an acceptance step
(docs/build/README.md)."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

import pytest

from mtport import __version__
from mtport import bootstrap as bs
from mtport import build as bd
from repo_paths import REPO

needs_bash = pytest.mark.skipif(os.name == "nt" or not shutil.which("bash"), reason="needs bash")
needs_pwsh = pytest.mark.skipif(not shutil.which("pwsh"), reason="needs pwsh (PowerShell 7+)")
needs_just = pytest.mark.skipif(not shutil.which("just"), reason="needs just")


# --- Arguments -------------------------------------------------------------------------------


@pytest.mark.parametrize(
    ("argv", "command", "platform", "config"),
    [
        ([], "build", None, "release"),
        (["mac"], "build", "mac", "release"),
        (["macos"], "build", "mac", "release"),
        (["--mac"], "build", "mac", "release"),
        (["windows", "debug"], "build", "win", "debug"),
        (["-linux"], "build", "linux", "release"),
        (["--WIN", "--release"], "build", "win", "release"),
        (["build", "linux"], "build", "linux", "release"),
        (["debug"], "build", None, "debug"),
        (["package", "linux"], "package", "linux", "release"),
        (["run", "debug"], "run", None, "debug"),
        (["doctor"], "doctor", None, "release"),
    ],
)
def test_parse_args(argv, command, platform, config):
    opts = bd.parse_args(argv)
    assert (opts.command, opts.platform, opts.config) == (command, platform, config)


def test_clean_work_and_game_args():
    assert bd.parse_args(["clean", "work"]).work
    assert not bd.parse_args(["clean"]).work
    assert bd.parse_args(["run", "--", "--bundle", "x"]).game_args == ["--bundle", "x"]


@pytest.mark.parametrize(
    "argv",
    [["frobnicate"], ["doctor", "mac"], ["run", "mac"], ["build", "work"], ["mac", "--", "x"]],
)
def test_parse_args_rejects(argv):
    with pytest.raises(bd.BuildError):
        bd.parse_args(argv)


def test_check_target_ios_names_the_issue():
    with pytest.raises(bd.BuildError, match="#83"):
        bd.check_target("ios")


def test_check_target_other_os(monkeypatch):
    monkeypatch.setattr(bd, "host_platform", lambda: "linux")
    for platform in ("mac", "win"):
        with pytest.raises(bd.BuildError, match="builds only on"):
            bd.check_target(platform)
    bd.check_target("linux")


def test_needs_msvc_only_for_a_windows_build(monkeypatch):
    monkeypatch.setattr(bd, "host_platform", lambda: "win")
    assert bd.needs_msvc(bd.parse_args([]))
    assert bd.needs_msvc(bd.parse_args(["win", "debug"]))
    assert not bd.needs_msvc(bd.parse_args(["linux"]))
    assert not bd.needs_msvc(bd.parse_args(["doctor"]))


# --- Stamps ----------------------------------------------------------------------------------


def make_repo(root: Path) -> Path:
    """Just enough of a checkout for the stamps: the IPA, bootstrap.json, recomp sources."""
    (root / "original").mkdir()
    ipa = root / bs.IPA
    ipa.write_bytes(b"synthetic ipa")
    (root / bs.BINARY).parent.mkdir(parents=True)
    (root / bs.BINARY).write_bytes(b"fat")
    slice_file = root / bs.slice_path(bs.PRIMARY_ARCH)
    slice_file.parent.mkdir(parents=True)
    slice_file.write_bytes(b"slice")
    doc = {
        "tool": {"name": bs.TOOL_NAME, "version": __version__},
        "ipa": {"sha256": bs.sha256_file(ipa)},
        "binary": {"path": bs.BINARY},
        "slices": {bs.PRIMARY_ARCH: {"sha256": "aa"}},
    }
    (root / bs.BOOTSTRAP_JSON).write_text(json.dumps(doc), encoding="utf-8")
    (root / "recomp").mkdir()
    (root / "recomp" / "core.py").write_text("x = 1\n", encoding="utf-8")
    (root / "tools/mtport").mkdir(parents=True)
    (root / "tools/mtport/helper.py").write_text("y = 1\n", encoding="utf-8")
    (root / "work/gen").mkdir(parents=True)
    (root / "work/gen/gen.cmake").write_text("# generated\n", encoding="utf-8")
    return root


def test_bootstrap_fresh_notices_a_changed_ipa(tmp_path):
    repo = make_repo(tmp_path)
    assert bd.bootstrap_fresh(repo)
    (repo / bs.IPA).write_bytes(b"another ipa, same size")
    assert not bd.bootstrap_fresh(repo)


def test_bootstrap_fresh_notices_missing_files_and_version(tmp_path):
    repo = make_repo(tmp_path)
    (repo / bs.slice_path(bs.PRIMARY_ARCH)).unlink()
    assert not bd.bootstrap_fresh(repo)
    second = tmp_path / "two"
    second.mkdir()
    repo2 = make_repo(second)
    doc = json.loads((repo2 / bs.BOOTSTRAP_JSON).read_text(encoding="utf-8"))
    doc["tool"]["version"] = "0.0.0-other"
    (repo2 / bs.BOOTSTRAP_JSON).write_text(json.dumps(doc), encoding="utf-8")
    assert not bd.bootstrap_fresh(repo2)


def test_bootstrap_fresh_without_anything(tmp_path):
    assert not bd.bootstrap_fresh(tmp_path)


def test_recomp_stamp(tmp_path, monkeypatch):
    repo = make_repo(tmp_path)
    runs = []
    monkeypatch.setattr(bd, "run", lambda r, cmd, env=None: runs.append(cmd))
    assert bd.ensure_recomp(repo) is True  # no stamp yet
    assert not bd.ensure_recomp(repo)  # up to date: skipped
    assert len(runs) == 1
    (repo / "recomp" / "core.py").write_text("x = 2\n", encoding="utf-8")
    assert bd.ensure_recomp(repo)  # a recompiler source changed
    assert len(runs) == 2
    doc = json.loads((repo / bs.BOOTSTRAP_JSON).read_text(encoding="utf-8"))
    doc["slices"][bs.PRIMARY_ARCH]["sha256"] = "bb"
    (repo / bs.BOOTSTRAP_JSON).write_text(json.dumps(doc), encoding="utf-8")
    assert bd.ensure_recomp(repo)  # the slice changed


def test_recomp_stamp_needs_the_generated_code(tmp_path, monkeypatch):
    repo = make_repo(tmp_path)
    monkeypatch.setattr(bd, "run", lambda r, cmd, env=None: None)
    bd.ensure_recomp(repo)
    (repo / "work/gen/gen.cmake").unlink()
    assert bd.ensure_recomp(repo)


def test_configure_runs_after_new_generated_code(tmp_path, monkeypatch):
    runs = []
    monkeypatch.setattr(bd, "run", lambda r, cmd, env=None: runs.append(cmd))
    (tmp_path / "CMakePresets.json").write_text("{}", encoding="utf-8")
    (tmp_path / "build/mac-app").mkdir(parents=True)
    (tmp_path / "build/mac-app/build.ninja").write_text("", encoding="utf-8")
    bd.configure(tmp_path, "mac-app", force=False)
    assert runs == []
    bd.configure(tmp_path, "mac-app", force=True)
    assert len(runs) == 1
    bd.configure(tmp_path, "mac-other", force=False)
    assert len(runs) == 2


def test_cmake_version_from_cache(tmp_path):
    assert bd.cmake_version(tmp_path, "win-player") == "0.0.1"
    (tmp_path / "build/win-player").mkdir(parents=True)
    (tmp_path / "build/win-player/CMakeCache.txt").write_text(
        "//c\nMAPLETHIEF_VERSION:STRING=1.2.3-rc.4\n", encoding="utf-8"
    )
    assert bd.cmake_version(tmp_path, "win-player") == "1.2.3-rc.4"


# --- Staging ---------------------------------------------------------------------------------


def make_bundle(root: Path) -> Path:
    bundle = root / "Payload" / "MapleThief.app"
    (bundle / "map").mkdir(parents=True)
    (bundle / "MapleThief").write_bytes(b"\xca\xfe\xba\xbe stand-in")
    (bundle / "map" / "m0.map").write_bytes(b"\x01")
    return bundle


def test_stage_windows_folder_and_zip_keep_the_saves(tmp_path):
    bundle = make_bundle(tmp_path)
    exe = tmp_path / "MapleThief.exe"
    exe.write_bytes(b"MZ game")
    target = tmp_path / "dist" / "win"
    folder, archive = bd.stage_windows_release(tmp_path, exe, "1.0.0", target, bundle, "key-1")
    assert (folder / "MapleThief.exe").read_bytes() == b"MZ game"
    assert (folder / "game/MapleThief.app/map/m0.map").is_file()
    (folder / "data").mkdir()
    (folder / "data/settings.ini").write_text("x", encoding="utf-8")

    # A rebuild replaces the program and leaves the player's data alone.
    exe.write_bytes(b"MZ newer")
    bd.stage_windows_release(tmp_path, exe, "1.0.0", target, bundle, "key-2")
    assert (folder / "MapleThief.exe").read_bytes() == b"MZ newer"
    assert (folder / "data/settings.ini").is_file()
    with zipfile.ZipFile(archive) as z:
        names = z.namelist()
    assert "MapleThief-game-1.0.0-windows-x64/MapleThief.exe" in names
    assert "MapleThief-game-1.0.0-windows-x64/game/MapleThief.app/map/m0.map" in names
    assert not [n for n in names if "/data/" in n]


def test_stage_windows_skips_when_up_to_date(tmp_path):
    bundle = make_bundle(tmp_path)
    exe = tmp_path / "MapleThief.exe"
    exe.write_bytes(b"MZ")
    target = tmp_path / "dist" / "win"
    folder, archive = bd.stage_windows_release(tmp_path, exe, "1.0.0", target, bundle, "k")
    before = archive.stat().st_mtime_ns
    (folder / "game/MapleThief.app/marker").write_text("left alone", encoding="utf-8")
    bd.stage_windows_release(tmp_path, exe, "1.0.0", target, bundle, "k")
    assert archive.stat().st_mtime_ns == before
    assert (folder / "game/MapleThief.app/marker").exists()


@pytest.mark.skipif(sys.platform != "darwin", reason="needs ditto (macOS)")
def test_stage_mac_copies_once_and_keeps_saves(tmp_path):
    src = tmp_path / "build/MapleThief.app"
    (src / "Contents/MacOS").mkdir(parents=True)
    (src / "Contents/MacOS/MapleThief").write_bytes(b"program")
    target = tmp_path / "dist/mac"
    (target / "data").mkdir(parents=True)
    (target / "data/save").write_text("s", encoding="utf-8")

    app = bd.stage_mac(tmp_path, src, target, release=False)
    assert (app / "Contents/MacOS/MapleThief").read_bytes() == b"program"
    first = app.stat().st_mtime_ns
    bd.stage_mac(tmp_path, src, target, release=False)
    assert app.stat().st_mtime_ns == first  # the same bytes: not copied again
    (src / "Contents/MacOS/MapleThief").write_bytes(b"changed")
    bd.stage_mac(tmp_path, src, target, release=False)
    assert (app / "Contents/MacOS/MapleThief").read_bytes() == b"changed"
    assert (target / "data/save").is_file()


def test_write_summary_is_one_line_in_the_folder(tmp_path, capsys):
    target = tmp_path / "dist/mac"
    bd.write_summary(target, "Built: x. Start: y.")
    assert (target / bd.SUMMARY).read_text(encoding="utf-8") == "Built: x. Start: y.\n"
    assert "Built: x. Start: y." in capsys.readouterr().out


# --- run -------------------------------------------------------------------------------------


def test_find_game_and_run_says_what_to_do(tmp_path, monkeypatch):
    monkeypatch.setattr(bd, "host_platform", lambda: "mac")
    with pytest.raises(bd.BuildError, match="just build") as e:
        bd.cmd_run(tmp_path, bd.parse_args(["run"]))
    assert "scripts" in str(e.value)
    exe = tmp_path / "dist/mac/MapleThief.app/Contents/MacOS/MapleThief"
    exe.parent.mkdir(parents=True)
    exe.write_text("", encoding="utf-8")
    assert bd.find_game(tmp_path, "mac", debug=False) == exe
    assert bd.find_game(tmp_path, "mac", debug=True) is None

    linux = tmp_path / "dist/linux/MapleThief-game-1.0.0-linux-x86_64/MapleThief.sh"
    linux.parent.mkdir(parents=True)
    linux.write_text("", encoding="utf-8")
    assert bd.find_game(tmp_path, "linux", debug=False) == linux


# --- clean -----------------------------------------------------------------------------------


def make_cleanable(root: Path) -> Path:
    for rel, text in {
        "original/MapleThief-1.5.ipa": "ipa",
        "work/ipa/x": "w",
        "assets-out/y": "a",
        "dist/mac/MapleThief.app/Contents/MacOS/MapleThief": "b",
        "dist/mac/data/saves/slot": "save",
        "dist/win/MapleThief-game-1.0.0-windows-x64/MapleThief.exe": "b",
        "dist/win/MapleThief-game-1.0.0-windows-x64/data/settings.ini": "s",
        "dist/linux/debug/MapleThief-game-1.0.0-linux-x86_64/data/settings.ini": "d",
        "dist/linux/MapleThief-game-1.0.0-linux-x86_64/user-data.txt": "u",
        "build/mac-app/platform/MapleThief.app/x": "b",
        "build/mac-app/platform/data/settings.ini": "old",
        "build/mac-app/CMakeCache.txt": "c",
        "build/_downloads/sdl.zip": "z",
        "build/python-env/bin": "p",
    }.items():
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
    return root


def test_clean_removes_builds_and_keeps_saves(tmp_path, capsys):
    repo = make_cleanable(tmp_path)
    assert bd.cmd_clean(repo, bd.parse_args(["clean"])) == 0
    out = capsys.readouterr().out
    assert not (repo / "dist/mac/MapleThief.app").exists()
    assert not (repo / "dist/win/MapleThief-game-1.0.0-windows-x64/MapleThief.exe").exists()
    assert not (repo / "build/mac-app/CMakeCache.txt").exists()
    assert (repo / "dist/mac/data/saves/slot").is_file()
    assert (repo / "dist/win/MapleThief-game-1.0.0-windows-x64/data/settings.ini").is_file()
    assert (
        repo / "dist/linux/debug/MapleThief-game-1.0.0-linux-x86_64/data/settings.ini"
    ).is_file()
    assert (repo / "dist/linux/MapleThief-game-1.0.0-linux-x86_64/user-data.txt").is_file()
    assert (repo / "build/mac-app/platform/data/settings.ini").is_file()
    assert (repo / "work/ipa/x").is_file()
    assert (repo / "assets-out/y").is_file()
    assert (repo / "original/MapleThief-1.5.ipa").is_file()
    assert (repo / "build/_downloads/sdl.zip").is_file()
    assert (repo / "build/python-env/bin").is_file()
    for line in ("removed dist/mac/MapleThief.app", "kept    dist/mac/data", "original/"):
        assert line in out


def test_clean_work_needs_the_word_and_never_touches_original(tmp_path):
    repo = make_cleanable(tmp_path)
    bd.cmd_clean(repo, bd.parse_args(["clean", "work"]))
    assert not (repo / "work").exists()
    assert not (repo / "assets-out").exists()
    assert (repo / "original/MapleThief-1.5.ipa").is_file()


# --- doctor ----------------------------------------------------------------------------------


def _check(checks, name):
    return next(c for c in checks if c.name.startswith(name))


def test_doctor_reports_the_ipa_with_a_fix(tmp_path, monkeypatch):
    (tmp_path / "original").mkdir()
    (tmp_path / "original/SHA256SUMS").write_text("", encoding="utf-8")
    ipa = _check(bd.doctor_checks(tmp_path), "The IPA")
    assert not ipa.ok
    assert "docs/play/your-ipa.md" in ipa.fix
    # The fix only points at the document.
    for word in ("decrypt", "download", "http"):
        assert word not in ipa.fix.lower()


def test_doctor_names_every_tool_and_a_fix_for_each(tmp_path, monkeypatch):
    monkeypatch.setattr(bd, "_version_of", lambda tool, pattern=None: None)
    monkeypatch.setattr(bd.shutil, "which", lambda tool: None)
    monkeypatch.setattr(bd, "host_platform", lambda: "mac")
    checks = bd.doctor_checks(tmp_path)
    names = " ".join(c.name for c in checks)
    for name in ("Python", "CMake", "Ninja", "Apple clang", "The IPA", "Disk space", "Docker"):
        assert name in names
    for check in checks:
        if not check.ok:
            assert check.fix, f"{check.name} has no fix"


@pytest.mark.parametrize(("host", "required"), [("mac", True), ("win", True), ("linux", False)])
def test_doctor_needs_cmake_and_ninja_only_outside_the_linux_container(
    tmp_path, monkeypatch, host, required
):
    monkeypatch.setattr(bd, "_version_of", lambda tool, pattern=None: None)
    monkeypatch.setattr(bd, "_vs_install", lambda: None)
    monkeypatch.setattr(bd.shutil, "which", lambda tool: None)
    monkeypatch.setattr(bd, "host_platform", lambda: host)
    by_name = {c.name: c for c in bd.doctor_checks(tmp_path)}
    assert by_name["CMake"].required is required
    assert by_name["Ninja"].required is required


def test_doctor_exit_status(tmp_path, monkeypatch, capsys):
    bad = bd.Check("Thing", False, "missing", "install it")
    optional = bd.Check("Other", False, "missing", "install it", required=False)
    monkeypatch.setattr(bd, "doctor_checks", lambda repo: [optional])
    assert bd.cmd_doctor(tmp_path) == 0
    monkeypatch.setattr(bd, "doctor_checks", lambda repo: [bad])
    assert bd.cmd_doctor(tmp_path) == 1
    assert "fix: install it" in capsys.readouterr().out


def test_doctor_version_is_compared_to_the_minimum():
    assert bd.check_version("x", "x", (3, 28), "f", version=(3, 28)).ok
    assert not bd.check_version("x", "x", (3, 28), "f", version=(3, 27)).ok


# --- The thin callers ------------------------------------------------------------------------


@needs_bash
def test_scripts_build_ios_names_the_issue():
    out = subprocess.run(
        ["bash", "scripts/build", "ios"], cwd=REPO, capture_output=True, text=True, timeout=60
    )
    assert out.returncode == 1
    assert "#83" in out.stderr


@needs_pwsh
def test_build_ps1_parses():
    command = (
        "$errors = $null; "
        "[System.Management.Automation.Language.Parser]::ParseFile("
        "'scripts/build.ps1', [ref]$null, [ref]$errors) | Out-Null; "
        "if ($errors) { $errors; exit 1 }"
    )
    out = subprocess.run(
        ["pwsh", "-NoProfile", "-NonInteractive", "-Command", command],
        cwd=REPO,
        capture_output=True,
        text=True,
        timeout=60,
    )
    assert out.returncode == 0, out.stdout + out.stderr


@needs_pwsh
def test_build_ps1_passes_the_words_to_the_module():
    out = subprocess.run(
        ["pwsh", "-NoProfile", "-NonInteractive", "-File", "scripts/build.ps1", "ios"],
        cwd=REPO,
        capture_output=True,
        text=True,
        timeout=60,
    )
    assert out.returncode == 1
    assert "#83" in out.stderr


@needs_just
@pytest.mark.skipif(os.name == "nt", reason="the POSIX spellings")
@pytest.mark.parametrize(
    ("recipe", "expected"),
    [
        ("test", "scripts/test"),
        ("ci", "scripts/local-ci"),
        ("build", "scripts/build build"),
        ("run", "scripts/build run"),
        ("bootstrap", "scripts/build bootstrap"),
        ("assets", "scripts/build assets"),
        ("recomp", "scripts/build recomp"),
        ("shaders", "scripts/build shaders"),
        ("package", "scripts/build package"),
        ("clean", "scripts/build clean"),
        ("doctor", "scripts/build doctor"),
    ],
)
def test_just_recipes_delegate(recipe, expected):
    out = subprocess.run(
        ["just", "--dry-run", recipe], cwd=REPO, capture_output=True, text=True, timeout=60
    )
    assert out.returncode == 0, out.stderr
    assert (out.stdout + out.stderr).strip() == expected


def test_dist_is_ignored_at_the_top_level_only():
    # A real ignore check, against the tree's .gitignore.
    def ignored(path: str) -> bool:
        return (
            subprocess.run(
                ["git", "check-ignore", "-q", "--no-index", path], cwd=REPO, check=False
            ).returncode
            == 0
        )

    assert ignored("dist/mac/MapleThief.app")
    assert not ignored("tools/dist/x")
