"""Every version pin of the repository against upstream (#340, ADR 0003 addendum of 2026-11-28).

Dependabot (``.github/dependabot.yml``) bumps the actions and the Python packages. Everything
else is pinned where it can't follow: the C and C++ dependencies by SHA-256 in
``cmake/Dependencies.cmake``, the container images by digest and their tools by version in the
``ARG`` lines of ``tools/containers/*/Dockerfile``, uv in CI, the linters in ``scripts/lint``,
Ghidra and its JDK in ``tools/mtport/ghidra_setup.py``. This module reads each pin from its file
(:func:`pins`, so a new pin needs no second list), asks upstream for the latest, and prints one
row per pin. The Python packages are listed too, for the whole picture.

A row's state is ``current``, ``newer`` (with what upstream has), ``follows`` (the pin moves
with another one, such as the Switch's licence texts with the devkitA64 image), ``fixed`` (a
dated snapshot) or ``unknown`` (the lookup failed; the reason is shown). Upstream is GitHub's
API (with ``GH_TOKEN``, ``GITHUB_TOKEN`` or ``gh auth token`` when there is one), PyPI's JSON
API, and the Docker Hub and ghcr.io registries. Run by hand; never on a pull request, where
upstream would decide whether CI passes. The bump routine is
``docs/build/how-to/bump-a-dependency.md``.

CLI (``scripts/deps check``)::

    PYTHONPATH=tools python3 -m mtport.deps check [--only SUBSTRING] [--offline]

``--offline`` lists the pins without asking upstream. Exit status: 0 nothing newer, 1 something
is newer, 2 a lookup failed (and nothing is newer).
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import urllib.error
import urllib.request
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

DEPENDENCIES_CMAKE = "cmake/Dependencies.cmake"
CONTAINERS = "tools/containers"
CI_YML = ".github/workflows/ci.yml"
LINT = "scripts/lint"
GHIDRA_SETUP = "tools/mtport/ghidra_setup.py"
PYPROJECT = "pyproject.toml"

# Dockerfile ARGs that name a version, and the GitHub repository whose latest release is its
# upstream, with the prefix of that repository's tags. tests/test_deps.py checks that every
# version ARG of every Dockerfile is here.
DOCKER_ARG_RELEASES: dict[str, tuple[str, str]] = {
    "CMAKE_VERSION": ("Kitware/CMake", "v"),
    "PWSH_VERSION": ("PowerShell/PowerShell", "v"),
    "DXC_TAG": ("microsoft/DirectXShaderCompiler", ""),
}
# The same for repositories whose "latest" release is a rolling one (``continuous``): the
# highest tag that is a version.
DOCKER_ARG_TAGS: dict[str, tuple[str, str]] = {
    "APPIMAGE_RUNTIME_RELEASE": ("AppImage/type2-runtime", ""),
}
# Dockerfile ARGs that pin something no lookup can improve on: the apt snapshot's date (any
# later date is newer), and what is derived from another pin (a file's hash, an archive name).
DOCKER_ARG_FIXED = ("UBUNTU_SNAPSHOT",)
DOCKER_ARG_DERIVED = re.compile(r"_SHA256(_\w+)?$|^DXC_ARCHIVE$|^TARGETARCH$")

# scripts/lint's <NAME>_VERSION lines and their PyPI packages.
LINT_PACKAGES = {
    "RUFF": "ruff",
    "CLANG_FORMAT": "clang-format",
    "SHELLCHECK_PY": "shellcheck-py",
    "CLANG_TIDY": "clang-tidy",
    "CHECK_JSONSCHEMA": "check-jsonschema",
}

STATES = ("current", "newer", "follows", "fixed", "unknown")


@dataclass(frozen=True)
class Result:
    state: str  # one of STATES
    upstream: str = ""


@dataclass
class Pin:
    """One pinned version: what it is, its value, where it lives, and how to look it up."""

    name: str
    pinned: str
    where: list[str]
    check: Callable[[Upstream], Result] = field(repr=False, compare=False)


# --- Upstream ------------------------------------------------------------------------------


class UpstreamError(Exception):
    """A lookup failed: no network, a rate limit, an unexpected answer."""


Fetch = Callable[[str, dict[str, str], str], tuple[dict[str, str], bytes]]


def urllib_fetch(url: str, headers: dict[str, str], method: str) -> tuple[dict[str, str], bytes]:
    request = urllib.request.Request(url, headers=headers, method=method)
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            return {k.lower(): v for k, v in response.headers.items()}, response.read()
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        raise UpstreamError(f"{url}: {e}") from e


def github_token() -> str:
    for var in ("GH_TOKEN", "GITHUB_TOKEN"):
        if os.environ.get(var):
            return os.environ[var]
    gh = shutil.which("gh")
    if gh:
        out = subprocess.run(
            [gh, "auth", "token"], capture_output=True, text=True, timeout=30, check=False
        )
        if out.returncode == 0:
            return out.stdout.strip()
    return ""


class Upstream:
    """The lookups, through one ``fetch`` (tests pass a fake), each answer cached."""

    def __init__(self, fetch: Fetch = urllib_fetch, token: str = "") -> None:
        self._fetch = fetch
        self._token = token
        self._cache: dict[tuple[str, str], tuple[dict[str, str], bytes]] = {}

    def _get(self, url: str, headers: dict[str, str], method: str = "GET"):
        key = (method, url)
        if key not in self._cache:
            self._cache[key] = self._fetch(url, headers, method)
        return self._cache[key]

    def _json(self, url: str, headers: dict[str, str] | None = None):
        _, body = self._get(url, headers or {})
        try:
            return json.loads(body)
        except ValueError as e:
            raise UpstreamError(f"{url}: not JSON") from e

    def github(self, path: str):
        headers = {"Accept": "application/vnd.github+json", "User-Agent": "maplethief-deps"}
        if self._token:
            headers["Authorization"] = f"Bearer {self._token}"
        return self._json(f"https://api.github.com/{path}", headers)

    def latest_release(self, repo: str) -> str:
        """The tag of ``repo``'s latest release (GitHub skips drafts and pre-releases)."""
        return str(self.github(f"repos/{repo}/releases/latest")["tag_name"])

    def tags(self, repo: str) -> list[str]:
        return [str(t["name"]) for t in self.github(f"repos/{repo}/tags?per_page=100")]

    def commits_ahead(self, repo: str, base: str, head: str) -> int:
        """How many commits ``head`` (a branch or commit) has that ``base`` lacks."""
        return int(self.github(f"repos/{repo}/compare/{base}...{head}")["ahead_by"])

    def default_branch(self, repo: str) -> str:
        return str(self.github(f"repos/{repo}")["default_branch"])

    def last_commit_touching(self, repo: str, path: str) -> str:
        commits = self.github(f"repos/{repo}/commits?path={path}&per_page=1")
        if not commits:
            raise UpstreamError(f"{repo}: no commit touches {path}")
        return str(commits[0]["sha"])

    def pypi_latest(self, package: str) -> str:
        """The latest final release on PyPI (``info.version`` skips pre-releases)."""
        return str(self._json(f"https://pypi.org/pypi/{package}/json")["info"]["version"])

    def image_digest(self, image: str, tag: str) -> str:
        """The digest a registry serves for ``image:tag``: the index's, as a pin names it."""
        registry, repo = split_image(image)
        if registry == "docker.io":
            token_url = (
                "https://auth.docker.io/token?service=registry.docker.io"
                f"&scope=repository:{repo}:pull"
            )
            api = "https://registry-1.docker.io"
        else:
            token_url = f"https://{registry}/token?scope=repository:{repo}:pull"
            api = f"https://{registry}"
        token = self._json(token_url).get("token", "")
        accept = ", ".join(
            (
                "application/vnd.oci.image.index.v1+json",
                "application/vnd.docker.distribution.manifest.list.v2+json",
                "application/vnd.oci.image.manifest.v1+json",
                "application/vnd.docker.distribution.manifest.v2+json",
            )
        )
        headers, _ = self._get(
            f"{api}/v2/{repo}/manifests/{tag}",
            {"Accept": accept, "Authorization": f"Bearer {token}"},
            "HEAD",
        )
        digest = headers.get("docker-content-digest", "")
        if not digest.startswith("sha256:"):
            raise UpstreamError(f"{image}:{tag}: no digest in the registry's answer")
        return digest


