"""The compendium's screenshot and sideways-scroll check (#230) on the synthetic site.

The headless Chrome parts skip when no Chrome or Chromium is installed."""

from __future__ import annotations

from pathlib import Path

import pytest

from mtport.compendium import shots, site
from tests.fixtures.compendium import site_kb as fx

CHROME = shots.find_chrome()
needs_chrome = pytest.mark.skipif(CHROME is None, reason="no Chrome or Chromium installed")


@pytest.fixture
def built_site(tmp_path: Path) -> Path:
    kb = fx.write_kb(tmp_path)
    pages = fx.write_prose(tmp_path)
    out = tmp_path / "site"
    fx.write_images(out)
    assert site.main(["build", "--kb", str(kb), "--pages", str(pages), "--out", str(out)]) == 0
    return out


def test_default_pages_cover_every_kind(built_site):
    pages = shots.default_pages(built_site)
    assert pages[:2] == ["index.html", "search.html"]
    assert "monsters/index.html" in pages and "world/index.html" in pages
    # one page of each entity kind, the preferred one when it exists
    assert "monster/1.html" in pages and "monster/2.html" not in pages
    assert "item/etc-3.html" in pages and "map/1-1.html" in pages
    assert all((built_site / p).is_file() for p in pages)


def test_shot_names_and_groups():
    assert shots.shot_groups([320, 390, 1280]) == [(320, 390), (1280,)]
    assert shots.shot_groups([1280]) == [(1280,)]
    assert shots.shot_name("monster/1.html", (320, 390), "dark") == "monster_1_320-390_dark.png"


@needs_chrome
def test_no_page_scrolls_sideways_at_320(built_site):
    pages = ["index.html", "monster/1.html", "monsters/index.html", "npcs/index.html"]
    got = shots.overflow(CHROME, built_site, pages, [320])
    assert len(got) == len(pages)
    assert [r for r in got if r["overflow"]] == []


@needs_chrome
def test_a_planted_wide_element_is_caught(built_site):
    (built_site / "wide.html").write_text(
        '<!doctype html><meta name="viewport" content="width=device-width">'
        '<body><div id="too-wide" style="width:900px">x</div></body>',
        encoding="utf-8",
    )
    got = shots.overflow(CHROME, built_site, ["wide.html"], [320])
    assert got[0]["overflow"] and got[0]["scroll"] > got[0]["client"]
    assert any(o.startswith("div#too-wide") for o in got[0]["offenders"])


@needs_chrome
def test_screenshots_are_written(built_site, tmp_path):
    files = shots.screenshots(
        CHROME, built_site, ["index.html"], tmp_path / "png", [320, 1280], ["light"]
    )
    assert sorted(f.name for f in files) == ["index_1280_light.png", "index_320_light.png"]
    for f in files:
        assert f.read_bytes()[:8] == b"\x89PNG\r\n\x1a\n"
