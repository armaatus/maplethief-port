"""Acceptance (#29): docs/re/audio.md against the shipped sound files, the armv7 slice and the
Ghidra export.

Skipped when work/ipa/ is absent (CI), and per test when the slice or the RE pipeline outputs
are missing (run scripts/bootstrap, then scripts/re-analyze).
"""

from __future__ import annotations

import json
from collections import Counter

import pytest

from mtport import audio, macho
from mtport import names as names_mod
from repo_paths import REPO, assert_committed_doc

pytestmark = pytest.mark.real_assets


def _need(*rels: str) -> None:
    for rel in rels:
        if not (REPO / rel).exists():
            pytest.skip(f"{rel} absent (run scripts/bootstrap and scripts/re-analyze)")


@pytest.fixture(scope="module")
def sounds() -> list[audio.Sound]:
    _need(audio.SOUND_DIR)
    return audio.inventory(REPO / audio.SOUND_DIR)


@pytest.fixture(scope="module")
def built():
    _need(audio.SOUND_DIR, audio.SLICE, audio.FUNCTIONS_JSON, audio.XREFS_JSON)
    return audio.build(REPO)


def test_all_44_files_parse_and_classify(sounds):
    """Every file decodes; BGM ids are exactly the .mp3 files and SFX ids the .wav files."""
    assert len(sounds) == 44
    kinds = Counter(s.kind for s in sounds)
    assert kinds == {"BGM": 18, "SFX": 26}
    for s in sounds:
        assert isinstance(s.info, audio.Mp3Info if s.kind == "BGM" else audio.WavInfo), s.file
        assert s.info.duration > 0


def test_formats_match_the_doc(sounds):
    wavs = [s.info for s in sounds if s.kind == "SFX"]
    assert all(w.format_name == "PCM" and w.bits_per_sample == 16 for w in wavs)
    assert Counter((w.sample_rate, w.channels) for w in wavs) == {
        (8000, 1): 13,
        (22050, 1): 11,
        (44100, 2): 2,
    }
    mp3s = {s.id: s.info for s in sounds if s.kind == "BGM"}
    for i, m in mp3s.items():
        mpeg1 = i not in range(35, 42)
        assert (m.first.version, m.first.sample_rate) == (
            ("MPEG-1", 44100) if mpeg1 else ("MPEG-2", 22050)
        ), i
        assert m.bitrates == ((128,) if mpeg1 else (80,)) and m.first.channel_mode == "joint stereo"
        assert m.info_tag is None and m.encoder is None and m.trailing == 0
    assert mp3s[42].title == "Flaming Racoon" and mp3s[36].title == "To Kerning City"


def test_bgm_list_is_what_the_code_compares():
    _need(audio.SLICE)
    image = macho.load(REPO / audio.SLICE)
    assert audio.bgm_ids_in_code(image) == audio.BGM_IDS


def test_generated_blocks_are_current(built):
    """The committed doc equals what `python3 -m mtport.audio generate` writes (which also runs
    check(): constant ids, the map transcription, armscan == Ghidra call sites)."""
    _, _, doc = built
    assert_committed_doc(audio.DOC, doc, "python3 -m mtport.audio generate")


def test_every_call_and_scene_command_is_in_the_doc(built):
    _, got, _ = built
    text = (REPO / audio.DOC).read_text(encoding="utf-8")
    for c in got.calls:
        assert f"`0x{c.site:X}`" in text, c
    for s in got.scene:
        assert f"`0x{s.site:X}`" in text, s
    assert got.ghidra_only == [] and got.armscan_only == []
    assert len(got.calls) >= 150 and len(got.scene) >= 70


def test_only_42_is_never_played(built, sounds):
    _, got, _ = built
    played = set(audio.users_of(got))
    assert {s.id for s in sounds} - played == {42}


def test_names_file_is_current_and_applied():
    _need(audio.NAMES_OUT, audio.FUNCTIONS_JSON)
    assert (REPO / audio.NAMES_OUT).read_text(encoding="utf-8") == audio.names_json()
    names_mod.load_dir(REPO / names_mod.NAMES_DIR)  # merges with every other names file
    export = json.loads((REPO / audio.FUNCTIONS_JSON).read_text(encoding="utf-8"))["functions"]
    by_address = {int(f["address"], 16): f["name"] for f in export}
    for address, kind, name, _ in audio.NAMES:
        if kind == "function":
            assert by_address.get(int(address, 16)) == name, (address, name)
    for address, name in audio.NAMED_BY_FLOW.items():
        assert by_address.get(address) == name
