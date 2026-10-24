"""M1 exit criterion: every file in MapleThief.app has a known format and decodes.

Skipped when work/ipa/ is absent (CI); run scripts/bootstrap first.
"""

import pytest

from mtport import formats
from repo_paths import REPO

pytestmark = pytest.mark.real_assets


@pytest.fixture(scope="module")
def report():
    return formats.check(REPO / formats.bootstrap.APP_DIR)


def test_every_file_is_classified(report):
    assert report.unclassified == []


def test_every_file_decodes(report):
    assert report.failures == []


def test_counts_match_analysis(report):
    c = report.counts
    assert (c["map"], c["obj"], c["att"], c["mapprj"]) == (90, 90, 91, 11)
    assert (c["sprprj"], c["script-out"], c["png-dat"], c["data-table"]) == (138, 307, 40, 24)
    assert (c["png"] + c["cgbi-png"], c["cgbi-png"]) == (1322, 113)
    assert (c["mp3"], c["wav"], c["nib"]) == (18, 26, 1)


def test_every_spec_format_has_shipped_files(report):
    for fmt in formats.FORMATS.values():
        assert report.counts[fmt.name] > 0, fmt.name
