"""Shared pytest setup (ADR 0005).

tools/ is put on sys.path by `pythonpath` in pyproject.toml, and the ``real_assets`` marker
is registered there too. This file makes that marker skip its tests, with a reason in the
report, when the extracted IPA (work/ipa/, from scripts/bootstrap) is absent, as in CI.
"""

from __future__ import annotations

from pathlib import Path

import pytest

# Before the import: repo_paths.assert_committed_doc should show pytest's diff on failure.
pytest.register_assert_rewrite("repo_paths")

from repo_paths import IPA_DIR  # noqa: E402

SKIP_REASON = "real assets: work/ipa/ absent (run scripts/bootstrap)"


def pytest_collection_modifyitems(config: pytest.Config, items: list[pytest.Item]) -> None:
    if IPA_DIR.is_dir():
        return
    skip = pytest.mark.skip(reason=SKIP_REASON)
    for item in items:
        if item.get_closest_marker("real_assets") is not None:
            item.add_marker(skip)


@pytest.fixture(scope="session")
def ipa_dir() -> Path:
    """work/ipa/. Only use it from tests marked ``real_assets``."""
    return IPA_DIR


@pytest.fixture(scope="session")
def real_kb(ipa_dir: Path):
    """The compendium knowledge base built from work/ipa/ (once per session: the quests
    stage walks every script). Only use it from tests marked ``real_assets``."""
    from mtport.compendium import kb

    return kb.build(ipa_dir / "Payload" / "MapleThief.app")
