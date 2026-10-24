"""Parse every shipped .sprprj (docs/formats/sprprj.md -> Validation).

Skipped when work/ipa/ is absent (CI); run scripts/bootstrap first.
"""

import struct
from collections import Counter

import pytest

from mtport import sprprj
from repo_paths import IPA_DIR

pytestmark = pytest.mark.real_assets

SPRITE_DIR = IPA_DIR / "Payload" / "MapleThief.app" / "sprite"
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


@pytest.fixture(scope="module")
def projects():
    paths = sprprj.find_projects(SPRITE_DIR)
    # parse() raises on any leftover byte, so a clean parse means the whole file was used.
    return {p: sprprj.load(p) for p in paths}


def png_chunks(data: bytes) -> dict[bytes, bytes]:
    assert data[:8] == PNG_SIGNATURE
    chunks, pos = {}, 8
    while pos + 8 <= len(data):
        (length,) = struct.unpack_from(">I", data, pos)
        chunks.setdefault(data[pos + 4 : pos + 8], data[pos + 8 : pos + 8 + length])
        pos += 12 + length
    return chunks


def test_all_138_files_parse_with_no_leftover_bytes(projects):
    assert len(projects) == 138
    per_dir = Counter(p.parent.name for p in projects)
    assert per_dir == {
        "mob": 129,
        "hero": 3,
        "title": 2,
        "effect": 1,
        "effect_kor": 1,
        "hatching": 1,
        "loading": 1,
    }


def test_totals(projects):
    values = projects.values()
    assert sum(len(p.images) for p in values) == 860
    assert sum(len(p.actions) for p in values) == 331
    assert sum(1 for p in values for _ in p.frames()) == 1167
    assert sum(1 for p in values if not p.images and not p.actions) == 17


def test_every_frame_references_an_existing_png(projects):
    layers = 0
    for path, project in projects.items():
        res = path.parent / sprprj.RES_DIR
        assert sprprj.missing_images(project, res) == [], path
        for _, _, frame in project.frames():
            for layer in frame.layers:
                assert layer.image < len(project.images)  # also enforced by parse()
                png = res / project.images[layer.image].name
                assert png.read_bytes()[:8] == PNG_SIGNATURE, png
                layers += 1
    assert layers == 2816


def test_palettes_replace_the_png_plte_exactly(projects):
    count = 0
    for path, project in projects.items():
        for image in project.images:
            if image.palette is None:
                continue
            chunks = png_chunks((path.parent / sprprj.RES_DIR / image.name).read_bytes())
            assert len(image.palette) == len(chunks[b"PLTE"]), (path, image.name)
            count += 1
    assert count == 163


def test_field_ranges(projects):
    delays = Counter(f.delay for p in projects.values() for _, _, f in p.frames())
    assert set(delays) == {1, 2, 3, 5, 10, 20}
    layers = [la for p in projects.values() for _, _, f in p.frames() for la in f.layers]
    assert all(la.visible for la in layers)
    assert sum(la.hflip for la in layers) == 13
    assert all(p.actions for p in projects.values() if p.images)
    assert all(a.frames for p in projects.values() for a in p.actions)
