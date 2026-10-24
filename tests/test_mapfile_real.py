"""Real-asset check: every shipped .map file decodes cleanly (docs/formats/map.md).

Skipped when work/ipa/ is absent (CI); run scripts/bootstrap first.
"""

import pytest

from mtport import mapfile as mf

pytestmark = pytest.mark.real_assets


@pytest.fixture(scope="module")
def map_files(ipa_dir):
    return sorted((ipa_dir / "Payload" / "MapleThief.app" / "map").glob("*.map"))


def test_all_90_maps_decode_exactly(map_files):
    assert len(map_files) == 90
    layer_counts = {}
    for path in map_files:
        data = path.read_bytes()
        # decode() is strict: no truncated token, no run past a layer, no trailing byte.
        m = mf.decode(data, path.name)
        assert all(len(layer) == m.width * m.height for layer in m.layers), path.name
        assert 20 <= m.width <= 60 and 20 <= m.height <= 52, path.name
        layer_counts[len(m.layers)] = layer_counts.get(len(m.layers), 0) + 1
        # Every byte is accounted for: re-encoding gives the original file back.
        assert mf.encode(m) == data, path.name
    assert layer_counts == {2: 11, 3: 48, 4: 31}


def test_first_map_matches_the_spec(map_files):
    m = mf.load(next(p for p in map_files if p.name == "00.map"))
    assert (m.width, m.height, len(m.layers)) == (40, 28, 3)
    # 00.map starts 28 00 1C 00 03 72 FF F5 8D 8E 8F 8E 90: 114 empty cells, then 5 tiles.
    assert m.layers[0][:114] == (mf.EMPTY,) * 114
    assert m.layers[0][114:119] == (0x8D, 0x8E, 0x8F, 0x8E, 0x90)
    assert m.max_tile() == 159
