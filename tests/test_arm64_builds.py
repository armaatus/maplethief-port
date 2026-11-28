"""The Windows ARM64 and Linux aarch64 builds (#362, ADR 0194): their presets, CI's full-profile
matrix, release.yml's kits and the release's platform lists agree. No YAML parser in the test
environment, so the workflows are read as text."""

from __future__ import annotations

import json
import re

import pytest

from mtport import cireport, itch
from mtport import release as rel
from repo_paths import REPO

PRESETS = json.loads((REPO / "CMakePresets.json").read_text(encoding="utf-8"))
WORKFLOWS = REPO / ".github" / "workflows"
ARM64_RUNNERS = ("ubuntu-24.04-arm", "windows-11-arm")


def by_name(kind: str) -> dict[str, dict]:
    return {p["name"]: p for p in PRESETS[kind]}


def cache_variable(name: str, variable: str) -> str | None:
    """A configure preset's cache variable, through its `inherits` chain."""
    presets = by_name("configurePresets")
    preset = presets[name]
    if variable in preset.get("cacheVariables", {}):
        return preset["cacheVariables"][variable]
    parents = preset.get("inherits", [])
    for parent in [parents] if isinstance(parents, str) else parents:
        if (value := cache_variable(parent, variable)) is not None:
            return value
    return None


@pytest.mark.parametrize("os_name", ["win", "linux"])
@pytest.mark.parametrize("kind", ["debug", "release", "player"])
def test_arm64_presets_mirror_the_x64_ones(os_name, kind):
    name, x64 = f"{os_name}-arm64-{kind}", f"{os_name}-{kind}"
    assert cache_variable(name, "MAPLETHIEF_TARGET_ARCH") == "arm64"
    assert cache_variable(x64, "MAPLETHIEF_TARGET_ARCH") is None
    for variable in ("CMAKE_BUILD_TYPE", "MAPLETHIEF_PLAYER_BUILD", "CMAKE_MSVC_RUNTIME_LIBRARY"):
        assert cache_variable(name, variable) == cache_variable(x64, variable), variable
    assert by_name("buildPresets")[name].get("targets") == by_name("buildPresets")[x64].get(
        "targets"
    )
    assert by_name("testPresets")[name]["configurePreset"] == name
    steps = [(s["type"], s["name"]) for s in by_name("workflowPresets")[name]["steps"]]
    x64_types = [s["type"] for s in by_name("workflowPresets")[x64]["steps"]]
    assert steps == [(t, name) for t in x64_types]
    if "package" in x64_types:
        assert by_name("packagePresets")[name]["configurePreset"] == name


def test_the_windows_arm64_preset_wants_an_arm64_developer_environment():
    preset = by_name("configurePresets")["win-arm64-base"]
    assert preset["architecture"] == {"value": "arm64", "strategy": "external"}
    # The x64 PC cross-compiles the player build and can't run it: only the checks that read it.
    test = by_name("testPresets")["win-arm64-player"]
    assert test["filter"]["include"]["name"] == r"^platform\.player_contents$"


def test_cmake_checks_the_target_arch_and_names_windows_kits_from_msvc():
    top = (REPO / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "CMAKE_CXX_COMPILER_ARCHITECTURE_ID" in top
    assert (
        "if(MAPLETHIEF_TARGET_ARCH AND NOT MAPLETHIEF_TARGET_ARCH STREQUAL MAPLETHIEF_ARCH)" in top
    )
    packaging = (REPO / "cmake/Packaging.cmake").read_text(encoding="utf-8")
    windows = packaging.split("elseif(WIN32)", 1)[1].split("else()\n        string", 1)[0]
    code = "\n".join(line for line in windows.splitlines() if not line.lstrip().startswith("#"))
    assert "MAPLETHIEF_ARCH" in code and "CMAKE_SYSTEM_PROCESSOR" not in code


def test_ci_full_profile_runs_the_arm64_runners():
    ci = (WORKFLOWS / "ci.yml").read_text(encoding="utf-8")
    full = re.search(r"\|\| '(\[\"ubuntu-latest\",\"macos-latest\".*?\])'\) }}", ci)
    assert full, "the build matrix's full-profile OS list"
    assert json.loads(full.group(1)) == [
        "ubuntu-latest",
        "macos-latest",
        "windows-latest",
        *ARM64_RUNNERS,
    ]
    # Pull requests into dev keep the cheap profile.
    assert "github.base_ref != 'main' && '[\"ubuntu-latest\"]'" in ci
    # The weekly report expects every job of the full profile.
    for runner in ARM64_RUNNERS:
        for config in ("debug", "release"):
            assert f"{runner} {config}" in cireport.EXPECTED["build"]


def test_release_yml_builds_every_kit_platform():
    text = (WORKFLOWS / "release.yml").read_text(encoding="utf-8")
    kits = text.split("\n  kits:\n", 1)[1]
    entry = r"^          - os: (\S+)\n            kit: (\S+)\n            arch: (\S+)$"
    found = re.findall(entry, kits, re.M)
    assert sorted(kit for _, kit, _ in found) == sorted(f"{p}.{e}" for p, e in rel.KIT_PLATFORMS)
    arch = {os_name: a for os_name, _, a in found}
    assert {r: arch[r] for r in ARM64_RUNNERS} == {r: "arm64" for r in ARM64_RUNNERS}


def test_arm64_downloads_have_release_platforms_and_itch_channels():
    assert ("linux-aarch64", "tar.gz") in rel.KIT_PLATFORMS
    assert ("windows-arm64", "zip") in rel.KIT_PLATFORMS
    assert itch.CHANNELS["linux-arm64"] == "linux-aarch64"
    assert itch.CHANNELS["windows-arm64"] == "windows-arm64"
    release = (REPO / "scripts/release").read_text(encoding="utf-8")
    # publish finds each platform's file by a key that matches only that platform's.
    keys = dict(re.findall(r"^            ([a-z0-9-]+)\) key='([^']+)' ;;$", release, re.M))
    names = rel.kit_names("1.0.0") + rel.player_names("1.0.0")
    for platform, key in keys.items():
        hits = [n for n in names if re.search(key.replace("\\.", r"\."), n)]
        expected = 1 if platform == "switch" else 2  # its kit and its player build
        assert len(hits) == expected, (platform, hits)
