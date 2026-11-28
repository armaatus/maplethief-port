"""Dependency updates (#340): Dependabot's config, and ``scripts/deps check``'s pins and lookups
(tools/mtport/deps.py). The lookups run against a fake upstream; the real one is a hand run."""

from __future__ import annotations

import json
import re

import pytest

from mtport import deps
from repo_paths import REPO

# --- .github/dependabot.yml ------------------------------------------------------------------

DEPENDABOT = REPO / ".github" / "dependabot.yml"


def updates() -> dict[str, str]:
    """Each update entry's YAML text, by its package-ecosystem."""
    text = DEPENDABOT.read_text(encoding="utf-8")
    entries = re.split(r"^  - ", text.split("\nupdates:\n", 1)[1], flags=re.MULTILINE)
    out = {}
    for entry in entries:
        m = re.match(r"package-ecosystem: (\S+)", entry)
        if m:
            out[m.group(1)] = entry
    return out


def test_dependabot_watches_the_actions_and_uv():
    assert set(updates()) == {"github-actions", "uv"}


@pytest.mark.parametrize("ecosystem", ["github-actions", "uv"])
def test_dependabot_runs_weekly_grouped_into_dev(ecosystem):
    entry = updates()[ecosystem]
    assert re.search(r"^\s+interval: weekly$", entry, re.MULTILINE)
    assert re.search(r"^\s+target-branch: dev$", entry, re.MULTILINE)
    # One group matching everything: one pull request per ecosystem and week.
    assert re.search(
        r"^    groups:\n      \S+:\n        patterns: \[\"\*\"\]$", entry, re.MULTILINE
    )


@pytest.mark.parametrize("ecosystem", ["github-actions", "uv"])
def test_dependabot_labels_with_an_existing_label(ecosystem):
    # Dependabot only applies labels the repository has, and the repository has no
    # "dependencies" label (Dependabot's default): area:infra is one of the board's own.
    assert re.search(r'^    labels: \["area:infra"\]$', updates()[ecosystem], re.MULTILINE)


def test_lint_checks_dependabot_against_its_schema():
    lint = (REPO / "scripts" / "lint").read_text(encoding="utf-8")
    assert "dependabot-2.0.json" in lint and ".github/dependabot.yml" in lint


# --- The pins ----------------------------------------------------------------------------------


def by_name(pins: list[deps.Pin]) -> dict[str, deps.Pin]:
    return {p.name: p for p in pins}


def test_every_pin_of_the_repository_is_found():
    pins = by_name(deps.pins())
    for name in (
        "SDL3",
        "SDL3 for Switch (sdl3-nx)",
        "doctest",
        "dr_mp3",
        "dr_flac",
        "stb",
        "NanumGothic",
        "Fira Sans",
        "image ubuntu:26.04",
        "image ubuntu:22.04",
        "image devkitpro/devkita64",
        "image ghcr.io/astral-sh/uv:0.11.7",
        "CMAKE_VERSION",
        "PWSH_VERSION",
        "DXC_TAG",
        "APPIMAGE_RUNTIME_RELEASE",
        "uv",
        "Ghidra",
        "Temurin JDK",
        "ruff",
        "check-jsonschema",
        "SchemaStore schemas",
        "capstone",
        "pytest",
    ):
        assert name in pins, name
    assert all(p.pinned for p in pins.values())


def test_every_switch_licence_pin_is_listed():
    text = (REPO / deps.DEPENDENCIES_CMAKE).read_text(encoding="utf-8")
    block = text.split("set(MAPLETHIEF_SWITCH_LICENSE_PINS", 1)[1].split("\n\n", 1)[0]
    files = re.findall(r'"([\w.-]+)\|https://', block)
    assert len(files) == 7
    names = by_name(deps.pins())
    for f in files:
        assert f"Switch licence {f}" in names


