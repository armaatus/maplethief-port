"""Push a published release's player builds to itch.io with butler (ADR 0146, #208).

``python3 -m mtport.release itch TAG`` (run by ``scripts/release itch TAG`` on the Mac and
``scripts\\release.ps1 itch TAG`` on Windows):

1. refuses a pre-release tag (``-rc.N`` is not pushed to itch.io);
2. checks that the GitHub Release ``TAG`` is published (not a draft) and lists each platform's
   player build and ``SHA256SUMS``, and downloads them with ``gh release download`` into
   ``build/release/TAG/itch/``;
3. checks each file against ``SHA256SUMS`` and runs the content guard's player mode on it
   again (:mod:`mtport.release`);
4. runs ``butler push <file> <target>:<channel> --userversion <version>`` per platform, then
   ``butler status <target>``.

Nothing is pushed unless every file passes step 3. The channels are the player builds' platform
names (:data:`CHANNELS`). butler is the pinned version of :data:`BUTLER`, downloaded once into
``build/_downloads/`` and checked by SHA-256 (ADR 0003); ``--butler`` names another executable
(the tests use a fake one). butler reads its key from ``BUTLER_API_KEY`` in the environment;
it never appears on a command line or in a file. ``--dry-run`` does steps 1 to 3 and prints the
butler commands instead of running them, so it needs neither butler nor the key. ``--from DIR``
takes the files and ``SHA256SUMS`` from DIR instead of the published release (a rehearsal on
the outputs of ``scripts/release build`` and ``publish --dry-run``).

The downloads are untrusted input: they are hashed and read by the guard, never executed.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import stat
import subprocess
import sys
import urllib.request
import zipfile
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

from mtport import release as rel

TARGET = "armaatus/maplethief"  # the itch.io page: armaatus.itch.io/maplethief
# Platform (scripts/release --platforms) -> itch.io channel, which is also the player build's
# platform name (mtport.release.PLAYER_PLATFORMS). The Switch zip is not pushed (ADR 0146).
CHANNELS = {
    "mac": "macos-universal",
    "linux": "linux-x86_64",
    "windows": "windows-x64",
}
KEY_VARIABLE = "BUTLER_API_KEY"
SUMS = "SHA256SUMS"


@dataclass(frozen=True)
class ButlerBuild:
    channel: str  # broth.itch.zone's name for the OS and CPU
    sha256: str
    executable: str


@dataclass(frozen=True)
class Butler:
    version: str
    builds: dict[str, ButlerBuild]

    def url(self, build: ButlerBuild) -> str:
        return f"https://broth.itch.zone/butler/{build.channel}/{self.version}/archive/default"


# butler 15.32.0 (2026-10-06), the zip broth.itch.zone serves per OS; hashes taken 2026-11-13.
BUTLER = Butler(
    version="15.32.0",
    builds={
        "darwin-arm64": ButlerBuild(
            "darwin-arm64",
            "f1c460f08c02686b49f8da74344e5578ece9ea1a8bae1779445b5d98e0a794b3",
            "butler",
        ),
        "darwin-x86_64": ButlerBuild(
            "darwin-amd64",
            "a92947c9556c9bfb0cc9bdc3814f342b30e0ee7269efc418658894a916ca9584",
            "butler",
        ),
        "linux-x86_64": ButlerBuild(
            "linux-amd64",
            "2335971394ef6596f95ded0833e85ee28755e13761716ef4c91d6b11f69162f5",
            "butler",
        ),
        "windows-x86_64": ButlerBuild(
            "windows-amd64",
            "5902c488b86bb3518ca00bf4e706d7687f229e3f0f28653cd943348ea9ec8878",
            "butler.exe",
        ),
    },
)


class ItchError(Exception):
    """A refusal: nothing was pushed."""


def host_key(system: str | None = None, machine: str | None = None) -> str:
    """This machine's key into :data:`BUTLER`'s builds, e.g. ``darwin-arm64``."""
    system = (system or platform.system()).lower()
    machine = (machine or platform.machine()).lower()
    cpu = {"amd64": "x86_64", "x64": "x86_64", "aarch64": "arm64"}.get(machine, machine)
    return f"{system}-{cpu}"


def channels(platforms: Sequence[str]) -> dict[str, str]:
    """Platform -> channel for the platforms asked for, in that order."""
    unknown = [p for p in platforms if p not in CHANNELS]
    if unknown:
        raise ItchError(
            f"--platforms: unknown platform(s) {', '.join(unknown)} (mac, linux, windows)"
        )
    return {p: CHANNELS[p] for p in platforms}


def file_name(version: str, channel: str) -> str:
    """The player build file of a channel: one of mtport.release.player_names()."""
    for name in rel.player_names(version):
        if name.startswith(f"MapleThief-{version}-{channel}."):
            return name
    raise ItchError(f"no player build for channel {channel}")


# --- The published release ------------------------------------------------------------------


Run = Callable[[list[str]], str]


def run_command(argv: list[str]) -> str:
    """Run argv, return its stdout; ItchError if it fails."""
    try:
        out = subprocess.run(argv, check=True, capture_output=True, text=True)
    except FileNotFoundError as e:
        raise ItchError(f"{argv[0]} not found") from e
    except subprocess.CalledProcessError as e:
        raise ItchError(f"{' '.join(argv)} failed (exit {e.returncode}): {e.stderr.strip()}") from e
    return out.stdout


def download_release(
    tag: str, names: Sequence[str], dest: Path, gh: Sequence[str] = ("gh",), run: Run = run_command
) -> None:
    """Download ``names`` and SHA256SUMS of the published release TAG into ``dest``."""
    info = json.loads(run([*gh, "release", "view", tag, "--json", "isDraft,isPrerelease,assets"]))
    if info.get("isDraft"):
        raise ItchError(f"release {tag} is a draft; run scripts/release publish {tag} first")
    if info.get("isPrerelease"):
        raise ItchError(f"release {tag} is a pre-release; pre-releases are not pushed to itch.io")
    assets = {a.get("name") for a in info.get("assets", [])}
    missing = [n for n in (*names, SUMS) if n not in assets]
    if missing:
        raise ItchError(f"missing from the published release {tag}: {', '.join(missing)}")
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    patterns = [arg for n in (*names, SUMS) for arg in ("--pattern", n)]
    run([*gh, "release", "download", tag, "--dir", str(dest), *patterns])


def copy_local(source: Path, names: Sequence[str], dest: Path) -> None:
    """``--from DIR``: copy ``names`` and SHA256SUMS from DIR into ``dest``."""
    missing = [n for n in (*names, SUMS) if not (source / n).is_file()]
    if missing:
        raise ItchError(f"missing from {source}: {', '.join(missing)}")
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    for n in (*names, SUMS):
        shutil.copyfile(source / n, dest / n)


def verify(dest: Path, names: Sequence[str]) -> None:
    """Each file must be listed in SHA256SUMS with its SHA-256 (byte-identical to the release's
    file); then the guard's player mode must pass on all of them."""
    sums = rel.parse_sums((dest / SUMS).read_text(encoding="utf-8"), str(dest / SUMS))
    for n in names:
        if n not in sums:
            raise ItchError(f"{n} is not listed in {SUMS}")
        got = rel.sha256_file(dest / n)
        if got != sums[n]:
            raise ItchError(f"{n} does not match {SUMS} (SHA-256 {got}, expected {sums[n]})")
        print(f"{n}: OK")
    status = rel.main(["guard", "--mode", "player", *(str(dest / n) for n in names)])
    if status != 0:
        raise ItchError("the content guard rejected a player build; nothing was pushed")


