"""The iOS build's configuration (#83, ADR 0136): the presets, the memory model of ADR 0025 and the
CI job. This Mac has no Xcode, so these check the configuration; the build itself is the CI job
``ios`` (and the device, a needs-human check on #83)."""

from __future__ import annotations

import json
import shutil
import subprocess
from pathlib import Path

import pytest

from repo_paths import REPO

PRESETS = json.loads((REPO / "CMakePresets.json").read_text(encoding="utf-8"))
needs_cmake = pytest.mark.skipif(not shutil.which("cmake"), reason="needs cmake")


def configure_preset(name: str) -> dict:
    by_name = {p["name"]: p for p in PRESETS["configurePresets"]}
    preset = dict(by_name[name])
    for parent in (
        preset.get("inherits", [])
        if isinstance(preset.get("inherits"), list)
        else [preset["inherits"]]
        if preset.get("inherits")
        else []
    ):
        base = configure_preset(parent)
        cache = {**base.get("cacheVariables", {}), **preset.get("cacheVariables", {})}
        preset = {**base, **preset, "cacheVariables": cache}
    return preset


@pytest.mark.parametrize("name", ["ios-app", "ios-ci"])
def test_ios_presets_target_ios_16_arm64_through_xcode(name):
    preset = configure_preset(name)
    cache = preset["cacheVariables"]
    assert preset["generator"] == "Xcode"
    assert cache["CMAKE_SYSTEM_NAME"] == "iOS"
    assert cache["CMAKE_OSX_SYSROOT"] == "iphoneos"
    assert cache["CMAKE_OSX_ARCHITECTURES"] == "arm64"
    assert cache["CMAKE_OSX_DEPLOYMENT_TARGET"] == "16.0"
    assert cache["MAPLETHIEF_MEM_MODEL"] == "checked"


def test_ios_app_needs_the_game_and_ios_ci_does_not():
    assert configure_preset("ios-app")["cacheVariables"]["MAPLETHIEF_IOS_APP"] == "ON"
    ci = configure_preset("ios-ci")["cacheVariables"]
    assert "MAPLETHIEF_IOS_APP" not in ci
    assert ci["MAPLETHIEF_GEN_DIR"] == ""
    assert ci["CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED"] == "NO"


def test_ios_presets_build_only_the_game_target():
    build = {p["name"]: p for p in PRESETS["buildPresets"]}
    for name in ("ios-app-debug", "ios-app-release", "ios-ci-debug", "ios-ci-release"):
        assert build[name]["targets"] == ["MapleThief"]
    workflow = {p["name"]: p for p in PRESETS["workflowPresets"]}["ios-ci"]
    assert [s["name"] for s in workflow["steps"]] == ["ios-ci", "ios-ci-debug", "ios-ci-release"]


def test_memory_header_enforces_the_arena_model_on_ios():
    text = (REPO / "runtime/include/maplethief/memory.h").read_text(encoding="utf-8")
    assert "#include <TargetConditionals.h>" in text
    ios = text.index("#if defined(__APPLE__) && TARGET_OS_IPHONE\n#if !MT_MEM_CHECKED")
    arena = text.index("#define MT_MEM_RESERVE_FOR_BUILD MT_MEM_RESERVE_ARENA", ios)
    assert arena < text.index("#elif MT_MEM_CHECKED", ios)
    assert "#error" in text[ios:arena]


def _mem_checked(tmp_path: Path, system: str, model: str) -> subprocess.CompletedProcess:
    script = tmp_path / "probe.cmake"
    script.write_text(
        f'set(CMAKE_SYSTEM_NAME "{system}")\n'
        f'set(MAPLETHIEF_MEM_MODEL "{model}")\n'
        f'include("{(REPO / "cmake/GeneratedCode.cmake").as_posix()}")\n'
        "maplethief_mem_checked(value)\n"
        'message(STATUS "value=${value}")\n',
        encoding="utf-8",
    )
    return subprocess.run(
        ["cmake", "-P", str(script)], capture_output=True, text=True, timeout=30, check=False
    )


@needs_cmake
@pytest.mark.parametrize("model", ["auto", "checked"])
def test_generated_code_is_checked_on_ios(tmp_path, model):
    done = _mem_checked(tmp_path, "iOS", model)
    assert done.returncode == 0, done.stdout + done.stderr
    assert "value=1" in done.stdout


@needs_cmake
def test_reserved_memory_is_a_configure_error_on_ios(tmp_path):
    done = _mem_checked(tmp_path, "iOS", "reserved")
    assert done.returncode != 0
    assert "iOS cannot reserve 4 GiB" in done.stdout + done.stderr


def test_ci_compiles_ios_where_every_os_runs():
    text = (REPO / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    job = text[text.index("\n  ios:\n") : text.index("\n  lint-python:")]
    assert "cmake --workflow --preset ios-ci" in job
    assert "runs-on: macos-latest" in job
    assert "github.event_name == 'workflow_dispatch' || github.base_ref == 'main'" in job
    assert "needs.scope.outputs.native == 'true'" in job
    assert "*ipa-guard" in job
    assert "cmake --preset ios-app" in job  # on a stand-in: no game code or files


def test_no_game_files_in_the_ios_catalog():
    catalog = REPO / "platform/icon/Assets.xcassets"
    names = sorted(p.relative_to(catalog).as_posix() for p in catalog.rglob("*") if p.is_file())
    assert names == [
        "AppIcon.appiconset/Contents.json",
        "AppIcon.appiconset/icon-1024.png",
        "Contents.json",
        "LaunchBackground.colorset/Contents.json",
        "LaunchIcon.imageset/Contents.json",
        "LaunchIcon.imageset/launch-icon.png",
    ]
