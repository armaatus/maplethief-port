"""The release content guard against the real IPA (mtport.release, ADR 0024, ADR 0125).
Skipped when work/ipa/ is absent (CI).

Run scripts/bootstrap first; the dispatch-table test also needs `python3 -m recomp all`
(work/gen/dispatch.c) and skips without it. The player-build test guards every player package
in build/<os>-player/ (and the Linux one in build/linux-game/), and skips when there is none.
"""

from __future__ import annotations

import hashlib
import subprocess
from pathlib import Path

import pytest

from mtport import release as rel
from repo_paths import IPA_DIR, REPO
from test_release import CLEAN, make_zip

pytestmark = pytest.mark.real_assets

# What the kits ship from the repository (cmake/Packaging.cmake), as git pathspecs.
SHIPPED = [
    "README.md",
    "THIRD_PARTY.md",
    "pyproject.toml",
    "uv.lock",
    "docs",
    "recomp",
    "runtime/include",
    "scripts",
    "tools/mtport",
    "tools/ghidra",
]


@pytest.fixture(scope="module")
def ipa_refs() -> dict[str, str]:
    refs = rel.reference_hashes([IPA_DIR])
    assert len(refs) > 1000  # the bundle has thousands of files
    return refs


def test_no_shipped_repo_file_equals_an_ipa_file(ipa_refs):
    files = subprocess.run(
        ["git", "ls-files", "-z", "--", *SHIPPED], cwd=REPO, capture_output=True, check=True
    ).stdout.split(b"\0")
    checked = 0
    for name in filter(None, files):
        data = (REPO / name.decode()).read_bytes()
        if not data:
            continue
        checked += 1
        digest = hashlib.sha256(data).hexdigest()
        assert digest not in ipa_refs, f"{name.decode()} equals {ipa_refs[digest]}"
    assert checked > 100


def test_guard_rejects_a_planted_ipa_file(tmp_path, ipa_refs):
    game = min(
        (p for p in (IPA_DIR / "Payload").rglob("*") if p.is_file() and p.stat().st_size > 0),
        key=lambda p: (p.stat().st_size, p.as_posix()),
    )
    kit = make_zip(tmp_path / "kit.zip", {**CLEAN, "docs/renamed.bin": game.read_bytes()})
    report = rel.guard(kit, ipa_refs, [])
    assert [(f.rule, f.member.endswith("docs/renamed.bin")) for f in report.findings] == [
        ("derived", True)
    ]
    assert rel.guard(make_zip(tmp_path / "clean.zip", CLEAN), ipa_refs, []).ok


def test_guard_finds_the_real_dispatch_table(tmp_path):
    dispatch = REPO / rel.GEN_DISPATCH
    if not dispatch.is_file():
        pytest.skip("work/gen/dispatch.c absent (run python3 -m recomp all)")
    table = rel.dispatch_table_addresses(dispatch)
    assert len(table) == rel.TABLE_RUN
    assert table[0] == 0x27E0  # the entry stub, ARM code (maplethief/recomp/dispatch.h)
    # A "stripped" 64-bit binary: the table's data only, no symbol names.
    data = b"\xcf\xfa\xed\xfe" + b"\0" * 60
    data += b"".join(a.to_bytes(4, "little") + b"\0" * 12 for a in table)
    report = rel.guard(make_zip(tmp_path / "kit.zip", {**CLEAN, "bin/MapleThief": data}), {}, table)
    assert [f.rule for f in report.findings] == ["symbols"]


def _player_builds() -> list[Path]:
    """The player builds made in this checkout (the <os>-player workflows, ADR 0125)."""
    found: list[Path] = []
    for preset in ("win-player", "mac-player", "linux-player", "linux-game"):
        for pattern in ("MapleThief-[0-9]*.zip", "MapleThief-[0-9]*.tar.gz"):
            found += sorted((REPO / "build" / preset).glob(pattern))
    return found


@pytest.mark.parametrize(
    "package", _player_builds() or [None], ids=lambda p: p.parent.name if p else "none"
)
def test_real_player_build(package, ipa_refs):
    """A real player build passes player mode against the IPA's hashes and the dispatch
    table, and fails kit mode because it holds the recompiled code."""
    if package is None:
        pytest.skip("no player build (cmake --workflow --preset <os>-player)")
    dispatch = REPO / rel.GEN_DISPATCH
    table = rel.dispatch_table_addresses(dispatch) if dispatch.is_file() else []
    refs = {**rel.reference_hashes([REPO / "work" / "gen", REPO / "assets-out"]), **ipa_refs}
    player = rel.guard(package, refs, table, "player")
    assert player.ok, player.findings
    assert player.game_code >= 1
    kit = rel.guard(package, refs, table, "kit")
    assert kit.findings and {f.rule for f in kit.findings} == {"symbols"}
