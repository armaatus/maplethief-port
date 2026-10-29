"""Real-asset check of the CoreFoundation subset (#48, ADR 0032) against the armv7 slice.

Skipped when work/ipa/ is absent (CI). Acceptance of #48: every CoreFoundation import of the
game that is not an Objective-C class (Foundation's NSString, NSData, ... are imported from
CoreFoundation too, and are host classes) has an entry in the runtime's HLE registry, as a
function or as a data-object init hook. The one exception is the constant-string isa, which
the ObjC start-up binds as the class object of ``__NSCFConstantString``. The behaviour of
each entry is tested by tests/cpp/cf_test.cpp (CI) and tests/cpp/cf_real_test.cpp (local).
"""

from __future__ import annotations

import re

import pytest

from mtport import bootstrap as bs
from recomp import loader
from recomp.imports import ImportTable
from repo_paths import REPO

pytestmark = pytest.mark.real_assets

SLICE = REPO / bs.slice_path("armv7")
REGISTRY = REPO / "runtime" / "src" / "hle" / "registry.cpp"
OBJC = REPO / "runtime" / "src" / "objc" / "objc.cpp"
CF = "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation"
CLASS_PREFIXES = ("_OBJC_CLASS_$_", "_OBJC_METACLASS_$_")
CLASS_ISA = "___CFConstantStringClassReference"


def registry_entries() -> dict[str, tuple[str, str]]:
    """name -> (function, init hook) of every registry line."""
    text = REGISTRY.read_text(encoding="utf-8")
    return {
        m[1]: (m[2], m[3]) for m in re.finditer(r'^\s*\{"(\w+)",\s*(\w+),\s*(\w+)\}', text, re.M)
    }


def test_every_corefoundation_import_has_an_hle_entry():
    table = ImportTable.from_image(loader.Image.load(SLICE))
    cf = [e for e in table.entries if e.library == CF and not e.name.startswith(CLASS_PREFIXES)]
    functions = {e.name for e in cf if e.function}
    data = {e.name for e in cf if not e.function} - {CLASS_ISA}
    assert (len(functions), len(data)) == (24, 12)
    entries = registry_entries()
    missing = sorted(n for n in functions if entries.get(n, ("nullptr",))[0] == "nullptr")
    assert not missing, missing
    no_hook = sorted(n for n in data if entries.get(n, ("", "nullptr"))[1] == "nullptr")
    assert not no_hook, no_hook
    # The constant-string isa is the class object of __NSCFConstantString (objc.cpp).
    assert CLASS_ISA in {e.name for e in cf}
    assert f'{{"{CLASS_ISA}", "__NSCFConstantString"}}' in OBJC.read_text(encoding="utf-8")
