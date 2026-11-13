"""Check the hand-off checklists (#283): ``docs/testing/handoffs/``, one page per device.

The README holds the committed list of hand-off issues between two markers, one line per
issue naming the device pages that cover it::

    <!-- handoffs:begin -->
    - #192: windows, mac (saves in the menu)
    <!-- handoffs:end -->

A device page covers an issue when one of its level-2 headings names it (``## #192: ...``).
:func:`check` compares the two both ways, and checks that what the pages name in code (code
spans and fenced blocks) exists on this branch: repo paths, ``--preset`` names and ``just``
recipes (a ``--preset`` only on a line that runs CMake or one of our build scripts, since
the game takes ``--preset`` as well). Links are left to ``tests/test_doc_links.py``. It needs
no network, so CI runs it.

``--live`` also diffs the committed list against GitHub's open ``needs-human`` issues through
``gh``: an open hand-off missing from the list is an error, a listed issue that is closed or no
longer labelled is reported as a note.

CLI::

    python3 -m mtport.handoffs [--live]

Prints one line per problem and exits 1 if there are any, 2 when ``gh`` fails under ``--live``.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HANDOFFS = Path("docs") / "testing" / "handoffs"
DEVICES = ("windows", "mac", "iphone", "linux", "switch", "playthrough")

BEGIN = "<!-- handoffs:begin -->"
END = "<!-- handoffs:end -->"
LIST_LINE = re.compile(r"^- #(\d+): ([a-z]+(?:, [a-z]+)*)\b")
SECTION = re.compile(r"^## (.*)$")
ISSUE = re.compile(r"#(\d+)\b")
FENCE = re.compile(r"^\s*(```|~~~)")
CODE_SPAN = re.compile(r"(`+)(.+?)\1")
URL = re.compile(r"[a-z][a-z0-9+.-]*://\S+")

# Top-level folders and files of the repository whose paths are checked. Generated folders
# (build/, work/, dist/, assets-out/) are not in git, so their paths are not checked.
TRACKED_TOPS = (
    "scripts",
    "docs",
    "tools",
    "tests",
    "platform",
    "runtime",
    "recomp",
    "cmake",
    "original",
    ".github",
)
REPO_PATH = re.compile(
    r"(?<![\w./\\-])((?:" + "|".join(re.escape(t) for t in TRACKED_TOPS) + r")[/\\][\w.\\/-]*\w)"
)
# Paths the pages name on purpose that are never in git: each user's own copy (ADR 0100).
NOT_IN_GIT = {"original/MapleThief-1.5.ipa"}
PRESET = re.compile(r"(?:--preset|-Preset)[ =]([A-Za-z0-9_-]+)")
# The game takes --preset too (original, enhanced...): only a line that runs a tool taking a
# CMake preset is checked for one.
PRESET_TOOLS = re.compile(r"\b(?:cmake|ctest|build-switch|switch-perf|test\.ps1|scripts/test)\b")
JUST = re.compile(r"(?<![\w-])just ([a-z][\w-]*)")
JUST_RECIPE = re.compile(r"^([a-z][\w-]*)(?: [^:=]*)?:(?!=)")


def device_page(device: str, root: Path = ROOT) -> Path:
    return root / HANDOFFS / f"{device}.md"


def committed_list(readme: str) -> dict[int, tuple[str, ...]]:
    """The README's issue list: issue number to the device pages that cover it."""
    if BEGIN not in readme or END not in readme:
        raise ValueError(f"the README has no {BEGIN} ... {END} list")
    block = readme.split(BEGIN, 1)[1].split(END, 1)[0]
    out: dict[int, tuple[str, ...]] = {}
    for line in block.splitlines():
        if not line.strip():
            continue
        m = LIST_LINE.match(line)
        if not m:
            raise ValueError(f"not a list line: {line!r}")
        n = int(m.group(1))
        if n in out:
            raise ValueError(f"#{n} is listed twice")
        out[n] = tuple(m.group(2).split(", "))
    return out


def split_code(text: str) -> tuple[list[str], list[str]]:
    """The page's lines outside fenced blocks (code spans kept), and its code: every fenced
    line and every code span."""
    prose: list[str] = []
    code: list[str] = []
    fence = ""
    for line in text.splitlines():
        m = FENCE.match(line)
        if m:
            if not fence:
                fence = m.group(1)
            elif fence == m.group(1):
                fence = ""
            continue
        if fence:
            code.append(line)
            continue
        prose.append(line)
        code.extend(s.group(2) for s in CODE_SPAN.finditer(line))
    return prose, code


def sections(text: str) -> dict[int, list[str]]:
    """Issue number to the level-2 headings of the page that name it."""
    out: dict[int, list[str]] = {}
    for line in split_code(text)[0]:
        m = SECTION.match(line)
        if m:
            for n in ISSUE.findall(m.group(1)):
                out.setdefault(int(n), []).append(m.group(1))
    return out