def split_image(image: str) -> tuple[str, str]:
    """``ubuntu`` → (docker.io, library/ubuntu); ``ghcr.io/a/b`` → (ghcr.io, a/b)."""
    first, _, rest = image.partition("/")
    if rest and ("." in first or ":" in first):
        return first, rest
    return "docker.io", image if rest else f"library/{image}"


# --- Checks --------------------------------------------------------------------------------


def version_key(text: str) -> tuple[int, ...]:
    return tuple(int(n) for n in re.findall(r"\d+", text))


def same_or_newer(pinned: str, latest: str) -> Result:
    if not version_key(latest):
        raise UpstreamError(f"upstream's latest, {latest!r}, is not a version")
    if latest == pinned or version_key(latest) <= version_key(pinned):
        return Result("current", latest)
    return Result("newer", latest)


def release_check(repo: str, pinned: str, prefix: str = "") -> Callable[[Upstream], Result]:
    def check(up: Upstream) -> Result:
        tag = up.latest_release(repo)
        return same_or_newer(pinned, tag.removeprefix(prefix))

    return check


def tag_check(repo: str, pinned: str, prefix: str) -> Callable[[Upstream], Result]:
    """The highest version among ``repo``'s tags that start with ``prefix``."""

    def check(up: Upstream) -> Result:
        versions = [t.removeprefix(prefix) for t in up.tags(repo) if t.startswith(prefix)]
        versions = [v for v in versions if re.fullmatch(r"[\d.]+", v)]
        if not versions:
            raise UpstreamError(f"{repo}: no tag {prefix}<version>")
        return same_or_newer(pinned, max(versions, key=version_key))

    return check


