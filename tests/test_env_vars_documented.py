"""Every ``MAPLETHIEF_*`` environment variable the program reads is in the table of
docs/build/running.md (#333), and the table names no variable the program doesn't read.

The program's variables are the string literals ``"MAPLETHIEF_..."`` in the C/C++ sources of
``runtime/`` and ``platform/src/`` (``getenv`` and ``SDL_getenv`` take a literal, or a macro or
constant defined as one), and the ``${MAPLETHIEF_...`` expansions of the shipped launcher
scripts in ``platform/``. Header guards and CMake variables are never string literals there. The
build's own variables (``MAPLETHIEF_VERSION``, ``MAPLETHIEF_GEN_DIR``, ...) are on
docs/build/README.md and are not checked here.
"""

from __future__ import annotations

import re
import subprocess

from repo_paths import REPO

DOC = REPO / "docs" / "build" / "running.md"
C_SOURCES = ("*.c", "*.cpp", "*.h", "*.hpp", "*.m", "*.mm", "*.def")
LITERAL = re.compile(r'"(MAPLETHIEF_[A-Z0-9_]+)"')
SHELL_EXPANSION = re.compile(r"\$\{(MAPLETHIEF_[A-Z0-9_]+)")
# A row of the table: | `MAPLETHIEF_NAME` or | `MAPLETHIEF_NAME=VALUE`
ROW = re.compile(r"^\|\s*`(MAPLETHIEF_[A-Z0-9_]+)[=`]")


def tracked(*pathspecs: str) -> list[str]:
    out = subprocess.run(
        ["git", "ls-files", "-z", "--", *pathspecs],
        cwd=REPO,
        capture_output=True,
        check=True,
    ).stdout
    return [n for n in out.decode("utf-8").split("\0") if n]


def read_by_code() -> dict[str, list[str]]:
    """Each variable the program reads, with the files that name it."""
    found: dict[str, list[str]] = {}
    specs = [f"{d}/**/{g}" for d in ("runtime", "platform/src") for g in C_SOURCES]
    specs += [f"{d}/{g}" for d in ("runtime", "platform/src") for g in C_SOURCES]
    for name in sorted(set(tracked(*specs))):
        for var in LITERAL.findall((REPO / name).read_text(encoding="utf-8")):
            found.setdefault(var, []).append(name)
    for name in tracked("platform/**/*.sh", "platform/*.sh"):
        for var in SHELL_EXPANSION.findall((REPO / name).read_text(encoding="utf-8")):
            found.setdefault(var, []).append(name)
    return found


def documented() -> set[str]:
    text = DOC.read_text(encoding="utf-8")
    section = text.split("\n## Environment variables\n", 1)[1].split("\n## ", 1)[0]
    return {m.group(1) for line in section.splitlines() if (m := ROW.match(line))}


def test_the_scan_finds_the_known_variables():
    # Guards the scan itself: an empty or shrunken result would pass the checks below.
    found = read_by_code()
    for var in (
        "MAPLETHIEF_DATA_DIR",  # platform/src, a literal
        "MAPLETHIEF_SYMBOLS",  # runtime/include, a macro
        "MAPLETHIEF_CRASH_DIALOG_LOG",  # platform/src, a constexpr
        "MAPLETHIEF_GAMESCOPE_WINDOW",  # platform/linux/MapleThief.sh
    ):
        assert var in found, var


def test_every_variable_the_program_reads_is_in_the_table():
    missing = {v: files for v, files in read_by_code().items() if v not in documented()}
    assert not missing, (
        f"add these to the table under '## Environment variables' in {DOC.relative_to(REPO)}: "
        f"{missing}"
    )


def test_the_table_names_only_variables_the_program_reads():
    stale = documented() - set(read_by_code())
    assert not stale, f"no code reads these any more; remove them from the table: {sorted(stale)}"