def preset_names(root: Path = ROOT) -> set[str]:
    data = json.loads((root / "CMakePresets.json").read_text(encoding="utf-8"))
    return {p["name"] for key, presets in data.items() if key.endswith("Presets") for p in presets}


def just_recipes(root: Path = ROOT) -> set[str]:
    lines = (root / "justfile").read_text(encoding="utf-8").splitlines()
    return {m.group(1) for line in lines if (m := JUST_RECIPE.match(line)) and m.group(1) != "set"}


def named_problems(name: str, text: str, root: Path = ROOT) -> list[str]:
    """What ``text``'s code names that doesn't exist on this branch."""
    presets = preset_names(root)
    recipes = just_recipes(root)
    errors = []
    for line in split_code(text)[1]:
        line = URL.sub("", line)
        for raw in REPO_PATH.findall(line):
            path = raw.replace("\\", "/")
            if path not in NOT_IN_GIT and not (root / path).exists():
                errors.append(f"{name}: no such path in the repository: {raw}")
        for preset in PRESET.findall(line) if PRESET_TOOLS.search(line) else []:
            if preset not in presets:
                errors.append(f"{name}: no such CMake preset: {preset}")
        for recipe in JUST.findall(line):
            if recipe not in recipes:
                errors.append(f"{name}: no such just recipe: {recipe}")
    return errors


def check(root: Path = ROOT) -> list[str]:
    """Every problem with the checklists, one line each; empty when they are in order."""
    errors: list[str] = []
    readme = (root / HANDOFFS / "README.md").read_text(encoding="utf-8")
    try:
        listed = committed_list(readme)
    except ValueError as e:
        return [f"README.md: {e}"]
    covered: dict[int, set[str]] = {}
    for device in DEVICES:
        page = device_page(device, root)
        if not page.is_file():
            errors.append(f"{device}.md: missing")
            continue
        text = page.read_text(encoding="utf-8")
        for n, headings in sections(text).items():
            if len(headings) > 1:
                errors.append(f"{device}.md: #{n} has {len(headings)} sections; give it one")
            covered.setdefault(n, set()).add(device)
        errors += named_problems(f"{device}.md", text, root)
    for n, devices in sorted(listed.items()):
        unknown = sorted(set(devices) - set(DEVICES))
        if unknown:
            errors.append(f"README.md: #{n} names no such device page: {', '.join(unknown)}")
        got = covered.get(n, set())
        if got != set(devices):
            errors.append(
                f"#{n}: the README lists {', '.join(devices)}, "
                f"sections are in {', '.join(sorted(got)) or 'no page'}"
            )
    for n in sorted(set(covered) - set(listed)):
        errors.append(f"#{n}: has a section in {', '.join(sorted(covered[n]))} but isn't listed")
    for path in sorted((root / HANDOFFS).glob("*.md")):
        for i, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            for dash in "‒–—―":
                if dash in line:
                    errors.append(f"{path.name}:{i}: dash U+{ord(dash):04X}; use a comma or 'to'")
    return errors


def open_handoffs() -> set[int]:
    """GitHub's open issues labelled ``needs-human``, through ``gh`` (raises OSError or
    CalledProcessError when it isn't there or fails)."""
    out = subprocess.run(
        [
            "gh",
            "issue",
            "list",
            "--label",
            "needs-human",
            "--state",
            "open",
            "--limit",
            "500",
            "--json",
            "number",
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=True,
        timeout=60,
    ).stdout
    return {item["number"] for item in json.loads(out)}


def live_diff(listed: set[int], open_now: set[int]) -> tuple[list[str], list[str]]:
    """Errors (open hand-offs missing from the list) and notes (listed, but not open with the
    label)."""
    errors = [
        f"#{n}: open needs-human issue missing from the README" for n in sorted(open_now - listed)
    ]
    notes = [f"#{n}: listed, but not an open needs-human issue" for n in sorted(listed - open_now)]
    return errors, notes


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="mtport.handoffs", description=__doc__.split("\n")[0])
    parser.add_argument("--live", action="store_true", help="also diff against gh's open list")
    args = parser.parse_args(argv)
    errors = check()
    if args.live:
        readme = (ROOT / HANDOFFS / "README.md").read_text(encoding="utf-8")
        try:
            open_now = open_handoffs()
        except (OSError, subprocess.SubprocessError) as e:
            print(f"handoffs: gh failed: {e}", file=sys.stderr)
            return 2
        live_errors, notes = live_diff(set(committed_list(readme)), open_now)
        errors += live_errors
        for note in notes:
            print(f"note: {note}")
    for e in errors:
        print(e)
    print(f"handoffs: {'FAILED' if errors else 'OK'}")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
