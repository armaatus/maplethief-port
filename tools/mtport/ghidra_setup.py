"""Fetch, verify and unpack the pinned Ghidra and JDK for the RE tooling (ADR 0007).

Nothing is installed system-wide. The archives are downloaded into the shared download
cache (``MAPLETHIEF_DOWNLOAD_DIR``, default ``build/_downloads``, the ADR 0003 convention),
checked against the SHA-256 pinned below and unpacked under ``build/ghidra/``::

    build/ghidra/ghidra_12.2_PUBLIC/     Ghidra (GHIDRA_INSTALL_DIR)
    build/ghidra/jdk-25.0.4.1+1/         Temurin JDK (JAVA_HOME, Contents/Home on macOS)
    build/ghidra/gradle-9.8.0/           Gradle, only where Ghidra ships no native binaries
    build/ghidra/home/                   Ghidra's settings, cache and temp files (XDG_*)
    build/ghidra/python-env/             uv environment of the ``ghidra`` group (PyGhidra)

Ghidra's zip has native binaries (decompiler, demangler) only for Windows x64 and Linux
x64. On other hosts (macOS, Linux arm64) setup compiles them once with the pinned Gradle
and the system C/C++ toolchain (``gradle buildNatives``, about 20 s on an M-series Mac).

Each unpacked tree has a ``<dir>.sha256`` stamp holding its archive's hash; a tree whose
stamp is missing or stale is unpacked again. Re-running is cheap and needs no network once
the cache is filled.

Command line (``tools/ghidra/setup`` runs the first form)::

    python3 -m mtport.ghidra_setup [--repo DIR] setup     # fetch + unpack, print the paths
    python3 -m mtport.ghidra_setup [--repo DIR] env       # print KEY=VALUE for a shell
    python3 -m mtport.ghidra_setup [--repo DIR] check     # exit 1 unless both are unpacked

Standard library only, so it runs before any uv environment exists.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import platform
import shutil
import stat
import subprocess
import sys
import tarfile
import urllib.request
import zipfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

GHIDRA_VERSION = "12.2"
JDK_VERSION = "25.0.4.1+1"


@dataclass(frozen=True)
class Archive:
    """One pinned download and the single top-level directory it unpacks to."""

    url: str
    sha256: str
    size: int
    top: str  # top-level directory inside the archive
    home: str = ""  # path below ``top`` used as the tool's home ("" = ``top`` itself)

    @property
    def filename(self) -> str:
        return self.url.rsplit("/", 1)[1].replace("%2B", "+")


GHIDRA = Archive(
    url=(
        "https://github.com/NationalSecurityAgency/ghidra/releases/download/"
        "Ghidra_12.2_build/ghidra_12.2_PUBLIC_20261016.zip"
    ),
    sha256="2c2cd776db3c1f6a097ba418922378ac3649cadd5e844a6f5f088ae628a7be67",
    size=569712708,
    top="ghidra_12.2_PUBLIC",
)

# Ghidra's public zip ships native binaries (decompiler, demangler) only for Windows x64 and
# Linux x64. Elsewhere `gradle buildNatives` compiles them from the zip's own sources; this is
# the Gradle the zip's wrapper names (support/gradle/gradle-wrapper.properties).
GRADLE = Archive(
    url="https://github.com/gradle/gradle-distributions/releases/download/v9.8.0/gradle-9.8.0-bin.zip",
    sha256="bafd5ce9cfaea0fbccfdc8439a1ac42fbd4cd9c89dc9a988228d8a2639a58e6c",
    size=151611662,
    top="gradle-9.8.0",
)

_JDK_BASE = "https://github.com/adoptium/temurin25-binaries/releases/download/jdk-25.0.4.1%2B1/"
_JDK_TOP = "jdk-25.0.4.1+1"

# Temurin 25.0.4.1+1 (2026-08-19): the newest JDK 25 build published for all five hosts.
# Keyed by (platform.system(), normalised machine).
JDKS: dict[tuple[str, str], Archive] = {
    ("Darwin", "arm64"): Archive(
        _JDK_BASE + "OpenJDK25U-jdk_aarch64_mac_hotspot_25.0.4.1_1.tar.gz",
        "61979887f7506a24a57439ff99adb8b3a7fc89977d9cfe3b8984f58a981b7b9d",
        136355078,
        _JDK_TOP,
        "Contents/Home",
    ),
    ("Darwin", "x86_64"): Archive(
        _JDK_BASE + "OpenJDK25U-jdk_x64_mac_hotspot_25.0.4.1_1.tar.gz",
        "e6229d9504f7922053ab31821b9e6bee8761daf7b026a3476d1a027563009880",
        120256199,
        _JDK_TOP,
        "Contents/Home",
    ),
    ("Linux", "x86_64"): Archive(
        _JDK_BASE + "OpenJDK25U-jdk_x64_linux_hotspot_25.0.4.1_1.tar.gz",
        "dbb698396d478e7fa2b1e50f4103324b2a99b90569ee27c33f2261f9215cf41e",
        141329719,
        _JDK_TOP,
    ),
    ("Linux", "arm64"): Archive(
        _JDK_BASE + "OpenJDK25U-jdk_aarch64_linux_hotspot_25.0.4.1_1.tar.gz",
        "69df11a02cfa3ef7d7ca645e03edce6778ec090e100f6ae2b42097865730ac52",
        140137820,
        _JDK_TOP,
    ),
    ("Windows", "x86_64"): Archive(
        _JDK_BASE + "OpenJDK25U-jdk_x64_windows_hotspot_25.0.4.1_1.zip",
        "00c847d804f4a78e9f04f2683faf14fed898535b177b7fc704486cb0284e9283",
        141167264,
        _JDK_TOP,
    ),
}

_MACHINES = {"amd64": "x86_64", "x86_64": "x86_64", "x64": "x86_64", "arm64": "arm64"}
_MACHINES["aarch64"] = "arm64"


class SetupError(RuntimeError):
    """A download, hash check or unpack step failed."""


def host() -> tuple[str, str]:
    machine = platform.machine().lower()
    return platform.system(), _MACHINES.get(machine, machine)


def jdk_for(system: str, machine: str) -> Archive:
    try:
        return JDKS[(system, _MACHINES.get(machine.lower(), machine.lower()))]
    except KeyError:
        known = ", ".join(f"{s}/{m}" for s, m in JDKS)
        raise SetupError(f"no pinned JDK for {system}/{machine} (pinned: {known})") from None


@dataclass(frozen=True)
class Layout:
    """Where everything lives for one checkout."""

    repo: Path
    downloads: Path
    root: Path  # build/ghidra

    @classmethod
    def for_repo(cls, repo: Path, env: dict[str, str] | None = None) -> Layout:
        env = os.environ if env is None else env
        downloads = env.get("MAPLETHIEF_DOWNLOAD_DIR") or str(repo / "build" / "_downloads")
        root = env.get("MAPLETHIEF_GHIDRA_DIR") or str(repo / "build" / "ghidra")
        return cls(repo, Path(downloads), Path(root))

    @property
    def ghidra_dir(self) -> Path:
        return self.root / GHIDRA.top

    def jdk_home(self, jdk: Archive) -> Path:
        base = self.root / jdk.top
        return base / jdk.home if jdk.home else base

    @property
    def home(self) -> Path:
        return self.root / "home"

    @property
    def python_env(self) -> Path:
        return self.root / "python-env"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def fetch(archive: Archive, downloads: Path, log=print) -> Path:
    """The cached archive, downloaded first if it is missing or its hash is wrong."""
    path = downloads / archive.filename
    if path.is_file():
        if sha256_file(path) == archive.sha256:
            return path
        log(f"cached {path.name} has the wrong SHA-256; downloading it again")
    downloads.mkdir(parents=True, exist_ok=True)
    part = path.with_name(path.name + ".part")
    log(f"downloading {archive.url} ({archive.size / 1e6:.0f} MB)")
    try:
        with urllib.request.urlopen(archive.url, timeout=60) as response, part.open("wb") as out:
            shutil.copyfileobj(response, out, 1 << 20)
    except OSError as e:
        part.unlink(missing_ok=True)
        raise SetupError(f"download of {archive.url} failed: {e}") from e
    got = sha256_file(part)
    if got != archive.sha256:
        part.unlink()
        raise SetupError(f"{archive.filename}: SHA-256 {got}, expected {archive.sha256}")
    os.replace(part, path)
    return path


def _safe_member(name: str, top: str) -> PurePosixPath:
    """``name`` as a relative path inside ``top``; raises for anything that could escape."""
    if "\\" in name:
        raise SetupError(f"archive entry with a backslash: {name!r}")
    p = PurePosixPath(name)
    if p.is_absolute() or ".." in p.parts or not p.parts or p.parts[0] != top:
        raise SetupError(f"archive entry outside {top}/: {name!r}")
    return p


def _unzip(path: Path, dest: Path, top: str) -> None:
    with zipfile.ZipFile(path) as archive:
        for info in archive.infolist():
            rel = _safe_member(info.filename, top)
            mode = info.external_attr >> 16
            if stat.S_ISLNK(mode):
                raise SetupError(f"{path.name}: symbolic link entry {info.filename!r}")
            target = dest.joinpath(*rel.parts)
            if info.is_dir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(info) as src, target.open("wb") as out:
                shutil.copyfileobj(src, out, 1 << 20)
            # zipfile drops permissions; keep the executable bit (launchers, decompiler).
            target.chmod(0o755 if mode & 0o111 else 0o644)


def _untar(path: Path, dest: Path, top: str) -> None:
    if not hasattr(tarfile, "data_filter"):
        raise SetupError("this Python lacks tarfile.data_filter; use Python 3.10.12+ / 3.11.4+")
    with tarfile.open(path) as archive:
        for member in archive.getmembers():
            _safe_member(member.name, top)
        # The "data" filter refuses links that leave dest, device files and setuid bits.
        archive.extractall(dest, filter="data")


def unpack(archive: Archive, cached: Path, root: Path, log=print) -> Path:
    """Unpack ``cached`` to ``root/archive.top`` unless its stamp already matches."""
    target = root / archive.top
    stamp = root / f"{archive.top}.sha256"
    if target.is_dir() and stamp.is_file() and stamp.read_text().strip() == archive.sha256:
        return target
    log(f"unpacking {cached.name} -> {target}")
    root.mkdir(parents=True, exist_ok=True)
    tmp = root / f".tmp-{archive.top}"
    shutil.rmtree(tmp, ignore_errors=True)
    tmp.mkdir()
    try:
        if cached.name.endswith(".zip"):
            _unzip(cached, tmp, archive.top)
        else:
            _untar(cached, tmp, archive.top)
        stamp.unlink(missing_ok=True)
        shutil.rmtree(target, ignore_errors=True)
        os.replace(tmp / archive.top, target)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    stamp.write_text(archive.sha256 + "\n")
    return target


_NATIVE_OS = {
    ("Darwin", "arm64"): "mac_arm_64",
    ("Darwin", "x86_64"): "mac_x86_64",
    ("Linux", "arm64"): "linux_arm_64",
    ("Linux", "x86_64"): "linux_x86_64",
    ("Windows", "x86_64"): "win_x86_64",
}


def native_os(system: str, machine: str) -> str:
    return _NATIVE_OS[(system, _MACHINES.get(machine.lower(), machine.lower()))]


def has_decompiler(ghidra_dir: Path, os_name: str) -> bool:
    """Whether Ghidra has a decompiler binary for ``os_name``, shipped (``os/``) or built
    by ``gradle buildNatives`` (``build/os/``, which Ghidra prefers)."""
    exe = "decompile.exe" if os_name.startswith("win") else "decompile"
    module = ghidra_dir / "Ghidra" / "Features" / "Decompiler"
    return any((module / sub / os_name / exe).is_file() for sub in ("os", "build/os"))


def build_natives(layout: Layout, jdk: Archive, os_name: str, log=print) -> None:
    """Compile Ghidra's native tools with the pinned Gradle (needs a C/C++ toolchain:
    Xcode Command Line Tools on macOS, gcc/clang + make on Linux)."""
    gradle_dir = unpack(GRADLE, fetch(GRADLE, layout.downloads, log), layout.root, log)
    gradle = gradle_dir / "bin" / ("gradle.bat" if os_name.startswith("win") else "gradle")
    if not os_name.startswith("win"):
        gradle.chmod(0o755)
    env = dict(os.environ)
    env["JAVA_HOME"] = str(layout.jdk_home(jdk))
    env["GRADLE_USER_HOME"] = str(layout.home / "gradle")
    log(f"building Ghidra native tools for {os_name} (gradle buildNatives)")
    result = subprocess.run(
        [str(gradle), "--no-daemon", "--quiet", "buildNatives"],
        cwd=layout.ghidra_dir / "support" / "gradle",
        env=env,
        check=False,
    )
    if result.returncode != 0 or not has_decompiler(layout.ghidra_dir, os_name):
        raise SetupError(
            "gradle buildNatives failed; install a C/C++ toolchain "
            "(macOS: xcode-select --install; Linux: gcc/clang and make) and re-run"
        )


def is_ready(layout: Layout, jdk: Archive, os_name: str) -> bool:
    for archive in (GHIDRA, jdk):
        stamp = layout.root / f"{archive.top}.sha256"
        if not stamp.is_file() or stamp.read_text().strip() != archive.sha256:
            return False
    return (
        (layout.ghidra_dir / "support").is_dir()
        and layout.jdk_home(jdk).is_dir()
        and has_decompiler(layout.ghidra_dir, os_name)
    )


def setup(layout: Layout, jdk: Archive, os_name: str, log=print) -> None:
    for archive in (GHIDRA, jdk):
        unpack(archive, fetch(archive, layout.downloads, log), layout.root, log)
    for sub in ("config", "cache", "tmp"):
        (layout.home / sub).mkdir(parents=True, exist_ok=True)
    if not has_decompiler(layout.ghidra_dir, os_name):
        build_natives(layout, jdk, os_name, log)


def environment(layout: Layout, jdk: Archive) -> dict[str, str]:
    """Variables that point PyGhidra and Ghidra at the unpacked tools and keep their
    settings, caches and temp files inside ``build/ghidra/home``."""
    java_home = str(layout.jdk_home(jdk))
    return {
        "GHIDRA_INSTALL_DIR": str(layout.ghidra_dir),
        "JAVA_HOME": java_home,
        "JAVA_HOME_OVERRIDE": java_home,
        "XDG_CONFIG_HOME": str(layout.home / "config"),
        "XDG_CACHE_HOME": str(layout.home / "cache"),
        "MAPLETHIEF_GHIDRA_TMP": str(layout.home / "tmp"),
        "UV_PROJECT_ENVIRONMENT": str(layout.python_env),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.ghidra_setup", description=__doc__.splitlines()[0]
    )
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("cmd", choices=("setup", "env", "check"), nargs="?", default="setup")
    args = parser.parse_args(argv)
    layout = Layout.for_repo(args.repo.resolve())
    try:
        jdk = jdk_for(*host())
        os_name = native_os(*host())
        if args.cmd == "check":
            return 0 if is_ready(layout, jdk, os_name) else 1
        if args.cmd == "setup":
            setup(layout, jdk, os_name)
        elif not is_ready(layout, jdk, os_name):
            raise SetupError("Ghidra/JDK not unpacked yet; run tools/ghidra/setup")
        env = environment(layout, jdk)
    except SetupError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    for key, value in env.items():
        print(f"{key}={value}" if args.cmd == "env" else f"{key:<22} {value}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
