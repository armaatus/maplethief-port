"""Real-asset checks for the data-table browser: build the pages from the shipped tables.

Skipped when work/ipa/ is absent (CI); run scripts/bootstrap first.
"""

import pytest

from mtport import datatables as dt
from mtport import pngdat
from mtport import tablebrowser as tb

pytestmark = pytest.mark.real_assets


@pytest.fixture(scope="module")
def app(ipa_dir):
    return ipa_dir / "Payload" / "MapleThief.app"


@pytest.fixture(scope="module")
def built(app, tmp_path_factory):
    out = tmp_path_factory.mktemp("tables")
    files, errors = tb.build(app, out)
    return out, files, errors


def test_all_8_tables_build_without_errors(app, built):
    out, files, errors = built
    assert errors == []
    assert tb.check(app, out) == []
    assert sorted(f for f in files if f.endswith(".html")) == sorted(
        ["index.html", *(f"{n}.html" for n in tb.PAGES)]
    )


def test_record_counts_match_the_parser(app, built):
    out, _, _ = built
    for name in dt.TABLES:
        counts = {
            len(dt.parse(name, (app / lang / "data" / f"{name}.dat").read_bytes()).records)
            for lang in tb.LANGS
        }
        assert len(counts) == 1, name
        n = counts.pop()
        assert tb.page_rows((out / f"{name}.html").read_bytes()) == (n, n), name
    # buildup.dat: one value per shown language.
    assert tb.page_rows((out / "buildup.html").read_bytes()) == (2, 2)
    index = (out / "index.html").read_text()
    assert "byte-identical to English in all 8 files" in index
    assert "No errors." in index


def test_every_icon_resolves_into_its_pack(app, built):
    _, files, _ = built
    for name, ip in tb.ICONS.items():
        images = pngdat.split((app / ip.pack).read_bytes(), ip.pack)
        table = dt.parse(name, (app / "English" / "data" / f"{name}.dat").read_bytes())
        used = {r.icon for r in table.records}
        assert max(used) < len(images), name
        for index in used:
            assert files[tb.icon_file(name, index)] == images[index].data
    icons = [f for f in files if f.startswith("icons/")]
    assert len(icons) == 73 + 105 + 21  # distinct icon values of item_equip, item_etc, skill


def test_output_is_deterministic(app, built):
    _, files, _ = built
    again, _ = tb.render(tb.load(app))
    assert again == files
