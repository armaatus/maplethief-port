"""Paths of this checkout, shared by the Python tests (import with ``from repo_paths import``),
and the check that a committed generated doc is what its generator writes today."""

from __future__ import annotations

from pathlib import Path

from mtport import bootstrap as bs

REPO = Path(__file__).resolve().parents[1]
# The extracted IPA, created by scripts/bootstrap; real_assets tests skip without it.
IPA_DIR = REPO / bs.IPA_DIR


def assert_committed_doc(rel: str, generated: str, regenerate: str) -> None:
    """The committed file ``rel`` (repo-relative) equals ``generated``, the generator's output
    for the current inputs; otherwise it is stale and ``regenerate`` rewrites it."""
    committed = (REPO / rel).read_text(encoding="utf-8")
    assert committed == generated, f"{rel} is stale: run {regenerate}"