# --- butler ---------------------------------------------------------------------------------


def _extract_butler(archive: Path, into: Path) -> None:
    """Unpack butler's zip: plain files at its top level only."""
    into.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as z:
        for info in z.infolist():
            p = PurePosixPath(info.filename)
            if info.is_dir() or len(p.parts) != 1 or "\\" in info.filename or p.name in ("", ".."):
                raise ItchError(f"{archive.name}: unexpected entry {info.filename!r}")
            target = into / p.name
            target.write_bytes(z.read(info))
            target.chmod(target.stat().st_mode | stat.S_IXUSR | stat.S_IRUSR)


def pinned_butler(downloads: Path, key: str | None = None, log=print) -> Path:
    """The pinned butler for this machine, downloaded and hash-checked once into
    ``downloads/butler-<version>-<key>/``."""
    key = key or host_key()
    build = BUTLER.builds.get(key)
    if build is None:
        raise ItchError(f"no pinned butler for {key} ({', '.join(BUTLER.builds)})")
    root = downloads / f"butler-{BUTLER.version}-{key}"
    exe = root / build.executable
    stamp = root / "archive.sha256"
    if (
        exe.is_file()
        and stamp.is_file()
        and stamp.read_text(encoding="utf-8").strip() == build.sha256
    ):
        return exe
    downloads.mkdir(parents=True, exist_ok=True)
    archive = downloads / f"butler-{BUTLER.version}-{key}.zip"
    url = BUTLER.url(build)
    log(f"downloading butler {BUTLER.version} ({build.channel}) from {url}")
    try:
        with urllib.request.urlopen(url, timeout=60) as response, archive.open("wb") as out:
            shutil.copyfileobj(response, out, 1 << 20)
    except OSError as e:
        archive.unlink(missing_ok=True)
        raise ItchError(f"download of {url} failed: {e}") from e
    got = rel.sha256_file(archive)
    if got != build.sha256:
        archive.unlink()
        raise ItchError(
            f"butler {BUTLER.version} ({build.channel}): SHA-256 {got}, expected {build.sha256}"
        )
    if root.exists():
        shutil.rmtree(root)
    _extract_butler(archive, root)
    archive.unlink()
    stamp.write_text(build.sha256 + "\n", encoding="utf-8")
    return exe


