"""The sanitizer presets (#321, ADR 0157): linux-asan and mac-asan build with ASan and UBSan, no
recovery, the guest memory model checked, and CI runs linux-asan. A suppression file in
tests/sanitizers/ gives the reason for every entry in a comment just above it."""

from __future__ import annotations

import json
import re

from repo_paths import REPO

SUPPRESSIONS = REPO / "tests" / "sanitizers"


def unexplained(text: str) -> list[str]:
    """The entries of a suppression file that have no comment line directly above them (an
    entry may share the comment of the entry right before it)."""
    missing = []
    reason = False
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped:
            reason = False
        elif stripped.startswith("#"):
            reason = len(stripped.lstrip("#").strip()) > 0
        elif not reason:
            missing.append(stripped)
    return missing


def test_an_entry_needs_a_reason_above_it() -> None:
    assert unexplained("# SDL's X11 init leaks its display (upstream #1)\nleak:X11_Open\n") == []
    assert unexplained("leak:X11_Open\n") == ["leak:X11_Open"]
    assert unexplained("#\nleak:X11_Open\n") == ["leak:X11_Open"]
    assert unexplained("# reason\n\nleak:X11_Open\n") == ["leak:X11_Open"]
    assert unexplained("# two from one cause\nleak:a\nleak:b\n") == []


def test_every_suppression_has_its_reason() -> None:
    for path in sorted(SUPPRESSIONS.glob("*.supp")) if SUPPRESSIONS.is_dir() else []:
        assert unexplained(path.read_text(encoding="utf-8")) == [], path.name


def test_the_presets_sanitize_with_the_checked_memory_model() -> None:
    presets = json.loads((REPO / "CMakePresets.json").read_text(encoding="utf-8"))
    configure = {p["name"]: p for p in presets["configurePresets"]}
    for name in ("linux-asan", "mac-asan"):
        cache = configure[name]["cacheVariables"]
        assert cache["MAPLETHIEF_SANITIZE"] == "ON", name
        assert cache["MAPLETHIEF_MEM_MODEL"] == "checked", name
        assert cache["CMAKE_BUILD_TYPE"] == "Debug", name
    tests = {p["name"]: p for p in presets["testPresets"]}
    env = tests["asan-base"]["environment"]
    assert "halt_on_error=1" in env["ASAN_OPTIONS"]
    # LeakSanitizer is on (#414): Linux's default, never turned off.
    assert "detect_leaks=0" not in env["ASAN_OPTIONS"]
    assert "detect_leaks=0" not in env.get("LSAN_OPTIONS", "")
    assert "halt_on_error=1" in env["UBSAN_OPTIONS"]
    for name in ("linux-asan", "mac-asan"):
        assert tests[name]["inherits"] == "asan-base"
    cmake = (REPO / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "-fno-sanitize-recover=all" in cmake


def test_ci_runs_the_linux_asan_workflow() -> None:
    ci = (REPO / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
    job = re.search(r"\n  asan:\n(.*?)\n  \w[\w-]*:\n", ci, re.S)
    assert job is not None
    assert "needs.scope.outputs.native == 'true'" in job.group(1)
    assert "cmake --workflow --preset linux-asan" in job.group(1)
