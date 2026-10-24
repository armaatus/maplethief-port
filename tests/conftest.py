"""Shared pytest setup (ADR 0005).

tools/ is put on sys.path by `pythonpath` in pyproject.toml, and the ``real_assets`` marker
is registered there too. This file makes that marker skip its tests, with a reason in the
report, when the extracted IPA (work/ipa/, from scripts/bootstrap) is absent, as in CI.
"""

from __future__ import annotations

from pathlib import Path

import pytest

from repo_paths import IPA_DIR

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
