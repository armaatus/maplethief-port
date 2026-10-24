"""Real-asset checks for the splitter. Skipped when work/ipa/ is absent (CI).

Run scripts/bootstrap (or scripts/verify-original.sh) first to create work/ipa/.
"""

import hashlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from mtport import bootstrap as bs
from mtport import fat

REPO = Path(__file__).resolve().parents[1]
BINARY = REPO / bs.BINARY

# From docs/ANALYSIS.md -> Binary.
EXPECTED_LAYOUT = {"armv6": (4096, 1543296), "armv7": (1548288, 1197296)}


@unittest.skipUnless(BINARY.is_file(), "real assets: work/ipa/ absent (run scripts/bootstrap)")
class RealBinaryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = BINARY.read_bytes()
        cls.sums = bs.read_sums(REPO / bs.SUMS)
        cls.parts = fat.split(cls.data)

    def test_layout_matches_analysis(self):
        layout = {name: (entry.offset, entry.size) for name, (entry, _) in self.parts.items()}
        self.assertEqual(layout, EXPECTED_LAYOUT)

    def test_slice_hashes_match_sha256sums(self):
        for arch in bs.ARCHS:
            blob = self.parts[arch][1]
            self.assertEqual(hashlib.sha256(blob).hexdigest(), self.sums[bs.slice_path(arch)], arch)

    @unittest.skipUnless(sys.platform == "darwin" and shutil.which("lipo"), "needs lipo (macOS)")
    def test_slices_match_lipo_thin(self):
        with tempfile.TemporaryDirectory() as tmp:
            for arch in bs.ARCHS:
                out = Path(tmp) / arch
                subprocess.run(
                    ["lipo", str(BINARY), "-thin", arch, "-output", str(out)], check=True
                )
                self.assertEqual(out.read_bytes(), self.parts[arch][1], arch)


if __name__ == "__main__":
    unittest.main()
