"""Real-asset check of the libc subset (#44, ADR 0028) against the armv7 slice.

Skipped when work/ipa/ is absent (CI). Acceptance of #44: every libc and compiler-rt import
of the game (libSystem.B.dylib and libgcc_s.1.dylib) has an entry in the runtime's HLE
registry, except the ones other issues own. The behaviour of each entry is tested by
tests/cpp/libc_test.cpp and tests/cpp/imports_test.cpp (CI).
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
LIBRARIES = ("/usr/lib/libSystem.B.dylib", "/usr/lib/libgcc_s.1.dylib")

# libSystem and libgcc imports that are not the C library, with the issue that owns them.
OWNED_ELSEWHERE = {
    "__Block_object_assign": "#47 blocks",
    "__Block_object_dispose": "#47 blocks",
    "__NSConcreteStackBlock": "#47 blocks",
    "__NSConcreteGlobalBlock": "#47 blocks",
    "___cxa_atexit": "#43 boot / #45 C++ ABI",
    "_CC_MD5": "CommonCrypto",
    "_CC_SHA1": "CommonCrypto",
    "dyld_stub_binder": "never called: lazy binding is resolved at load",
    "__Unwind_SjLj_Register": "#37 SjLj",
    "__Unwind_SjLj_Resume": "#37 SjLj",
    "__Unwind_SjLj_Unregister": "#37 SjLj",
}


def registry_names() -> set[str]:
    return set(re.findall(r'^\s*\{"(\w+)",', REGISTRY.read_text(encoding="utf-8"), re.M))


def test_every_libc_import_has_an_hle_entry():
    table = ImportTable.from_image(loader.Image.load(SLICE))
    libc = {
        e.name
        for e in table.entries
        if e.library in LIBRARIES
        and e.name not in OWNED_ELSEWHERE
        and not e.name.startswith("_dispatch_")  # GCD (#50)
        and e.name != "__dispatch_main_q"
    }
    # the libSystem C library (with _exit, which #43 implements) and the libgcc helpers
    assert len(libc) == 34 + 7
    missing = libc - registry_names()
    assert not missing, sorted(missing)
    # The exclusions name real imports, so the list cannot go stale silently.
    names = {e.name for e in table.entries}
    assert set(OWNED_ELSEWHERE) <= names
