"""The hand-off checklists (#283, docs/testing/handoffs/): the README's committed issue list and
the device pages' sections agree, and what the pages name exists. No network; the live diff
against GitHub's open needs-human issues runs only with MAPLETHIEF_LIVE_GH=1 and a working gh.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess

import pytest

from mtport import handoffs


def test_the_checklists_match_the_list_and_name_what_exists():
    errors = handoffs.check()
    assert not errors, "\n".join(errors)


def test_the_shared_hand_offs_are_on_two_pages():
    listed = handoffs.committed_list(
        (handoffs.ROOT / handoffs.HANDOFFS / "README.md").read_text(encoding="utf-8")
    )
    assert set(listed[192]) == {"windows", "mac"}
    assert set(listed[198]) == {"windows", "mac"}
    assert set(listed[264]) == {"windows", "mac", "linux"}


@pytest.mark.skipif(
    os.environ.get("MAPLETHIEF_LIVE_GH") != "1" or shutil.which("gh") is None,
    reason="live check: set MAPLETHIEF_LIVE_GH=1 with gh logged in",
)
def test_the_list_covers_every_open_hand_off_on_github():
    readme = (handoffs.ROOT / handoffs.HANDOFFS / "README.md").read_text(encoding="utf-8")
    try:
        open_now = handoffs.open_handoffs()
    except (OSError, subprocess.SubprocessError) as e:
        pytest.skip(f"gh failed: {e}")
    errors, _ = handoffs.live_diff(set(handoffs.committed_list(readme)), open_now)
    assert not errors, "\n".join(errors)


# Synthetic checkouts


@pytest.fixture
def root(tmp_path):
    """A checkout with one preset, one recipe, one script and two device pages in order."""
    (tmp_path / "CMakePresets.json").write_text(
        json.dumps({"version": 6, "configurePresets": [{"name": "mac-debug"}]}), encoding="utf-8"
    )
    (tmp_path / "justfile").write_text("set x := 1\n\nbuild *args:\n    @true\n", encoding="utf-8")
    (tmp_path / "scripts").mkdir()
    (tmp_path / "scripts" / "lint").write_text("", encoding="utf-8")
    pages = tmp_path / handoffs.HANDOFFS
    pages.mkdir(parents=True)
    for device in handoffs.DEVICES:
        (pages / f"{device}.md").write_text("# Page\n\n## Setup\n", encoding="utf-8")
    write_list(tmp_path, "- #10: windows (one)\n- #20: windows, mac (two)\n")
    (pages / "windows.md").write_text(
        "# Windows\n\n## #10: One\n\n1. `scripts/lint`\n\n## #20: Two\n\n"
        "```sh\njust build mac\ncmake --workflow --preset mac-debug\n```\n",
        encoding="utf-8",
    )
    (pages / "mac.md").write_text("# Mac\n\n## #20: Two on the Mac\n", encoding="utf-8")
    return tmp_path


def write_list(root, lines: str) -> None:
    (root / handoffs.HANDOFFS / "README.md").write_text(
        f"# Hand-offs\n\n{handoffs.BEGIN}\n{lines}{handoffs.END}\n", encoding="utf-8"
    )


def append(root, device: str, text: str) -> None:
    page = handoffs.device_page(device, root)
    page.write_text(page.read_text(encoding="utf-8") + text, encoding="utf-8")


def test_a_synthetic_checkout_in_order_passes(root):
    assert handoffs.check(root) == []


def test_a_listed_issue_without_a_section_is_found(root):
    write_list(root, "- #10: windows (one)\n- #20: windows, mac (two)\n- #30: linux (three)\n")
    assert any(e.startswith("#30:") for e in handoffs.check(root))


def test_a_section_on_a_page_the_list_does_not_name_is_found(root):
    append(root, "linux", "\n## #10: One on Linux\n")
    assert any(e.startswith("#10:") for e in handoffs.check(root))


def test_a_section_for_an_unlisted_issue_is_found(root):
    append(root, "iphone", "\n## #40: Four\n")
    assert any("#40" in e and "isn't listed" in e for e in handoffs.check(root))


def test_two_sections_for_one_issue_on_a_page_are_found(root):
    append(root, "mac", "\n## #20: Again\n")
    assert any("2 sections" in e for e in handoffs.check(root))


@pytest.mark.parametrize(
    ("text", "what"),
    [
        ("`scripts/nothing`", "no such path"),
        ("`scripts\\nothing.ps1`", "no such path"),
        ("`cmake --preset win-nothing`", "no such CMake preset"),
        ("`just nothing`", "no such just recipe"),
        ("one — two", "dash U+2014"),
        ("1 – 2", "dash U+2013"),
    ],
)
def test_a_bad_name_or_a_dash_is_found(root, text, what):
    append(root, "mac", f"\n{text}\n")
    assert any(what in e for e in handoffs.check(root)), what


@pytest.mark.parametrize(
    "text",
    [
        "`original/MapleThief-1.5.ipa`",
        "`build/win-player/MapleThief.exe`",
        "`https://example.com/scripts/nothing`",
        "scripts/nothing in prose, not code",
        "`just build`",
        "`MapleThief --preset original --force-crash host`",
    ],
)
def test_names_outside_the_checks_are_not_reported(root, text):
    append(root, "mac", f"\n{text}\n")
    assert handoffs.check(root) == []


def test_the_list_is_parsed():
    readme = f"{handoffs.BEGIN}\n- #1: mac (a)\n\n- #2: windows, linux\n{handoffs.END}"
    assert handoffs.committed_list(readme) == {1: ("mac",), 2: ("windows", "linux")}


@pytest.mark.parametrize(
    "block",
    ["- #1: mac\n- #1: windows\n", "- 1: mac\n", "* #1: mac\n"],
)
def test_a_bad_list_is_refused(block):
    with pytest.raises(ValueError):
        handoffs.committed_list(f"{handoffs.BEGIN}\n{block}{handoffs.END}")


def test_the_live_diff_errs_on_missing_and_notes_extra():
    errors, notes = handoffs.live_diff({1, 2}, {2, 3})
    assert errors == ["#3: open needs-human issue missing from the README"]
    assert notes == ["#1: listed, but not an open needs-human issue"]
