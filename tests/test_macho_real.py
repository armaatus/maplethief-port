"""Real-asset checks for the Mach-O reader against docs/ANALYSIS.md -> Binary.

Skipped when work/ipa/ is absent (CI); run scripts/bootstrap first.
"""

import pytest

from mtport import bootstrap as bs
from mtport import macho
from repo_paths import REPO

pytestmark = pytest.mark.real_assets


@pytest.fixture(scope="module", params=bs.ARCHS)
def image(request):
    return request.param, macho.load(REPO / bs.slice_path(request.param))


def test_function_starts_and_entry(image):
    arch, m = image
    starts = m.function_starts
    assert len(starts) == 2655
    thumb = sum(a & 1 for a in starts)
    if arch == "armv7":
        assert thumb == 2654
        assert starts[0] == m.entry_point == 0x27E0  # the one ARM function: crt1 start
        assert starts[-1] == 0xD98D1
    else:
        assert thumb == 0
        assert m.entry_point == 0x2AC0
    text = m.section("__TEXT", "__text")
    assert all(text.contains(a & ~1) for a in starts)


def test_armv7_layout():
    m = macho.load(REPO / bs.slice_path("armv7"))
    text = m.section("__TEXT", "__text")
    assert (text.addr, text.size) == (0x27E0, 0xD713C)
    assert m.section("__TEXT", "__symbolstub1").size == 217 * 4
    assert m.section("__DATA", "__nl_symbol_ptr").size == 82 * 4
    assert m.section("__DATA", "__mod_init_func").size == 2 * 4
    assert m.section("__DATA", "__objc_classlist").size == 75 * 4
    assert len(m.dylibs) == 17
    assert sum(weak for _, weak in m.dylibs) == 2