def test_every_dockerfile_arg_is_a_pin_or_derived_from_one():
    # dockerfile_pins raises on an ARG it doesn't know, so a new pin can't be missed.
    files = {
        p.relative_to(REPO).as_posix(): p.read_text(encoding="utf-8")
        for p in (REPO / deps.CONTAINERS).glob("*/Dockerfile")
    }
    assert len(files) >= 5
    images = [p for p in deps.dockerfile_pins(files) if p.name.startswith("image ")]
    digests = {m for text in files.values() for m in re.findall(r"@(sha256:[0-9a-f]{64})", text)}
    assert {p.pinned for p in images} == {d[:19] for d in digests}


def test_every_appimage_licence_pin_is_listed():
    text = (REPO / deps.DEPENDENCIES_CMAKE).read_text(encoding="utf-8")
    files = [n for n, _ in deps.licence_pins(text, "MAPLETHIEF_APPIMAGE_LICENSE_PINS", "x")]
    assert len(files) == 7
    names = by_name(deps.pins())
    assert names["AppImage licence musl.txt"].pinned == "v1.2.5"  # cgit's h=
    assert names["AppImage licence libfuse-LGPL-2.1.txt"].pinned == "fuse-3.15.0"
    for f in files:
        assert f"AppImage licence {f}" in names
    # Neither list takes the other's items.
    assert not any(n.startswith("Switch licence AppImage") for n in names)


def test_an_unknown_dockerfile_arg_fails():
    with pytest.raises(ValueError, match="FOO_VERSION"):
        deps.dockerfile_pins({"x/Dockerfile": "ARG FOO_VERSION=1.0\n"})


def test_a_digest_shared_by_two_dockerfiles_is_one_row():
    image = "ARG BASE=ubuntu:26.04@sha256:" + "a" * 64 + "\n"
    pins = deps.dockerfile_pins({"a/Dockerfile": image, "b/Dockerfile": image})
    assert [(p.name, p.where) for p in pins] == [
        ("image ubuntu:26.04", ["a/Dockerfile", "b/Dockerfile"])
    ]


def test_an_unknown_lint_version_fails():
    with pytest.raises(ValueError, match="NEWTOOL_VERSION"):
        deps.lint_pins("NEWTOOL_VERSION=1.0\n")


def test_split_image():
    assert deps.split_image("ubuntu") == ("docker.io", "library/ubuntu")
    assert deps.split_image("devkitpro/devkita64") == ("docker.io", "devkitpro/devkita64")
    assert deps.split_image("ghcr.io/astral-sh/uv") == ("ghcr.io", "astral-sh/uv")


# --- The lookups, against a fake upstream ------------------------------------------------------


class FakeFetch:
    """Answers by URL substring; every request is recorded."""

    def __init__(self, answers: dict[str, object]) -> None:
        self.answers = answers
        self.urls: list[str] = []

    def __call__(self, url, headers, method):
        self.urls.append(url)
        for key, value in self.answers.items():
            if key in url:
                if isinstance(value, Exception):
                    raise value
                if method == "HEAD":
                    return {"docker-content-digest": value}, b""
                return {}, json.dumps(value).encode()
        raise deps.UpstreamError(f"no answer for {url}")


def run(check, answers):
    return check(deps.Upstream(FakeFetch(answers)))


def test_a_newer_release():
    check = deps.release_check("libsdl-org/SDL", "3.4.18", "release-")
    assert run(check, {"releases/latest": {"tag_name": "release-3.4.20"}}) == deps.Result(
        "newer", "3.4.20"
    )
    assert run(check, {"releases/latest": {"tag_name": "release-3.4.18"}}).state == "current"


def test_a_tag_check_takes_the_highest_matching_version():
    tags = [{"name": n} for n in ("mp3-0.7.3", "flac-0.13.4", "mp3-0.7.10", "mp3-0.7.4", "x")]
    assert run(deps.tag_check("mackron/dr_libs", "0.7.3", "mp3-"), {"/tags": tags}) == (
        deps.Result("newer", "0.7.10")
    )


