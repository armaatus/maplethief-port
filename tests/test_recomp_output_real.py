"""The output stage (#38, ADR 0021) on the real armv7 slice (local only: needs the IPA).

* The default partition puts every one of the 2,655 functions into exactly one of at most 64
  units, and no unit is bigger than the largest function plus one unit's share.
* Lifting again writes nothing; a change to one function's C rewrites only its unit.
* Built through cmake/GeneratedCode.cmake (Debug, Ninja), the real program compiles, and
  after that change an incremental build compiles exactly one translation unit.
* Every unit compiles to an x86-64 COFF object with clang in clang-cl mode
  (``--target=x86_64-pc-windows-msvc``, ``/W4 /WX /O2``, the flags GeneratedCode.cmake gives
  clang-cl), with shim CRT headers because the Mac has no Windows SDK.
"""

from __future__ import annotations

import dataclasses
import os
import re
import shutil
import subprocess
from concurrent.futures import ThreadPoolExecutor

import pytest

from genbuild import gen_build
from recomp import prototype
from recomp.discover import discover
from recomp.imports import ImportTable
from recomp.loader import Image
from recomp.output import DEFAULT_UNITS, partition, unit_name, write_program
from recomp_harness import INCLUDE, MSVC_SHIM
from repo_paths import REPO

pytestmark = pytest.mark.real_assets

SLICE = REPO / "work" / "slices" / "MapleThief.armv7"


@pytest.fixture(scope="module")
def program(tmp_path_factory):
    image = Image.load(SLICE)
    lifted = prototype.lift(image, list(image.function_starts), discovery=discover(image))
    span = (image.text.addr, image.text.end)
    gen = tmp_path_factory.mktemp("gen")
    imports = ImportTable.from_image(image)
    return lifted, imports, span, gen, write_program(gen, lifted, imports, DEFAULT_UNITS, span)


def _stamps(gen) -> dict[str, int]:
    out = {}
    for f in gen.iterdir():
        st = f.stat()
        os.utime(f, ns=(st.st_atime_ns - 10**11, st.st_mtime_ns - 10**11))
        out[f.name] = f.stat().st_mtime_ns
    return out


def _changed(gen, stamps) -> set[str]:
    return {f.name for f in gen.iterdir() if stamps.get(f.name) != f.stat().st_mtime_ns}


def test_units_cover_the_program_and_are_balanced(program) -> None:
    lifted, _, span, gen, sources = program
    units = [s for s in sources if s.name.startswith("fn_")]
    assert 48 <= len(units) <= DEFAULT_UNITS
    sizes = {s.name: len(s.read_bytes()) for s in units}
    text = "".join(s.read_text(encoding="utf-8") for s in units)
    defined = re.findall(r"^void (f_[0-9A-F]{8})\(mt_cpu \*MT_RESTRICT cpu\)$", text, re.M)
    assert len(lifted) == 2655 and sorted(defined) == sorted(f.name for f in lifted)
    biggest = max(len(f.source.encode()) for f in lifted)
    mean = sum(sizes.values()) / DEFAULT_UNITS
    assert max(sizes.values()) <= biggest + mean, (max(sizes.values()), biggest, mean)


def test_one_changed_function_rewrites_one_unit(program) -> None:
    lifted, imports, span, gen, sources = program
    stamps = _stamps(gen)
    assert write_program(gen, lifted, imports, DEFAULT_UNITS, span) == sources
    assert _changed(gen, stamps) == set()
    index = len(lifted) // 2
    changed = list(lifted)
    changed[index] = dataclasses.replace(
        lifted[index], source="/* changed */\n" + lifted[index].source
    )
    write_program(gen, changed, imports, DEFAULT_UNITS, span)
    cells = partition([f.ptr for f in lifted], DEFAULT_UNITS, span)
    (unit,) = [u for u, ptrs in cells.items() if lifted[index].ptr in ptrs]
    assert _changed(gen, stamps) == {unit_name(unit)}
    write_program(gen, lifted, imports, DEFAULT_UNITS, span)  # restore


def test_real_program_builds_and_rebuilds_one_unit(program, tmp_path) -> None:
    lifted, imports, span, gen, sources = program
    build = gen_build(tmp_path / "build", gen)
    assert sorted(build.pending()) == sorted(s.name for s in sources)
    build.build()
    assert build.pending() == []
    index = 1000
    changed = list(lifted)
    changed[index] = dataclasses.replace(
        lifted[index], source="/* changed */\n" + lifted[index].source
    )
    write_program(gen, changed, imports, DEFAULT_UNITS, span)
    cells = partition([f.ptr for f in lifted], DEFAULT_UNITS, span)
    (unit,) = [u for u, ptrs in cells.items() if lifted[index].ptr in ptrs]
    assert build.pending() == [unit_name(unit)]
    assert build.build().count("Building C object") == 1
    write_program(gen, lifted, imports, DEFAULT_UNITS, span)  # restore


def test_every_unit_compiles_in_clang_cl_mode(program, tmp_path) -> None:
    _, _, _, gen, sources = program
    clang = shutil.which("clang")
    if clang is None:
        pytest.skip("clang not found")
    cmd = [clang, "--driver-mode=cl", "--target=x86_64-pc-windows-msvc", "/std:c17", "/W4"]
    cmd += ["/WX", "/O2", "/bigobj", "/utf-8", "/clang:-ffp-contract=off", "/c"]
    cmd += ["-imsvc", str(MSVC_SHIM), f"/I{INCLUDE}", f"/I{gen}"]

    def compile_one(src):
        obj = tmp_path / f"{src.stem}.obj"
        done = subprocess.run([*cmd, str(src), f"/Fo{obj}"], capture_output=True, text=True)
        return src.name, done.returncode, done.stdout + done.stderr, obj

    with ThreadPoolExecutor(os.cpu_count()) as pool:
        results = list(pool.map(compile_one, sources))
    failed = [(name, out[:2000]) for name, status, out, _ in results if status or out.strip()]
    assert not failed, failed
    assert all(obj.read_bytes()[:2] == b"\x64\x86" for *_, obj in results)  # AMD64 COFF