def push_commands(
    files: dict[str, Path], target: str, version: str, butler: Sequence[str]
) -> list[list[str]]:
    """One ``butler push`` per channel, then ``butler status``."""
    cmds = [
        [*butler, "push", str(path), f"{target}:{channel}", "--userversion", version]
        for channel, path in files.items()
    ]
    cmds.append([*butler, "status", target])
    return cmds


def push(cmds: list[list[str]], dry_run: bool, env: dict[str, str] | None = None) -> None:
    """Run the butler commands, or print them with ``dry_run``. butler inherits the key from
    the environment."""
    for cmd in cmds:
        shown = " ".join(cmd)
        if dry_run:
            print(f"   dry-run, not run: {shown}")
            continue
        print(f"+ {shown}", flush=True)
        status = subprocess.run(cmd, env=env, check=False).returncode
        if status != 0:
            raise ItchError(f"{shown} failed (exit {status})")


# --- The command ----------------------------------------------------------------------------


def run(
    args: argparse.Namespace,
    gh: Sequence[str] = ("gh",),
    gh_run: Run = run_command,
    env: dict[str, str] | None = None,
    butler_cmd: Sequence[str] | None = None,
) -> int:
    """The ``itch`` command; ``gh``, ``gh_run``, ``env`` and ``butler_cmd`` are for tests."""
    env = dict(os.environ) if env is None else env
    try:
        version, prerelease = rel.parse_tag(args.tag)
        if prerelease:
            raise ItchError(
                f"{args.tag} is a pre-release; -rc.N is not pushed to itch.io (ADR 0146)"
            )
        chans = channels([p for p in args.platforms.split(",") if p])
        names = [file_name(version, c) for c in chans.values()]
        dest = Path(args.out) if args.out else rel.REPO / "build/release" / args.tag / "itch"

        print(f"== Player builds of {args.tag} for {', '.join(chans.values())}")
        if args.source:
            copy_local(Path(args.source), names, dest)
        else:
            download_release(args.tag, names, dest, gh, gh_run)

        print(f"== Verify against {SUMS} and the content guard (player mode)")
        verify(dest, names)

        print(f"== butler ({args.target})")
        if not args.dry_run and not env.get(KEY_VARIABLE):
            raise ItchError(f"{KEY_VARIABLE} is not set; butler reads its itch.io API key from it")
        butler: list[str]
        if butler_cmd:
            butler = list(butler_cmd)
        elif args.butler or args.dry_run:
            butler = [args.butler or "butler"]
        else:
            butler = [str(pinned_butler(rel.REPO / "build/_downloads"))]
        files = {c: dest / n for c, n in zip(chans.values(), names, strict=True)}
        push(push_commands(files, args.target, version, butler), args.dry_run, env)
    except ItchError as e:
        print(f"itch: {e}", file=sys.stderr)
        return 1
    except (ValueError, OSError) as e:
        print(f"itch: error: {e}", file=sys.stderr)
        return 1
    return 0


def add_parser(sub) -> None:
    p = sub.add_parser("itch", help="push a published release's player builds to itch.io")
    p.add_argument("tag", help="vX.Y.Z (pre-releases are not pushed)")
    p.add_argument(
        "--platforms",
        default=",".join(CHANNELS),
        help="comma-separated, of mac,linux,windows (default all three)",
    )
    p.add_argument("--target", default=TARGET, help=f"itch.io user/game (default {TARGET})")
    p.add_argument("--dry-run", action="store_true", help="print the butler commands")
    p.add_argument(
        "--from",
        dest="source",
        metavar="DIR",
        help="take the files and SHA256SUMS from DIR instead of the published release",
    )
    p.add_argument("--out", metavar="DIR", help="download directory (build/release/TAG/itch)")
    p.add_argument(
        "--butler", metavar="EXE", help="this butler instead of the pinned download (tests)"
    )
