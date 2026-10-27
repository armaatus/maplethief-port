"""Real-asset check of the C++ ABI imports (#45, ADR 0031) against the armv7 slice.

Skipped when work/ipa/ is absent (CI). Acceptance of #45: every C++ ABI import of the game is
implemented: the libstdc++.6.dylib imports (operator new/delete and their [] forms,
__cxa_pure_virtual, std::terminate, the three __cxxabiv1 type_info vtables and the SjLj
personality), __cxa_atexit from libSystem and the SjLj unwinder from libgcc. Each has a
registry entry, except the personality, which the guest never calls: the unwinder binds its
address (mt_sjlj_bind_personalities, ADR 0020). The behaviour is tested by
tests/cpp/cxxabi_test.cpp and tests/recomp/sjlj/*.sjlj (CI).
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
SJLJ_C = REPO / "runtime" / "src" / "sjlj.c"
LIBSTDCXX = "/usr/lib/libstdc++.6.dylib"
# The C++ ABI imports of other libraries (ANALYSIS.md, "Imports": 14 C++ ABI imports).
ELSEWHERE = {
    "___cxa_atexit",
    "__Unwind_SjLj_Register",
    "__Unwind_SjLj_Resume",
    "__Unwind_SjLj_Unregister",
}
BOUND_BY_THE_UNWINDER = {"___gxx_personality_sj0"}


def registry_names() -> set[str]:
    return set(re.findall(r'^\s*\{"(\w+)",', REGISTRY.read_text(encoding="utf-8"), re.M))


def test_every_cxx_abi_import_is_implemented():
    table = ImportTable.from_image(loader.Image.load(SLICE))
    by_name = {e.name: e for e in table.entries}
    libstdcxx = {e.name for e in table.entries if e.library == LIBSTDCXX}
    assert len(libstdcxx) == 10
    assert ELSEWHERE <= set(by_name)
    cxx_abi = libstdcxx | ELSEWHERE
    assert len(cxx_abi) == 14
    missing = cxx_abi - registry_names() - BOUND_BY_THE_UNWINDER
    assert not missing, sorted(missing)
    # The personality is bound by name in the unwinder, not called.
    for name in BOUND_BY_THE_UNWINDER:
        assert f'"{name}"' in SJLJ_C.read_text(encoding="utf-8")


def test_the_type_info_vtables_are_data_imports_with_room_for_the_virtuals():
    """The init hooks write offset-to-top, the type_info slot and 9 virtual slots (44 bytes)
    into the 64-byte objects recomp/imports.py gives the vtable imports."""
    table = ImportTable.from_image(loader.Image.load(SLICE))
    vtables = [e for e in table.entries if e.name.startswith("__ZTVN10__cxxabiv1")]
    assert sorted(e.name for e in vtables) == [
        "__ZTVN10__cxxabiv117__class_type_infoE",
        "__ZTVN10__cxxabiv120__si_class_type_infoE",
        "__ZTVN10__cxxabiv121__vmi_class_type_infoE",
    ]
    for e in vtables:
        assert e.data
        assert e.size >= 8 + 4 * 9
