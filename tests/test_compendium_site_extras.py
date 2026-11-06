"""The site's extra files and head tags (issue #226): robots.txt, the sitemap, the 404 page,
the description and Open Graph tags, and the footer check, on the synthetic knowledge base."""

from __future__ import annotations

import re
from pathlib import Path

import pytest

from mtport.compendium import site
from tests.fixtures.compendium import site_kb as fx


@pytest.fixture
def built(tmp_path: Path) -> site.Site:
    kb = fx.write_kb(tmp_path)
    pages = fx.write_prose(tmp_path)
    manifest = fx.write_images(tmp_path / "out")
    return site.build_site(kb, pages, manifest, plugins=[])


def text(s: site.Site, path: str) -> str:
    return s.files[path].decode("utf-8")


def test_robots_and_sitemap(built):
    robots = text(built, "robots.txt")
    assert robots == (
        "User-agent: *\nAllow: /\nSitemap: https://armaatus.github.io/maplethief-port/sitemap.xml\n"
    )
    sitemap = text(built, "sitemap.xml")
    locs = re.findall(r"<loc>([^<]+)</loc>", sitemap)
    want = [site.BASE_URL + p for p in sorted(built.pages) if p != "404.html"]
    assert locs == want
    assert "monster/1.html" in sitemap and "404.html" not in sitemap
    assert "lastmod" not in sitemap  # undated, so two builds are equal


def test_404_page(built):
    page = text(built, "404.html")
    head = page[: page.index("</head>")]
    assert '<base href="https://armaatus.github.io/maplethief-port/">' in head
    assert head.index("<base") < head.index("<link")  # before the first relative URL
    assert '<meta name="robots" content="noindex">' in head
    assert "Page not found" in page and 'href="search.html"' in page
    assert site.DISCLAIMER in page
    assert "404.html" not in {e["u"] for e in built.index}  # not in the search
    # only the 404 page has a <base>: every other page is relative from where it sits
    assert all("<base" not in text(built, p) for p in built.pages if p != "404.html")


def test_description_and_open_graph_on_every_page(built):
    for path in built.pages:
        head = text(built, path).split("</head>")[0]
        title = re.search(r"<title[^>]*>([^<]*)</title>", head).group(1)
        desc = re.search(r'<meta name="description" content="([^"]*)"', head).group(1)
        assert f'<meta property="og:title" content="{title}"' in head, path
        assert f'<meta property="og:description" content="{desc}"' in head, path
        # title and description only: no images or other Open Graph data (ADR 0121)
        assert set(re.findall(r'property="(og:[a-z_:]+)"', head)) == {"og:title", "og:description"}
    mon = text(built, "monster/1.html")
    assert re.search(r'<meta property="og:title" content="[^"]*" data-verbatim>', mon)


def test_check_footer(built):
    assert site.check_footer(built.files) == []
    files = dict(built.files)
    files["monster/1.html"] = files["monster/1.html"].replace(
        site.DISCLAIMER.encode(), b"something else"
    )
    assert site.check_footer(files) == ["monster/1.html: no disclaimer footer"]


def test_links_still_resolve_with_the_extras(built, tmp_path):
    assert site.check_links(built.files, tmp_path / "out" / "img") == []
