"""Paths of this checkout, shared by the Python tests (import with ``from repo_paths import``)."""

from __future__ import annotations

from pathlib import Path

from mtport import bootstrap as bs

REPO = Path(__file__).resolve().parents[1]
# The extracted IPA, created by scripts/bootstrap; real_assets tests skip without it.
IPA_DIR = REPO / bs.IPA_DIR
