"""One command from a fresh clone to a playable build (#141, ADR 0133).

``scripts/build`` and ``scripts\\build.ps1`` find a Python (and on Windows the Visual Studio
environment) and hand every argument to this module; the ``justfile`` calls those scripts. So
arguments are parsed in one place, and every OS gets the same behaviour.

Usage: ``scripts/build [COMMAND] [PLATFORM] [debug|release] [--ipa PATH]``

    build [mac|win|linux] [debug|release]   (default command, this OS, release)
    --ipa PATH                              with build or bootstrap: your own copy of the game
    run [debug] [-- GAME ARGS]              start this OS's build from dist/
    bootstrap | assets | recomp | shaders   one step, skipped when it is up to date
    package [mac|linux]                     the own-code kit, from a clean checkout (local-ci)
    clean [work]                            dist/ and the builds above; ``work`` adds work/
    doctor                                  check the tools and the IPA, each with its fix
    ci                                      (Windows) says there is no local CI there

Spellings: ``macos`` = ``mac``, ``windows`` = ``win``; a leading dash is ignored
(``--mac``, ``-linux``). ``ios`` stops and names #83.

Results land in ``dist/<platform>/`` (``dist/<platform>/debug/`` for a Debug build), which is
git-ignored. They hold the game, so they stay on your own machines (ADR 0024).

Every step is skipped when its stamp matches its inputs, so a second run with nothing changed
takes seconds:

    bootstrap   work/bootstrap.json against the IPA's SHA-256 (a changed IPA is noticed)
    recomp      work/gen/.recomp-stamp.json: the armv7 slice and the recompiler's sources
    configure   build/<preset>/build.ninja, unless the generated code just changed
    staging     a content digest of what was built, in dist/<platform>/.stamp.json
    linux       the same stamp over HEAD's tree, the generated code and the IPA (no Docker)

``--ipa PATH`` checks that file and copies it to ``original/MapleThief-1.5.ipa``, unless that
already holds the game (ADR 0135). ``scripts/build-from-ipa PATH`` is the same thing, path first.
A relative PATH is read from where you ran the script.

Your saves are safe: a rebuild and ``clean`` keep each ``data`` folder next to the game
(ADR 0127). ``clean`` never touches ``original/``, and ``--ipa`` never replaces a file in it.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
import zipfile
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path

from mtport import __version__
from mtport import bootstrap as bs

REPO = Path(__file__).resolve().parents[2]

PLATFORMS = {
    "mac": "mac",
    "macos": "mac",
    "win": "win",
    "windows": "win",
    "linux": "linux",
    "ios": "ios",
}
CONFIGS = ("debug", "release")
COMMANDS = (
    "build",
    "run",
    "bootstrap",
    "assets",
    "recomp",
    "shaders",
    "package",
    "clean",
    "doctor",
    "ci",
)
WORK_WORD = "work"
# What each command takes, besides its name.
ACCEPTS = {
    "build": {"platform", "config", "ipa"},
    "bootstrap": {"ipa"},
    "run": {"config"},
    "package": {"platform"},
    "clean": {"work"},
}
# Where the user ran scripts/build from: the scripts change to the repository before this module
# runs, so a relative --ipa would otherwise be read from the wrong folder.
CALLER_DIR_ENV = "MAPLETHIEF_CALLER_DIR"

DIST = "dist"
STAMP = ".stamp.json"
SUMMARY = "BUILD.txt"
RECOMP_STAMP = "work/gen/.recomp-stamp.json"
# Next to the game (ADR 0127): the player's data and the marker that moves it to the user folder.
SAVES = ("data", "user-data.txt")
# Directories below build/ that `build` writes.
BUILD_DIRS = (
    "mac-app",
    "mac-debug",
    "win-player",
    "win-debug",
    "linux-game",
    "linux-release",
    "linux-debug",
)
BUNDLE = bs.APP_DIR

# Tools the build needs: name, minimum version (ADR 0002).
CMAKE_MIN = (3, 28)
NINJA_MIN = (1, 11)
APPLE_CLANG_MIN = (15, 0)
GCC_MIN = (13, 0)
CLANG_MIN = (17, 0)
# A macOS Release build took 380 MB (build/, work/ and dist/); a Debug build and the Linux
# container image take more, so leave room for those.
MIN_FREE_GB = 3


class BuildError(Exception):
    """A step failed; the message says what to do."""


@dataclass
class Options:
    command: str = "build"
    platform: str | None = None
    config: str = "release"
    work: bool = False  # clean: also work/ and assets-out/
    game_args: list[str] = field(default_factory=list)
    needs_msvc: bool = False  # the query scripts\build.ps1 makes before running this module
    ipa: str | None = None  # build, bootstrap: the user's own copy of the game


# --- Arguments -------------------------------------------------------------------------------


def _normalise_words(head: Sequence[str]) -> list[str]:
    """A leading dash is ignored, for `just build -linux` and `--mac`, and so is case. The value
    of --ipa is a path and stays as it is, whatever it looks like (as `--ipa=PATH`, so that
    argparse doesn't take a path starting with a dash for an option)."""
    known = {*COMMANDS, *PLATFORMS, *CONFIGS, WORK_WORD}
    words: list[str] = []
    rest = iter(head)
    for word in rest:
        if word == "--ipa":
            value = next(rest, None)
            words.append(word if value is None else f"--ipa={value}")
        else:
            plain = word.lstrip("-").lower()
            words.append(plain if plain in known else word)
    return words


def parse_args(argv: Sequence[str]) -> Options:
    """The command line, in the one place that reads it; see the module docstring."""
    head, tail = list(argv), []
    if "--" in head:
        at = head.index("--")
        head, tail = head[:at], head[at + 1 :]
    words = _normalise_words(head)

    parser = argparse.ArgumentParser(
        prog="scripts/build",
        description="One command from a fresh clone to a playable build (ADR 0133).",
        epilog=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--needs-msvc", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--ipa", metavar="PATH", help="your own copy of the game (ADR 0135)")
    parser.add_argument("words", nargs="*", metavar="WORD", help="command, platform, debug/release")
    # intermixed: a word may follow --ipa (`mac --ipa X debug`).
    args = parser.parse_intermixed_args(words)

    opts = Options(needs_msvc=args.needs_msvc, ipa=args.ipa)
    rest = args.words
    if rest and rest[0] in COMMANDS:
        opts.command, rest = rest[0], rest[1:]
    accepts = ACCEPTS.get(opts.command, set())
    if opts.ipa is not None and "ipa" not in accepts:
        raise BuildError(f"--ipa goes with 'build' or 'bootstrap', not '{opts.command}'")
    if opts.ipa == "":
        raise BuildError("--ipa needs the path of your IPA file")
    for word in rest:
        if word in PLATFORMS and "platform" in accepts:
            opts.platform = PLATFORMS[word]
        elif word in CONFIGS and "config" in accepts:
            opts.config = word
        elif word == WORK_WORD and "work" in accepts:
            opts.work = True
        else:
            raise BuildError(
                f"unexpected '{word}' for '{opts.command}' (scripts/build --help lists the words)"
            )
    if tail and opts.command != "run":
        raise BuildError("arguments after -- only go to 'run' (they are the game's)")
    opts.game_args = tail
    return opts


def host_platform() -> str:
    if sys.platform == "darwin":
        return "mac"
    if sys.platform == "win32":
        return "win"
    if sys.platform.startswith("linux"):
        return "linux"
    raise BuildError(f"unsupported host {sys.platform}")


def needs_msvc(opts: Options) -> bool:
    """Whether this command compiles with MSVC, so scripts\\build.ps1 enters the VS environment."""
    return opts.command == "build" and (opts.platform or host_platform()) == "win"


def check_target(platform: str) -> None:
    """Refuse a platform this host can't build."""
    if platform == "ios":
        raise BuildError(
            "no iOS build yet: #83 adds the Xcode project and sideload steps, which need a "
            "device and signing. It is not part of this command."
        )
    host = host_platform()
    if platform == "mac" and host != "mac":
        raise BuildError("the macOS app builds only on a Mac (run this there)")
    if platform == "win" and host != "win":
        raise BuildError("the Windows game builds only on Windows (run scripts\\build.ps1 there)")


# --- Running things --------------------------------------------------------------------------


def log(message: str) -> None:
    print(message, flush=True)


def module_env(repo: Path) -> dict[str, str]:
    """The environment for a ``-m`` tool of ours: tools/ importable, and scripts find PYTHON."""
    env = dict(os.environ)
    tools = str(repo / "tools")
    env["PYTHONPATH"] = tools + os.pathsep + env["PYTHONPATH"] if env.get("PYTHONPATH") else tools
    env["PYTHON"] = sys.executable
    return env


def run(repo: Path, cmd: Sequence[str | Path], env: dict[str, str] | None = None) -> None:
    """Run a command in the repository; a non-zero exit stops the build."""
    argv = [str(c) for c in cmd]
    code = subprocess.run(argv, cwd=repo, env=env or module_env(repo), check=False).returncode
    if code != 0:
        raise BuildError(f"{' '.join(argv)} failed (exit {code})")


def find_bash() -> str:
    """bash for scripts that are written in it: on Windows Git for Windows' (not WSL's)."""
    if os.name != "nt":
        return "bash"
    git = shutil.which("git")
    if git:
        root = Path(git).parent.parent  # ...\Git\cmd\git.exe -> ...\Git
        for candidate in ("bin/bash.exe", "usr/bin/bash.exe"):
            if (root / candidate).is_file():
                return str(root / candidate)
    raise BuildError("Git for Windows' bash was not found (needed for this script)")


def sha256_tree(top: Path) -> str:
    """A digest of a directory's files and names; a symbolic link counts as its target text."""
    h = hashlib.sha256()
    for path in sorted(top.rglob("*")):
        rel = path.relative_to(top).as_posix()
        if path.is_symlink():
            h.update(f"{rel}\0L{os.readlink(path)}\0".encode())
        elif path.is_file():
            h.update(f"{rel}\0F{bs.sha256_file(path)}\0".encode())
    return h.hexdigest()


def read_stamp(path: Path) -> str | None:
    try:
        return json.loads(path.read_text(encoding="utf-8")).get("key")
    except (OSError, ValueError, AttributeError):
        return None


def write_stamp(path: Path, key: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({"key": key}) + "\n", encoding="utf-8")


# --- The steps -------------------------------------------------------------------------------


def bootstrap_fresh(repo: Path) -> bool:
    """work/bootstrap.json says this exact IPA was extracted and the files are still there.

    The IPA is hashed (18 MB, a fraction of a second), so a changed IPA is always noticed."""
    try:
        doc = json.loads((repo / bs.BOOTSTRAP_JSON).read_text(encoding="utf-8"))
        ipa = repo / bs.IPA
        if doc["tool"]["version"] != __version__ or doc["ipa"]["sha256"] != bs.sha256_file(ipa):
            return False
        paths = [doc["binary"]["path"], bs.slice_path(bs.PRIMARY_ARCH)]
    except (OSError, ValueError, KeyError, TypeError):
        return False
    return all((repo / p).is_file() for p in paths)


def ipa_source(raw: str) -> Path:
    """The path given to --ipa, made absolute against the folder the user ran the script from."""
    path = Path(raw).expanduser()
    return (
        path if path.is_absolute() else Path(os.environ.get(CALLER_DIR_ENV) or os.getcwd()) / path
    )


def adopt_ipa(repo: Path, raw: str) -> None:
    """Check the file at ``raw``, then copy it to ``original/`` for the rest of the build.

    A refused file copies nothing. A file already in ``original/`` is never replaced: when it is
    the same game it stays and is used (the main binary is what the build reads), and anything
    else is a refusal for the user to sort out (ADR 0135)."""
    source = ipa_source(raw)
    if not source.exists():
        raise BuildError(f"{raw}: there is no file at this path. {bs.WHICH_FILE}")
    if not source.is_file():
        raise BuildError(f"{raw}: this is a folder, not an IPA file. {bs.WHICH_FILE}")
    target = repo / bs.IPA
    try:
        sums = bs.load_sums(repo)
        _, digest = bs.check_ipa_file(source, sums, raw)
    except bs.BootstrapError as e:
        raise BuildError(str(e)) from None
    if os.path.lexists(target):
        try:
            bs.check_ipa_file(target, sums, bs.IPA)
        except (bs.BootstrapError, OSError) as e:
            raise BuildError(
                f"{bs.IPA} is already there and is not this game, so it was left alone: {e} "
                "Move it away or delete it, then run this again."
            ) from None
        log(f"== {bs.IPA} already holds this game: using it, and leaving it as it is")
        return
    log(f"== copying your IPA to {bs.IPA}")
    target.parent.mkdir(parents=True, exist_ok=True)
    # *.ipa is git-ignored, so an interrupted copy can't end up in a commit.
    tmp = target.with_name(f".incoming-{os.getpid()}.ipa")
    try:
        shutil.copyfile(source, tmp)
        if bs.sha256_file(tmp) != digest:
            raise BuildError(f"{raw}: the file changed while it was being copied")
        os.replace(tmp, target)
    except OSError as e:
        raise BuildError(f"copying {raw} to {bs.IPA} failed: {e}") from None
    finally:
        tmp.unlink(missing_ok=True)


def check_tools(repo: Path, platform: str) -> None:
    """Stop with doctor's fixes if this platform's build needs a tool that is missing."""
    missing = [c for c in doctor_checks(repo) if not c.ok and platform in c.needed_for]
    if missing:
        lines = "".join(f"\n  {c.name}: {c.detail}\n    fix: {c.fix}" for c in missing)
        raise BuildError(
            f"this build needs tools that are missing or too old:{lines}\n"
            "Nothing was copied. `scripts/build doctor` checks everything."
        )


def ensure_bootstrap(repo: Path) -> None:
    if bootstrap_fresh(repo):
        log("== bootstrap (work/ipa): up to date")
        return
    log("== bootstrap (work/ipa)")
    run(repo, [sys.executable, "-B", "-m", "mtport.bootstrap", "--repo", repo])


def recomp_key(repo: Path) -> str:
    """What the generated code depends on: the armv7 slice and the recompiler's own sources
    (recomp/ and the tools it imports)."""
    doc = json.loads((repo / bs.BOOTSTRAP_JSON).read_text(encoding="utf-8"))
    h = hashlib.sha256(doc["slices"][bs.PRIMARY_ARCH]["sha256"].encode())
    for top in ("recomp", "tools/mtport"):
        for path in sorted((repo / top).rglob("*.py")):
            if "__pycache__" not in path.parts:
                h.update(path.relative_to(repo).as_posix().encode() + b"\0")
                h.update(bs.sha256_file(path).encode())
    return h.hexdigest()


def ensure_recomp(repo: Path) -> bool:
    """Bring work/gen up to date; returns whether the recompiler ran."""
    key = recomp_key(repo)
    stamp = repo / RECOMP_STAMP
    if read_stamp(stamp) == key and (repo / "work/gen/gen.cmake").is_file():
        log("== recompile (work/gen): up to date")
        return False
    log("== recompile (work/gen)")
    run(repo, [sys.executable, "-B", "-m", "recomp", "all"])
    write_stamp(stamp, key)
    return True


def shaders_fresh(repo: Path) -> bool:
    script = repo / "tools/mtport/shaderbuild.py"
    return (
        subprocess.run(
            [sys.executable, "-I", "-B", str(script), "check", "--repo", str(repo)],
            cwd=repo,
            capture_output=True,
            check=False,
        ).returncode
        == 0
    )


def configure(repo: Path, preset: str, force: bool) -> None:
    """``cmake --preset`` unless the build tree is current. Ninja reconfigures by itself when a
    CMakeLists changes; the presets file and newly generated code it doesn't see."""
    ninja = repo / "build" / preset / "build.ninja"
    presets = repo / "CMakePresets.json"
    if not force and ninja.is_file() and ninja.stat().st_mtime >= presets.stat().st_mtime:
        log(f"== configure ({preset}): up to date")
        return
    log(f"== configure ({preset})")
    run(repo, ["cmake", "--preset", preset])


def cmake_version(repo: Path, preset: str) -> str:
    """The version this build was configured with: CMake cache, else the default."""
    try:
        text = (repo / "build" / preset / "CMakeCache.txt").read_text(encoding="utf-8")
    except OSError:
        return "0.0.1"
    found = re.search(r"^MAPLETHIEF_VERSION:[A-Z]+=(.+)$", text, re.MULTILINE)
    return found.group(1).strip() if found else "0.0.1"


def relative(repo: Path, path: Path) -> str:
    return path.relative_to(repo).as_posix()


def write_summary(target: Path, line: str) -> None:
    """The one line: what was built and how to start it, on screen and in the folder."""
    target.mkdir(parents=True, exist_ok=True)
    (target / SUMMARY).write_text(line + "\n", encoding="utf-8")
    log("")
    log(line)


def replace_dir(tmp: Path, final: Path) -> None:
    if final.exists():
        shutil.rmtree(final)
    tmp.rename(final)


def stage_mac(repo: Path, src: Path, target: Path, release: bool) -> Path:
    """Copy the built .app into ``target`` unless it is the same bytes; check the copy."""
    if not src.is_dir():
        raise BuildError(f"the build finished but {relative(repo, src)} is missing")
    target.mkdir(parents=True, exist_ok=True)
    app = target / "MapleThief.app"
    key = sha256_tree(src)
    if app.is_dir() and read_stamp(target / STAMP) == key:
        log(f"== staging ({relative(repo, app)}): up to date")
    else:
        log(f"== staging {relative(repo, app)}")
        tmp = target / ".MapleThief.staging"
        if tmp.exists():
            shutil.rmtree(tmp)
        # ditto keeps the bundle's symlinks and signature (as ADR 0024's kit does).
        run(repo, ["ditto", src, tmp])
        replace_dir(tmp, app)
        write_stamp(target / STAMP, key)
    if release:
        run(repo, ["codesign", "--verify", "--deep", "--strict", app])
        archs = subprocess.run(
            ["lipo", "-archs", str(app / "Contents/MacOS/MapleThief")],
            capture_output=True,
            text=True,
            check=False,
        ).stdout
        if not {"arm64", "x86_64"} <= set(archs.split()):
            raise BuildError(f"{relative(repo, app)} is not universal: lipo says '{archs.strip()}'")
    return app


def build_mac(repo: Path, opts: Options, recompiled: bool) -> str:
    debug = opts.config == "debug"
    preset = "mac-debug" if debug else "mac-app"
    configure(repo, preset, recompiled)
    log(f"== build ({preset})")
    target = "MapleThief" if debug else "MapleThief_app"
    run(repo, ["cmake", "--build", "--preset", preset, "--target", target])
    out = repo / DIST / "mac" / ("debug" if debug else "")
    src = repo / "build" / preset / "platform/MapleThief.app"
    app = stage_mac(repo, src, out, release=not debug)
    rel = relative(repo, app)
    if debug:
        return (
            f"Built: {rel} (macOS Debug, this CPU only, for debugging: not for judging the game; "
            f"needs work/ in place). Start: just run debug."
        )
    return (
        f"Built: {rel} (macOS universal, Release, ad-hoc signed, for your own machines only). "
        f"Start: just run, or open {rel}."
    )


def stage_windows_release(
    repo: Path, exe: Path, version: str, target: Path, bundle: Path, key: str
) -> tuple[Path, Path]:
    """The portable folder and its zip: exe, game/MapleThief.app, README.txt. ``data/`` next to
    the exe, and so the player's saves, is never touched and never in the zip."""
    from mtport import linux_game

    name = f"MapleThief-game-{version}-windows-x64"
    folder = target / name
    archive = target / f"{name}.zip"
    if folder.is_dir() and archive.is_file() and read_stamp(target / STAMP) == key:
        log(f"== staging ({relative(repo, folder)}): up to date")
        return folder, archive
    log(f"== staging {relative(repo, folder)} and its zip")
    if not exe.is_file():
        raise BuildError(f"the build finished but {relative(repo, exe)} is missing")
    folder.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(exe, folder / "MapleThief.exe")
    game = folder / "game" / "MapleThief.app"
    if game.parent.exists():
        shutil.rmtree(game.parent)
    linux_game._copy_bundle(bundle, game)
    (folder / "README.txt").write_text(
        f"MapleThief {version} for Windows x64: your own build.\n\n"
        "Built on your machine from your own copy of the game. It contains the game: keep it "
        "to your own machines and never share or upload it.\n\n"
        "Start: double-click MapleThief.exe. Saves and settings go in the data folder next "
        "to it.\n",
        encoding="utf-8",
    )
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as z:
        for path in [folder / "MapleThief.exe", folder / "README.txt", *sorted(game.rglob("*"))]:
            if path.is_file():
                z.write(path, f"{name}/{path.relative_to(folder).as_posix()}")
    write_stamp(target / STAMP, key)
    return folder, archive


def build_windows(repo: Path, opts: Options, recompiled: bool) -> str:
    debug = opts.config == "debug"
    # win-player: the static MSVC runtime and no work/ path compiled in, so the folder runs on
    # another PC; it finds the game in game/ next to the exe (ADR 0125).
    preset = "win-debug" if debug else "win-player"
    configure(repo, preset, recompiled)
    log(f"== build ({preset})")
    run(repo, ["cmake", "--build", "--preset", preset, "--target", "MapleThief"])
    exe = repo / "build" / preset / "platform/MapleThief.exe"
    out = repo / DIST / "win" / ("debug" if debug else "")
    if debug:
        if not exe.is_file():
            raise BuildError(f"the build finished but {relative(repo, exe)} is missing")
        out.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(exe, out / "MapleThief.exe")
        return (
            f"Built: {relative(repo, out / 'MapleThief.exe')} (Windows Debug, for debugging: not "
            "for judging the game; needs work/ in place). Start: just run debug."
        )
    version = cmake_version(repo, preset)
    ipa = json.loads((repo / bs.BOOTSTRAP_JSON).read_text(encoding="utf-8"))["ipa"]["sha256"]
    key = hashlib.sha256(f"{bs.sha256_file(exe)}\0{ipa}\0{version}".encode()).hexdigest()
    folder, archive = stage_windows_release(repo, exe, version, out, repo / BUNDLE, key)
    rel = relative(repo, folder)
    return (
        f"Built: {rel} and {relative(repo, archive)} (Windows x64, Release, portable, for your "
        f"own machines only). Start: just run, or {rel}\\MapleThief.exe."
    )


def linux_key(repo: Path, debug: bool) -> str:
    """HEAD's tree (the container builds an export of HEAD), the generated code and the IPA."""
    tree = subprocess.run(
        ["git", "rev-parse", "HEAD^{tree}"],
        cwd=repo,
        capture_output=True,
        text=True,
        check=False,
    ).stdout.strip()
    if not tree:
        raise BuildError("the Linux build needs a git checkout (it builds an export of HEAD)")
    doc = json.loads((repo / bs.BOOTSTRAP_JSON).read_text(encoding="utf-8"))
    gen = read_stamp(repo / RECOMP_STAMP) or ""
    parts = f"{tree}\0{gen}\0{doc['ipa']['sha256']}\0{debug}"
    return hashlib.sha256(parts.encode()).hexdigest()


def linux_tarball(target: Path) -> Path | None:
    found = sorted(target.glob("MapleThief-game-*-linux-*.tar.gz"))
    return found[-1] if found else None


def build_linux(repo: Path, opts: Options) -> str:
    debug = opts.config == "debug"
    out = repo / DIST / "linux" / ("debug" if debug else "")
    key = linux_key(repo, debug)
    tarball = linux_tarball(out)
    if tarball is not None and read_stamp(out / STAMP) == key:
        log("== build (Linux, Docker): up to date")
    else:
        dirty = subprocess.run(
            ["git", "diff", "--quiet", "HEAD"], cwd=repo, capture_output=True, check=False
        ).returncode
        if dirty:
            log("note: uncommitted changes are not in this build: the container exports HEAD")
        log("== build (Linux, in Docker)")
        cmd = [find_bash(), "scripts/linux-game", "build", "--out", relative(repo, out)]
        if debug:
            cmd.append("--debug")
        run(repo, cmd)
        tarball = linux_tarball(out)
        if tarball is None:
            raise BuildError(f"the build finished but there is no tarball in {relative(repo, out)}")
        write_stamp(out / STAMP, key)
    folder = tarball.name.removesuffix(".tar.gz")
    rel = relative(repo, out / folder)
    what = (
        "Debug, for debugging: not for judging the game"
        if debug
        else "Release, for your own machines only"
    )
    return (
        f"Built: {relative(repo, tarball)} and the folder {rel} (Linux x86_64, {what}). "
        f"Start: {rel}/MapleThief.sh."
    )


# --- The commands ----------------------------------------------------------------------------


def cmd_build(repo: Path, opts: Options) -> int:
    started = time.monotonic()
    platform = opts.platform or host_platform()
    check_target(platform)
    if opts.ipa is not None:
        check_tools(repo, platform)
        adopt_ipa(repo, opts.ipa)
    ensure_bootstrap(repo)
    recompiled = ensure_recomp(repo)
    if not shaders_fresh(repo):
        log("warning: the GPU shaders are older than their sources; run: just shaders")
    if platform == "mac":
        line = build_mac(repo, opts, recompiled)
    elif platform == "win":
        line = build_windows(repo, opts, recompiled)
    else:
        line = build_linux(repo, opts)
    write_summary(repo / DIST / platform / ("debug" if opts.config == "debug" else ""), line)
    log(f"Done in {time.monotonic() - started:.1f} s.")
    return 0


def find_game(repo: Path, platform: str, debug: bool) -> Path | None:
    out = repo / DIST / platform / ("debug" if debug else "")
    if platform == "mac":
        exe = out / "MapleThief.app" / "Contents" / "MacOS" / "MapleThief"
        return exe if exe.is_file() else None
    if platform == "win":
        if debug:
            exe = out / "MapleThief.exe"
            return exe if exe.is_file() else None
        found = sorted(out.glob("MapleThief-game-*-windows-*/MapleThief.exe"))
    else:
        found = sorted(out.glob("MapleThief-game-*-linux-*/MapleThief.sh"))
    return found[-1] if found else None


def cmd_run(repo: Path, opts: Options) -> int:
    platform = host_platform()
    debug = opts.config == "debug"
    game = find_game(repo, platform, debug)
    if game is None:
        build = "just build" + (" debug" if debug else "")
        script = ("scripts\\build.ps1" if os.name == "nt" else "scripts/build") + (
            " debug" if debug else ""
        )
        raise BuildError(f"no {platform} build in {DIST}/ yet. Build one first: {build} ({script})")
    return subprocess.run([str(game), *opts.game_args], check=False).returncode


def cmd_shaders(repo: Path) -> int:
    if shaders_fresh(repo):
        log("== shaders: up to date (the header's stamps match the sources)")
        return 0
    run(repo, [find_bash(), "scripts/build-shaders"])
    return 0


def cmd_package(repo: Path, opts: Options) -> int:
    """The own-code kit (ADR 0023, ADR 0024): always from a clean checkout, by scripts/local-ci,
    so nothing of work/ can reach it."""
    platform = opts.platform or host_platform()
    if platform not in ("mac", "linux") or os.name == "nt":
        raise BuildError(
            "no local kit build here: kits are packaged on a clean checkout, by CI for every "
            "OS (ADR 0126) or by scripts/local-ci on a Mac or Linux machine"
        )
    if platform == "mac":
        check_target("mac")
    log(f"== package ({platform} kit, clean checkout, scripts/local-ci --release)")
    run(repo, [find_bash(), "scripts/local-ci", "--only", platform, "--release"])
    log("The kit is in build/local-ci/<rev>/ (it holds none of the game).")
    return 0


def _remove_except(top: Path, keep: set[Path], log_removed: list[str], repo: Path) -> None:
    """Delete ``top`` and everything in it, except the paths in ``keep`` and their parents."""
    if not top.exists() and not top.is_symlink():
        return
    if not any(top == k or top in k.parents for k in keep):
        if top.is_dir() and not top.is_symlink():
            shutil.rmtree(top)
        else:
            top.unlink()
        log_removed.append(relative(repo, top))
        return
    if top in keep:
        return
    for child in list(top.iterdir()):
        _remove_except(child, keep, log_removed, repo)


def saves_under(repo: Path) -> set[Path]:
    """The player's data folders and markers in dist/ and in build/<preset>/platform/."""
    patterns = (
        f"{DIST}/*/{{name}}",
        f"{DIST}/*/*/{{name}}",
        f"{DIST}/*/debug/*/{{name}}",
        "build/*/platform/{name}",
        "build/linux-game/*/{name}",
    )
    found: set[Path] = set()
    for pattern in patterns:
        for name in SAVES:
            found.update(p for p in repo.glob(pattern.format(name=name)) if p.exists())
    return found


def cmd_clean(repo: Path, opts: Options) -> int:
    keep = saves_under(repo)
    removed: list[str] = []
    _remove_except(repo / DIST, keep, removed, repo)
    for name in BUILD_DIRS:
        _remove_except(repo / "build" / name, keep, removed, repo)
    if opts.work:
        _remove_except(repo / "work", keep, removed, repo)
        _remove_except(repo / "assets-out", keep, removed, repo)
    for path in removed:
        log(f"removed {path}")
    if not removed:
        log("nothing to remove")
    for path in sorted(keep):
        log(f"kept    {relative(repo, path)} (your saves and settings; delete it by hand)")
    untouched = ["original/ (your IPA)", "build/_downloads/ (the download cache)"]
    if not opts.work:
        untouched.insert(0, "work/ and assets-out/ (add the word 'work' to remove them)")
    log("Not touched: " + ", ".join(untouched) + ".")
    return 0


# --- doctor ----------------------------------------------------------------------------------


def _version_of(tool: str, pattern: str = r"(\d+)\.(\d+)") -> tuple[int, ...] | None:
    """A tool's version from its ``--version`` output, or None if it can't be found or run."""
    path = shutil.which(tool)
    if path is None:
        return None
    try:
        out = subprocess.run(
            [path, "--version"], capture_output=True, text=True, check=False, timeout=30
        )
    except (OSError, subprocess.SubprocessError):
        return None
    found = re.search(pattern, out.stdout + out.stderr)
    return tuple(int(g) for g in found.groups()) if found else None


def _vs_install() -> str | None:
    """The Visual Studio with the x64 C++ tools, from vswhere (Windows)."""
    base = os.environ.get("ProgramFiles(x86)", "")
    vswhere = Path(base) / "Microsoft Visual Studio/Installer/vswhere.exe"
    if not vswhere.is_file():
        return None
    out = subprocess.run(
        [
            str(vswhere),
            "-latest",
            "-products",
            "*",
            "-requires",
            "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
            "-property",
            "installationPath",
        ],
        capture_output=True,
        text=True,
        check=False,
    ).stdout.strip()
    return out or None


# What each `build` platform compiles with: the Linux build is in a container, so it needs only
# Docker (and, like everything, Python and disk space).
ALL_TARGETS = ("mac", "win", "linux")
NATIVE_TARGETS = ("mac", "win")


@dataclass
class Check:
    name: str
    ok: bool
    detail: str
    fix: str = ""
    required: bool = True
    needed_for: tuple[str, ...] = ALL_TARGETS  # the `build` platforms this check stops


def _dotted(version: tuple[int, ...] | None) -> str:
    return ".".join(map(str, version)) if version else "not found"


def check_version(
    name: str,
    tool: str,
    minimum: tuple[int, ...],
    fix: str,
    version=None,
    required: bool = True,
    needed_for: tuple[str, ...] = NATIVE_TARGETS,
) -> Check:
    found = version if version is not None else _version_of(tool)
    ok = found is not None and found >= minimum
    return Check(
        name, ok, f"{_dotted(found)} (needs {_dotted(minimum)}+)", fix, required, needed_for
    )


def doctor_checks(repo: Path) -> list[Check]:
    """Every check of ``doctor``; each failure carries its fix."""
    host = host_platform()
    py = sys.version_info
    checks = [
        Check(
            "Python",
            py >= (3, 10),
            f"{py.major}.{py.minor}.{py.micro} (needs 3.10+)",
            "install Python 3.10+ (python.org, Homebrew, or `uv python install 3.12`)",
        )
    ]

    cmake_fix = {
        "mac": "brew install cmake",
        "win": 'it comes with Visual Studio ("Desktop development with C++")',
        "linux": "sudo apt install cmake (3.28+)",
    }[host]
    ninja_fix = {
        "mac": "brew install ninja",
        "win": 'it comes with Visual Studio ("Desktop development with C++")',
        "linux": "sudo apt install ninja-build",
    }[host]
    vs = _vs_install() if host == "win" else None
    cmake_version_found = ninja_version_found = None
    if vs:
        # Visual Studio's own CMake and Ninja, when they aren't on PATH outside its environment.
        ide = Path(vs) / "Common7/IDE/CommonExtensions/Microsoft/CMake"
        if not shutil.which("cmake") and (ide / "CMake/bin/cmake.exe").is_file():
            cmake_version_found = _version_of(str(ide / "CMake/bin/cmake.exe"))
        if not shutil.which("ninja") and (ide / "Ninja/ninja.exe").is_file():
            ninja_version_found = _version_of(str(ide / "Ninja/ninja.exe"))
    # On Linux the game builds in a container, so these two are only for building by hand.
    native = host != "linux"
    checks.append(
        check_version("CMake", "cmake", CMAKE_MIN, cmake_fix, cmake_version_found, required=native)
    )
    checks.append(
        check_version("Ninja", "ninja", NINJA_MIN, ninja_fix, ninja_version_found, required=native)
    )

    if host == "mac":
        found = _version_of("clang", r"(?:clang version )(\d+)\.(\d+)")
        checks.append(
            check_version(
                "Apple clang", "clang", APPLE_CLANG_MIN, "xcode-select --install", version=found
            )
        )
    elif host == "win":
        checks.append(
            Check(
                "Visual Studio",
                vs is not None,
                vs or "not found (needs VS 2022 17.8+ with the x64 C++ tools)",
                'install Visual Studio 2022 with "Desktop development with C++"',
                needed_for=NATIVE_TARGETS,
            )
        )
    else:
        # A container build needs no local compiler; only --native and a build by hand do.
        gcc = _version_of("g++")
        clang = _version_of("clang++")
        ok = (gcc is not None and gcc >= GCC_MIN) or (clang is not None and clang >= CLANG_MIN)
        checks.append(
            Check(
                "Compiler (only for a build by hand)",
                ok,
                f"g++ {_dotted(gcc)}, clang++ {_dotted(clang)} (needs GCC 13+ or Clang 17+)",
                "sudo apt install build-essential g++-13",
                required=False,
                needed_for=(),
            )
        )

    docker = shutil.which("docker")
    checks.append(
        Check(
            "Docker (for `build linux` and `shaders`)",
            docker is not None,
            docker or "not found",
            "OrbStack or Docker Desktop (Linux: your distribution's docker)",
            required=host == "linux",
            needed_for=("linux",),
        )
    )
    # needed_for=(): `build --ipa` checks its own file, which is not in place yet at this point.
    try:
        sums = bs.load_sums(repo)
        size, digest = bs.verify_ipa(repo, sums)
        how = "SHA-256 matches" if bs.is_exact_copy(sums, digest) else "repacked, binary matches"
        checks.append(Check("The IPA", True, f"{bs.IPA} ({size} bytes, {how})", needed_for=()))
    except bs.BootstrapError as e:
        checks.append(
            Check(
                "The IPA",
                False,
                str(e),
                "see docs/play/your-ipa.md for which file it must be",
                needed_for=(),
            )
        )
    free = shutil.disk_usage(repo).free / 1024**3
    checks.append(
        Check(
            "Disk space",
            free >= MIN_FREE_GB,
            f"{free:.0f} GB free (needs {MIN_FREE_GB} GB)",
            "free up some disk space",
        )
    )
    checks.append(
        Check(
            "uv (for `just test`, optional)",
            shutil.which("uv") is not None,
            shutil.which("uv") or "not found",
            "https://docs.astral.sh/uv/getting-started/installation/",
            required=False,
            needed_for=(),
        )
    )
    return checks


def cmd_doctor(repo: Path) -> int:
    failed = False
    for check in doctor_checks(repo):
        if check.ok:
            log(f"ok    {check.name}: {check.detail}")
            continue
        tag = "FAIL " if check.required else "note "
        failed |= check.required
        log(f"{tag} {check.name}: {check.detail}\n      fix: {check.fix}")
    log("Everything `just build` needs is here." if not failed else "Fix the FAIL lines above.")
    return 1 if failed else 0


def dispatch(repo: Path, opts: Options) -> int:
    if opts.command == "build":
        return cmd_build(repo, opts)
    if opts.command == "run":
        return cmd_run(repo, opts)
    if opts.command == "bootstrap":
        if opts.ipa is not None:
            adopt_ipa(repo, opts.ipa)
        ensure_bootstrap(repo)
    elif opts.command == "recomp":
        ensure_bootstrap(repo)
        ensure_recomp(repo)
    elif opts.command == "assets":
        ensure_bootstrap(repo)
        # scripts/pipeline's own stamp is its manifest: a run with nothing changed writes nothing.
        log("== assets (assets-out/)")
        run(repo, [sys.executable, "-B", "-m", "mtport.pipeline", "run"])
    elif opts.command == "shaders":
        return cmd_shaders(repo)
    elif opts.command == "package":
        return cmd_package(repo, opts)
    elif opts.command == "clean":
        return cmd_clean(repo, opts)
    elif opts.command == "doctor":
        return cmd_doctor(repo)
    elif opts.command == "ci":
        if os.name == "nt":
            raise BuildError(
                "scripts/local-ci needs a Mac or Linux machine; on Windows run scripts\\test.ps1"
            )
        run(repo, [find_bash(), "scripts/local-ci"])
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    try:
        opts = parse_args(sys.argv[1:] if argv is None else argv)
        if opts.needs_msvc:
            print("yes" if needs_msvc(opts) else "no")
            return 0
        return dispatch(REPO, opts)
    except BuildError as e:
        print(f"scripts/build: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