def test_a_rolling_release_is_not_a_version():
    check = deps.release_check("AppImage/type2-runtime", "20251108")
    with pytest.raises(deps.UpstreamError, match="continuous"):
        run(check, {"releases/latest": {"tag_name": "continuous"}})


def test_a_commit_pin_against_its_branch():
    check = deps.branch_check("nothings/stb", "a" * 40)
    answers = {
        "repos/nothings/stb/compare": {"ahead_by": 3},
        "repos/nothings/stb": {"default_branch": "master"},
    }
    assert run(check, answers) == deps.Result("newer", "master is 3 commits ahead")
    answers["repos/nothings/stb/compare"] = {"ahead_by": 0}
    assert run(check, answers).state == "current"


def test_a_path_pin_is_current_until_a_later_commit_touches_the_path():
    pinned = "a" * 40
    check = deps.path_check("google/fonts", pinned, "ofl/firasans")
    assert run(check, {"commits?path=": [{"sha": pinned}]}).state == "current"
    answers = {"commits?path=": [{"sha": "b" * 40}], "/compare/": {"ahead_by": 0}}
    assert run(check, answers).state == "current"  # the last change is older than the pin
    answers["/compare/"] = {"ahead_by": 2}
    assert run(check, answers) == deps.Result("newer", "ofl/firasans changed in bbbbbbbbbbbb")


def test_an_image_digest_against_its_tag():
    digest = "sha256:" + "1" * 64
    check = deps.image_check("ubuntu", "26.04", digest)
    answers = {"auth.docker.io": {"token": "t"}, "registry-1.docker.io/v2/library/ubuntu": digest}
    fetch = FakeFetch(answers)
    assert check(deps.Upstream(fetch)) == deps.Result("current", "26.04 unchanged")
    assert fetch.urls[-1] == "https://registry-1.docker.io/v2/library/ubuntu/manifests/26.04"
    answers["registry-1.docker.io/v2/library/ubuntu"] = "sha256:" + "2" * 64
    assert run(check, answers).state == "newer"


def test_ghcr_uses_its_own_token_endpoint():
    digest = "sha256:" + "3" * 64
    fetch = FakeFetch({"ghcr.io/token": {"token": "t"}, "ghcr.io/v2/astral-sh/uv": digest})
    deps.image_check("ghcr.io/astral-sh/uv", "0.11.7", digest)(deps.Upstream(fetch))
    assert fetch.urls[0].startswith("https://ghcr.io/token?scope=repository:astral-sh/uv:pull")


def test_a_failed_lookup_is_unknown_and_the_rest_still_runs():
    pins = [
        deps.Pin("a", "1.0", ["x"], deps.pypi_check("a", "1.0")),
        deps.Pin("b", "1.0", ["x"], deps.pypi_check("b", "1.0")),
    ]
    fetch = FakeFetch(
        {"pypi/a/": deps.UpstreamError("offline"), "pypi/b/": {"info": {"version": "1.0"}}}
    )
    rows = deps.run_checks(pins, deps.Upstream(fetch))
    assert [r.state for _, r in rows] == ["unknown", "current"]
    assert deps.exit_status(rows) == 2


def test_exit_status_and_summary():
    pin = deps.Pin("p", "1", ["f"], deps.fixed("x"))
    newer = [(pin, deps.Result("newer", "2")), (pin, deps.Result("unknown", "?"))]
    assert deps.exit_status(newer) == 1
    assert deps.exit_status([(pin, deps.Result("current", "1"))]) == 0
    text = deps.render(newer)
    assert text.splitlines()[0].split() == ["pin", "pinned", "state", "upstream", "where"]
    assert "2 pins: 1 newer, 1 unknown." in text


def test_offline_lists_every_pin_without_a_request(capsys):
    assert deps.main(["check", "--offline"]) == 0
    out = capsys.readouterr().out
    assert len(deps.pins()) == int(re.search(r"^(\d+) pins:", out, re.MULTILINE).group(1))


