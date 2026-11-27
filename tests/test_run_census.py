"""The census of the port's ways into the guest (#405, ADR 0156's addendum,
platform/src/run_census.cpp): every call in platform/src that gives the guest input, calls a guest
function, writes guest memory, hooks a guest function or changes how a touch reaches a view is
listed there as recorded, pure or refused. A new one fails here by file and call until it is
classified; a refused one must call run_refuse with its name, so the recording ends before it."""

from __future__ import annotations

import re

from repo_paths import REPO

SRC = REPO / "platform" / "src"
CENSUS = SRC / "run_census.cpp"

# The runtime's calls that reach the guest from the host. A write through cpu->mem is
# "guest_memory".
CALLS = re.compile(
    r"\b(mt_call_guest|mt_objc_send\w*|mt_input_press|mt_input_release_all|mt_input_release"
    r"|mt_uikit_touch|mt_uikit_lifecycle_event|mt_uikit_set_host_\w+|mt_uikit_set_animation_scale"
    r"|mt_runloop_set_fire_tolerance|mt_widescreen_turn|mt_hooks_install_\w+|mt_hook_register"
    r"|mt_hook_unregister|mt_hook_wrap|mt_hook_unwrap|mt_hook_set_enabled|mt_saves_replace_file"
    r"|mt_snapshot_restore|mt_runtape_poke)\s*\("
)
GUEST_MEMORY = re.compile(r"memcpy\(\s*cpu->mem\s*\+|cpu->mem\[[^\]]*\]\s*=[^=]")
ENTRY = re.compile(r'\{"([\w.]+)",\s*"(\w+)",\s*([RPX]),\s*(nullptr|"[^"]*")')
# What tests the census itself (these files are its table and the recorder).
OWN = {"run_census.cpp", "run_recorder.cpp"}


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def sites() -> set[tuple[str, str]]:
    found: set[tuple[str, str]] = set()
    for path in sorted(SRC.glob("*.cpp")) + sorted(SRC.glob("*.mm")):
        if path.name in OWN:
            continue
        code = strip_comments(path.read_text(encoding="utf-8"))
        for m in CALLS.finditer(code):
            found.add((path.name, m.group(1)))
        if GUEST_MEMORY.search(code):
            found.add((path.name, "guest_memory"))
    return found


def census() -> dict[tuple[str, str], tuple[str, str | None]]:
    out: dict[tuple[str, str], tuple[str, str | None]] = {}
    for m in ENTRY.finditer(CENSUS.read_text(encoding="utf-8")):
        refuse = None if m.group(4) == "nullptr" else m.group(4).strip('"')
        out[(m.group(1), m.group(2))] = (m.group(3), refuse)
    return out


def test_every_way_into_the_guest_is_in_the_census():
    missing = sorted(sites() - census().keys())
    listed = ", ".join(f"{f}: {c}" for f, c in missing)
    assert not missing, f"not in platform/src/run_census.cpp (recorded, pure or refused?): {listed}"


def test_the_census_names_no_site_that_is_gone():
    stale = sorted(census().keys() - sites())
    assert not stale, "in run_census.cpp but no longer in its file: " + ", ".join(
        f"{f}: {c}" for f, c in stale
    )


def test_every_refused_site_ends_the_recording_by_its_name():
    for (file, call), (kind, refuse) in census().items():
        if kind != "X":
            assert refuse is None, f"{file}: {call} is not refused but has a refusal name"
            continue
        assert refuse, f"{file}: {call} is refused without a name"
        code = strip_comments((SRC / file).read_text(encoding="utf-8"))
        call_line = f'run_refuse("{refuse}")'
        assert call_line in code, f"{file} must call {call_line} before {call}"


def test_the_scan_sees_what_it_looks_for():
    found = sites()
    # One of each kind of site, so a broken pattern does not pass by finding nothing.
    for site in [
        ("input.cpp", "mt_uikit_touch"),
        ("lifecycle.cpp", "mt_uikit_lifecycle_event"),
        ("game_speed.cpp", "guest_memory"),
        ("quick_state.cpp", "mt_snapshot_restore"),
        ("autosave.cpp", "mt_call_guest"),
    ]:
        assert site in found, site
    assert len(census()) >= 50
