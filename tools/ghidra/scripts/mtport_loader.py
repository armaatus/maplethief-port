"""Make ``import mtport`` work inside Ghidra without putting ``tools/`` on ``sys.path``.

``tools/`` holds this ``ghidra/`` directory, and on ``sys.path`` Python would take it as a
namespace package named ``ghidra`` that hides Ghidra's own Java ``ghidra`` package from
PyGhidra (``from ghidra.app... import`` then fails). So the driver and every script in this
directory load the ``mtport`` package from its directory instead: ``import mtport_loader``
first (this directory is on ``sys.path`` while PyGhidra runs a script and for the driver).
"""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[2]
REPO = TOOLS.parent


def load() -> None:
    if "mtport" in sys.modules:
        return
    package = TOOLS / "mtport"
    spec = importlib.util.spec_from_file_location(
        "mtport", package / "__init__.py", submodule_search_locations=[str(package)]
    )
    module = importlib.util.module_from_spec(spec)
    sys.modules["mtport"] = module
    spec.loader.exec_module(module)


load()