# --- THIRD_PARTY.md, the other half of a bump ------------------------------------------------


def test_every_cmake_hash_is_in_third_party_md():
    # A bump changes the pin and its row in one commit (docs/build/how-to/bump-a-dependency.md).
    # The Switch's licence texts are named by file in THIRD_PARTY.md's prose, not by hash.
    text = (REPO / deps.DEPENDENCIES_CMAKE).read_text(encoding="utf-8")
    text = re.sub(r"set\(MAPLETHIEF_SWITCH_LICENSE_PINS.*?\)\n", "", text, flags=re.DOTALL)
    third_party = (REPO / "THIRD_PARTY.md").read_text(encoding="utf-8")
    hashes = set(re.findall(r"\b[0-9a-f]{64}\b", text))
    assert len(hashes) >= 10
    assert sorted(h for h in hashes if h not in third_party) == []


def test_every_image_digest_is_in_third_party_md():
    third_party = (REPO / "THIRD_PARTY.md").read_text(encoding="utf-8")
    for path in (REPO / deps.CONTAINERS).glob("*/Dockerfile"):
        for digest in re.findall(r"@(sha256:[0-9a-f]{64})", path.read_text(encoding="utf-8")):
            assert digest in third_party, f"{path.parent.name}: {digest}"


def dockerfiles() -> dict[str, str]:
    return {
        p.relative_to(REPO).as_posix(): p.read_text(encoding="utf-8")
        for p in (REPO / deps.CONTAINERS).glob("*/Dockerfile")
    }


def test_every_packaged_download_pin_is_in_third_party_md():
    # A download that ships in a player download (the AppImage's runtime, #439) needs its row
    # and licence in THIRD_PARTY.md, with the release and every SHA-256 the build checks.
    third_party = (REPO / "THIRD_PARTY.md").read_text(encoding="utf-8")
    files = dockerfiles()
    packaged = [
        name
        for text in files.values()
        for name in re.findall(r"^ARG (\w+)=", text, re.MULTILINE)
        if deps.DOCKER_ARG_PACKAGED.search(name)
    ]
    assert sorted(packaged) == [
        "APPIMAGE_RUNTIME_RELEASE",
        "APPIMAGE_RUNTIME_SHA256_AMD64",
        "APPIMAGE_RUNTIME_SHA256_ARM64",
    ]
    assert deps.packaged_missing_from(third_party, files) == []


def test_a_packaged_download_missing_from_third_party_md_fails():
    files = {
        "x/Dockerfile": "ARG APPIMAGE_RUNTIME_RELEASE=20990101\n"
        "ARG APPIMAGE_RUNTIME_SHA256_AMD64=" + "c" * 64 + "\n"
        "ARG CMAKE_VERSION=9.9.9\n"
    }
    third_party = "| AppImage runtime | 20990101 | ... |\n"
    assert deps.packaged_missing_from(third_party, files) == [
        "x/Dockerfile: APPIMAGE_RUNTIME_SHA256_AMD64=" + "c" * 64
    ]
    assert deps.packaged_missing_from(third_party + "c" * 64, files) == []


def test_the_guard_accepts_exactly_the_pinned_appimage_runtimes():
    from mtport import release

    text = dockerfiles()["tools/containers/linux-game/Dockerfile"]
    pinned = set(
        re.findall(r"^ARG APPIMAGE_RUNTIME_SHA256_\w+=([0-9a-f]{64})$", text, re.MULTILINE)
    )
    assert len(pinned) == 2
    assert set(release.APPIMAGE_RUNTIMES) == pinned


def test_dependabot_leaves_capstones_major_versions_alone():
    # ADR 0003 addendum of 2026-11-28: Capstone 6 is a move of its own, not a weekly bump.
    entry = updates()["uv"]
    assert re.search(
        r"^      - dependency-name: capstone\n        update-types: "
        r'\["version-update:semver-major"\]$',
        entry,
        re.MULTILINE,
    )
