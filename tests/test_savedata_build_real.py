"""Built start states equal the chained run's saves (#297, docs/testing/start-states.md).

The start of chapter 2 (tests/starts/chapter_2.json) is built with the shipped bundle and
compared with part L's save of the chapter-1 chain (``play.chapter_1_l``) in every byte the game
writes. Local only: it needs the bundle and the chain's output
(build/test-output/mac-debug/play_chapter_1_l, or the directory ``MT_CHAIN_OUTPUT`` names).
"""

import json
import os
from pathlib import Path

import pytest

from mtport import savedata as sd
from repo_paths import REPO

pytestmark = pytest.mark.real_assets

STARTS = REPO / "tests" / "starts"
CHAIN = REPO / "build" / "test-output" / "mac-debug" / "play_chapter_1_l"


@pytest.fixture(scope="module")
def bundle(ipa_dir):
    app = ipa_dir / "Payload" / "MapleThief.app"
    if not (app / "English" / "data").is_dir():
        pytest.skip("work/ipa/ absent (run scripts/bootstrap)")
    return sd.bundle_inputs(app, "en")


def chained_saves() -> Path:
    out = Path(os.environ.get("MT_CHAIN_OUTPUT", CHAIN)) / "data" / "saves"
    if not (out / "theif0").is_file() or not (out / "quest0").is_file():
        pytest.skip(f"{out}: no save of the chain (ctest --preset mac-debug -R play.chapter_1)")
    return out


def test_chapter_2_start_equals_part_l(bundle):
    skills, strings = bundle
    desc = json.loads((STARTS / "chapter_2.json").read_text(encoding="utf-8"))
    built = sd.build_files(desc, 0, skills, strings)
    chained = chained_saves()
    problems = []
    for name in ("theif0", "quest0"):
        problems += sd.written_diff(name, built[name], (chained / name).read_bytes())
    assert problems == [], "\n".join(problems)
    character = sd.parse_character(built["theif0"])
    quest = sd.parse_quest(built["quest0"])
    assert (quest.chapter, character.area, character.map) == (2, 4, 1)
    assert quest.flags_a[40] == 1


def test_every_start_builds(bundle):
    skills, strings = bundle
    for path in sorted(STARTS.glob("*.json")):
        desc = json.loads(path.read_text(encoding="utf-8"))
        assert sorted(sd.build_files(desc, 0, skills, strings)) == ["quest0", "theif0"], path
