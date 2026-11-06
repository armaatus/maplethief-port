"""The C++ IPA import's expected values (#182, platform/src/ipa_import.h) are the bootstrap's:
the IPA's size is ``IPA_SIZE`` in tools/mtport/bootstrap.py, and the IPA's and the bundle
executable's SHA-256 are their lines in original/SHA256SUMS."""

from __future__ import annotations

import re
import unittest

from mtport import bootstrap as bs
from repo_paths import REPO

HEADER = REPO / "platform/src/ipa_import.h"


def _constant(text: str, name: str) -> str:
    m = re.search(rf"\b{name}\s*=\s*(\"[^\"]*\"|[0-9]+)\s*;", text)
    assert m, f"{name} not found in {HEADER.relative_to(REPO)}"
    return m.group(1).strip('"')


class IpaImportConstantsTest(unittest.TestCase):
    def setUp(self) -> None:
        self.header = HEADER.read_text(encoding="utf-8")
        self.sums = bs.load_sums(REPO)

    def test_size_is_bootstraps(self) -> None:
        self.assertEqual(int(_constant(self.header, "kIpaSize")), bs.IPA_SIZE)

    def test_ipa_sha256_is_sha256sums(self) -> None:
        self.assertEqual(_constant(self.header, "kIpaSha256"), self.sums[bs.IPA])

    def test_executable_sha256_is_sha256sums(self) -> None:
        self.assertEqual(_constant(self.header, "kExecutableSha256"), self.sums[bs.BINARY])

    def test_file_name_is_bootstraps(self) -> None:
        self.assertEqual(_constant(self.header, "kIpaFileName"), bs.IPA.rsplit("/", 1)[1])