def branch_check(repo: str, commit: str, branch: str = "") -> Callable[[Upstream], Result]:
    """A commit pin against the head of ``branch`` (the default branch when empty)."""

    def check(up: Upstream) -> Result:
        head = branch or up.default_branch(repo)
        ahead = up.commits_ahead(repo, commit, head)
        if ahead == 0:
            return Result("current", f"{head} head")
        return Result("newer", f"{head} is {ahead} commit{'s' * (ahead != 1)} ahead")

    return check


def path_check(repo: str, commit: str, path: str) -> Callable[[Upstream], Result]:
    """A commit pin of files below ``path``: newer when a later commit touched them."""

    def check(up: Upstream) -> Result:
        last = up.last_commit_touching(repo, path)
        if last == commit or up.commits_ahead(repo, commit, last) == 0:
            return Result("current", f"last change to {path}")
        return Result("newer", f"{path} changed in {last[:12]}")

    return check


def pypi_check(package: str, pinned: str) -> Callable[[Upstream], Result]:
    return lambda up: same_or_newer(pinned, up.pypi_latest(package))


def image_check(image: str, tag: str, digest: str) -> Callable[[Upstream], Result]:
    """A digest pin against what the registry serves for its tag today."""

    def check(up: Upstream) -> Result:
        served = up.image_digest(image, tag)
        if served == digest:
            return Result("current", f"{tag} unchanged")
        return Result("newer", f"{tag} is now {served[:19]}")

    return check


def follows(what: str) -> Callable[[Upstream], Result]:
    return lambda up: Result("follows", what)


def fixed(what: str) -> Callable[[Upstream], Result]:
    return lambda up: Result("fixed", what)


# --- Pins ----------------------------------------------------------------------------------


def _one(pattern: str, text: str, where: str) -> str:
    m = re.search(pattern, text, re.MULTILINE)
    if not m:
        raise ValueError(f"{where}: no match for {pattern}")
    return m.group(1)


