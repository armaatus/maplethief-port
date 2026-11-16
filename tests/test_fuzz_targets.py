"""The fuzz targets of tests/fuzz/ (#322, ADR 0158) hang together: each has its harness, its
seed module and its CMake line, and its seeds build (synthetic, within MAX_LEN)."""

from __future__ import annotations

import re
import sys

import pytest

from repo_paths import REPO

FUZZ = REPO / "tests" / "fuzz"
sys.path.insert(0, str(FUZZ))
import make_corpus  # noqa: E402


def harnesses() -> set[str]:
    return {p.name.removesuffix("_fuzz.cpp") for p in FUZZ.glob("*_fuzz.cpp")}


def registered() -> set[str]:
    text = (FUZZ / "CMakeLists.txt").read_text(encoding="utf-8")
    return set(re.findall(r"^maplethief_add_fuzz_target\((\w+)", text, re.MULTILINE))


def test_every_target_has_a_harness_seeds_and_a_cmake_line():
    seeds = set(make_corpus.targets())
    assert seeds, "no seed modules in tests/fuzz/seeds/"
    assert harnesses() == seeds
    assert registered() == seeds


def test_322s_parsers_all_have_targets():
    # The parsers of #322's list: zip and the IPA import, PNG (CG, texture packs, the border),
    # WAV, MP3, FLAC, .strings, settings.ini, bindings.ini.
    want = {"zip", "ipa", "ipa_parts", "png", "texpack", "border", "wav", "mp3", "flac",
            "strings", "settings", "bindings"}  # fmt: skip
    assert want <= registered()


def test_every_dictionary_belongs_to_a_target():
    for d in FUZZ.glob("*.dict"):
        assert d.stem in registered(), d.name


@pytest.mark.parametrize("name", make_corpus.targets())
def test_the_seeds_build(name, tmp_path):
    out = tmp_path / name
    n = make_corpus.write(name, out)
    assert n == len(list(out.iterdir())) > 0
    assert all(len(p.read_bytes()) <= make_corpus.max_len(name) for p in out.iterdir())
    # The same bytes every time, so a corpus and its CI smoke are reproducible.
    assert make_corpus.corpus(name) == make_corpus.corpus(name)
