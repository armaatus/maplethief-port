"""The world map (#232) on the real knowledge base: every area and map is on it, every link
in its data resolves, every area and map page links to it, and the spot positions match the
dots on the game's world-map art. Skips without ``work/ipa/``.
"""

from __future__ import annotations

import json
import posixpath
from pathlib import Path

import pytest

from mtport import cgbi
from mtport.compendium import kb as kbm
from mtport.compendium import site
from mtport.compendium.images import WORLD_ART
from mtport.compendium.sections import _worldmap as wm
from mtport.compendium.siteapi import PAGES_DIR
from tests.test_compendium_worldmap import ids_of, urls

pytestmark = pytest.mark.real_assets

MANIFEST = Path("build/compendium/site/img/manifest.json")


@pytest.fixture(scope="module")
def built(real_kb: kbm.KB, tmp_path_factory) -> site.Site:
    out = tmp_path_factory.mktemp("kb")
    kbm.write(real_kb, out)
    manifest = MANIFEST if MANIFEST.is_file() else None
    return site.build_site(out, PAGES_DIR, manifest)


def data(s: site.Site) -> dict:
    return json.loads(s.files[wm.JSON_PATH])


def test_every_area_and_map_is_on_the_map(built) -> None:
    d = data(built)
    assert len(d["areas"]) == 11
    assert len(d["maps"]) == 90
    assert d["others"] == []
    assert sorted(m for a in d["areas"] for m in a["maps"]) == sorted(m["id"] for m in d["maps"])
    page = built.files[wm.PAGE].decode()
    for a in d["areas"]:
        assert f'id="area={a["id"]}"' in page
    for m in d["maps"]:
        assert f'id="map={m["id"]}"' in page


def test_every_link_in_the_data_resolves(built) -> None:
    links = urls(data(built))
    assert len(links) > 300
    parsed = {p: ids_of(built.files[p].decode("utf-8")) for p in built.files if p.endswith(".html")}
    for u in links:
        path, _, frag = u.partition("#")
        target = posixpath.normpath(posixpath.join("world", path))
        if target.startswith("img/"):
            if MANIFEST.is_file():  # the images step has run
                assert (MANIFEST.parent / target[len("img/") :]).is_file(), u
            continue
        assert target in built.files, u
        if frag:
            assert frag in parsed[target], u
    assert site.check_links(built.files, MANIFEST.parent if MANIFEST.is_file() else None) == []


def test_every_area_and_map_page_links_to_the_map(built) -> None:
    d = data(built)
    for m in d["maps"]:
        page = built.files[f"map/{m['id']}.html"].decode()
        assert f'world/map.html#map={m["id"]}"' in page, m["id"]
    for a in d["areas"]:
        page = built.files[f"area/{a['id']}.html"].decode()
        assert f'world/map.html#area={a["id"]}"' in page, a["id"]


def test_spot_positions_match_the_art(ipa_dir: Path) -> None:
    art = ipa_dir / "Payload" / "MapleThief.app" / WORLD_ART
    img = cgbi.decode(art.read_bytes(), art.name)
    assert (img.width, img.height) == wm.ART_SIZE

    def blue(x: int, y: int) -> bool:
        i = (y * img.width + x) * 4
        r, g, b = img.rgba[i : i + 3]
        return b > 200 and r < 90 and 100 < g < 190

    for spot, (x, y) in wm.SPOT_XY.items():
        near = [(x + dx, y + dy) for dx in range(-3, 4) for dy in range(-3, 4)]
        assert any(blue(*p) for p in near), spot