def cmake_pins(text: str, where: str = DEPENDENCIES_CMAKE) -> list[Pin]:
    """The pins of cmake/Dependencies.cmake."""
    sdl = _one(r"/release-([\d.]+)/SDL3-", text, where)
    nx_commit = _one(r"nx-mod/sdl3-nx/archive/([0-9a-f]{40})\.tar\.gz", text, where)
    nx_branch = _one(r"of branch ([\w.-]+)\.", text, where)
    doctest = _one(r"/download/v([\d.]+)/doctest-", text, where)
    dr_mp3 = _one(r"set\(MAPLETHIEF_DR_MP3_VERSION ([\d.]+)\)", text, where)
    dr_flac = _one(r"set\(MAPLETHIEF_DR_FLAC_VERSION ([\d.]+)\)", text, where)
    stb = _one(r"set\(MAPLETHIEF_STB_COMMIT ([0-9a-f]{40})\)", text, where)
    fonts = _one(r"set\(MAPLETHIEF_FONT_COMMIT ([0-9a-f]{40})\)", text, where)
    w = [where]
    out = [
        Pin("SDL3", sdl, w, release_check("libsdl-org/SDL", sdl, "release-")),
        Pin(
            "SDL3 for Switch (sdl3-nx)",
            nx_commit[:12],
            w,
            branch_check("nx-mod/sdl3-nx", nx_commit, nx_branch),
        ),
        Pin("doctest", doctest, w, release_check("doctest/doctest", doctest, "v")),
        Pin("dr_mp3", dr_mp3, w, tag_check("mackron/dr_libs", dr_mp3, "mp3-")),
        Pin("dr_flac", dr_flac, w, tag_check("mackron/dr_libs", dr_flac, "flac-")),
        Pin("stb", stb[:12], w, branch_check("nothings/stb", stb)),
        Pin("NanumGothic", fonts[:12], w, path_check("google/fonts", fonts, "ofl/nanumgothic")),
        Pin("Fira Sans", fonts[:12], w, path_check("google/fonts", fonts, "ofl/firasans")),
    ]
    for name, url in re.findall(r'"([\w.-]+)\|(https://[^|]+)\|[0-9a-f]{64}"', text):
        m = re.match(r"https://raw\.githubusercontent\.com/([^/]+/[^/]+)/(v[\d.]+)/", url)
        parts = url.split("/")
        ref = "/".join(parts[5:7]) if parts[5] == "releases" else parts[5]  # gcc's tags
        if m:  # a tag: say what upstream has, but the image decides
            repo, tag = m.groups()
            pinned_tag = tag

            def check(up: Upstream, repo=repo, pinned_tag=pinned_tag) -> Result:
                latest = tag_check(repo, pinned_tag.removeprefix("v"), "v")(up)
                return Result("follows", f"devkitA64 image (upstream v{latest.upstream})")

            out.append(Pin(f"Switch licence {name}", tag, w, check))
        else:
            out.append(Pin(f"Switch licence {name}", ref[:19], w, follows("devkitA64 image")))
    return out


def dockerfile_pins(files: dict[str, str]) -> list[Pin]:
    """The ARG pins of the Dockerfiles ({repo-relative path: text}), one row per distinct
    value, naming every file that holds it."""
    seen: dict[tuple[str, str], list[str]] = {}
    for where, text in sorted(files.items()):
        for name, value in re.findall(r"^ARG (\w+)=(\S+)$", text, re.MULTILINE):
            seen.setdefault((name, value), []).append(where)
    out = []
    for (name, value), where in seen.items():
        if "@sha256:" in value:
            ref, digest = value.split("@", 1)
            image, _, tag = ref.partition(":")
            label = f"{image}:{tag}" if tag else image
            out.append(
                Pin(
                    f"image {label}",
                    digest[:19],
                    where,
                    image_check(image, tag or "latest", digest),
                )
            )
        elif name in DOCKER_ARG_RELEASES:
            repo, prefix = DOCKER_ARG_RELEASES[name]
            out.append(Pin(name, value, where, release_check(repo, value, prefix)))
        elif name in DOCKER_ARG_TAGS:
            repo, prefix = DOCKER_ARG_TAGS[name]
            out.append(Pin(name, value, where, tag_check(repo, value, prefix)))
        elif name in DOCKER_ARG_FIXED:
            out.append(Pin(name, value, where, fixed("apt snapshot date")))
        elif DOCKER_ARG_DERIVED.search(name):
            continue
        else:
            raise ValueError(f"{where[0]}: ARG {name} is a pin deps.py doesn't know")
    return out


def lint_pins(text: str, where: str = LINT) -> list[Pin]:
    out = []
    for name, value in re.findall(r"^(\w+)_VERSION=(\S+)$", text, re.MULTILINE):
        package = LINT_PACKAGES.get(name)
        if not package:
            raise ValueError(f"{where}: {name}_VERSION is a pin deps.py doesn't know")
        out.append(Pin(package, value, [where], pypi_check(package, value)))
    schemastore = _one(r"SchemaStore/schemastore/([0-9a-f]{40})/", text, where)
    out.append(
        Pin(
            "SchemaStore schemas",
            schemastore[:12],
            [where],
            branch_check("SchemaStore/schemastore", schemastore),
        )
    )
    return out


