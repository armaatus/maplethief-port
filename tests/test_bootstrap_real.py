"""Real-asset checks for the splitter. Skipped when work/ipa/ is absent (CI).

Run scripts/bootstrap (or scripts/verify-original.sh) first to create work/ipa/.
"""

import hashlib
import shutil
import subprocess
import sys

import pytest

from mtport import bootstrap as bs
from mtport import fat
from repo_paths import REPO

pytestmark = pytest.mark.real_assets

BINARY = REPO / bs.BINARY

# From docs/ANALYSIS.md -> Binary.
EXPECTED_LAYOUT = {"armv6": (4096, 1543296), "armv7": (1548288, 1197296)}


@pytest.fixture(scope="module")
def parts():
    return fat.split(BINARY.read_bytes())


def test_layout_matches_analysis(parts):
    layout = {name: (entry.offset, entry.size) for name, (entry, _) in parts.items()}
    assert layout == EXPECTED_LAYOUT


def test_slice_hashes_match_sha256sums(parts):
    sums = bs.read_sums(REPO / bs.SUMS)
    for arch in bs.ARCHS:
        assert hashlib.sha256(parts[arch][1]).hexdigest() == sums[bs.slice_path(arch)], arch


@pytest.mark.skipif(
    sys.platform != "darwin" or shutil.which("lipo") is None, reason="needs lipo (macOS)"
)
def test_slices_match_lipo_thin(parts, tmp_path):
    for arch in bs.ARCHS:
        out = tmp_path / arch
        subprocess.run(["lipo", str(BINARY), "-thin", arch, "-output", str(out)], check=True)
        assert out.read_bytes() == parts[arch][1], arch