def python_pins(text: str, where: str = PYPROJECT) -> list[Pin]:
    """The ``name==version`` pins of pyproject.toml's dependency groups (Dependabot's too)."""
    return [
        Pin(name, version, [f"{where} (Dependabot)"], pypi_check(name, version))
        for name, version in re.findall(r'"([\w.-]+)==([\w.]+)"', text)
    ]


def tool_pins(ci: str, ghidra: str) -> list[Pin]:
    uv = _one(r'^\s*UV_VERSION: "([\d.]+)"', ci, CI_YML)
    ghidra_version = _one(r'^GHIDRA_VERSION = "([\d.]+)"', ghidra, GHIDRA_SETUP)
    jdk = _one(r'^JDK_VERSION = "([\w.+]+)"', ghidra, GHIDRA_SETUP)
    jdk_major = jdk.split(".", 1)[0]

    def ghidra_check(up: Upstream) -> Result:
        tag = up.latest_release("NationalSecurityAgency/ghidra")
        return same_or_newer(ghidra_version, tag.removeprefix("Ghidra_").removesuffix("_build"))

    return [
        Pin("uv", uv, [CI_YML], release_check("astral-sh/uv", uv)),
        Pin("Ghidra", ghidra_version, [GHIDRA_SETUP], ghidra_check),
        Pin(
            "Temurin JDK",
            jdk,
            [GHIDRA_SETUP],
            release_check(f"adoptium/temurin{jdk_major}-binaries", jdk, "jdk-"),
        ),
        Pin("Gradle", "Ghidra's", [GHIDRA_SETUP], follows("Ghidra's gradle wrapper")),
    ]


def pins(repo: Path = REPO) -> list[Pin]:
    def read(rel: str) -> str:
        return (repo / rel).read_text(encoding="utf-8")

    dockerfiles = {
        p.relative_to(repo).as_posix(): p.read_text(encoding="utf-8")
        for p in sorted((repo / CONTAINERS).glob("*/Dockerfile"))
    }
    return (
        cmake_pins(read(DEPENDENCIES_CMAKE))
        + dockerfile_pins(dockerfiles)
        + tool_pins(read(CI_YML), read(GHIDRA_SETUP))
        + lint_pins(read(LINT))
        + python_pins(read(PYPROJECT))
    )


# --- CLI -----------------------------------------------------------------------------------


def run_checks(all_pins: list[Pin], up: Upstream | None) -> list[tuple[Pin, Result]]:
    rows = []
    for pin in all_pins:
        if up is None:
            rows.append((pin, Result("unknown", "not looked up (--offline)")))
            continue
        try:
            rows.append((pin, pin.check(up)))
        except (UpstreamError, KeyError, TypeError, ValueError) as e:
            rows.append((pin, Result("unknown", str(e)[:120])))
    return rows


def render(rows: list[tuple[Pin, Result]]) -> str:
    table = [("pin", "pinned", "state", "upstream", "where")]
    table += [(p.name, p.pinned, r.state, r.upstream, ", ".join(p.where)) for p, r in rows]
    widths = [max(len(row[i]) for row in table) for i in range(4)]
    lines = [
        "  ".join(c.ljust(w) for c, w in zip(row[:4], widths, strict=True)) + "  " + row[4]
        for row in table
    ]
    counts = {s: sum(r.state == s for _, r in rows) for s in STATES}
    lines.append("")
    lines.append(
        f"{len(rows)} pins: "
        + ", ".join(f"{n} {s}" for s, n in counts.items() if n)
        + ". The actions: Dependabot (.github/dependabot.yml)."
    )
    return "\n".join(line.rstrip() for line in lines)


def exit_status(rows: list[tuple[Pin, Result]]) -> int:
    states = {r.state for _, r in rows}
    return 1 if "newer" in states else 2 if "unknown" in states else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="scripts/deps", description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    check = sub.add_parser("check", help="list every pin with its upstream state")
    check.add_argument("--only", default="", help="only the pins whose name contains this")
    check.add_argument("--offline", action="store_true", help="list the pins, ask no one")
    args = parser.parse_args(argv)

    selected = [p for p in pins() if args.only.lower() in p.name.lower()]
    up = None if args.offline else Upstream(token=github_token())
    rows = run_checks(selected, up)
    print(render(rows))
    return 0 if args.offline else exit_status(rows)


if __name__ == "__main__":
    sys.exit(main())
