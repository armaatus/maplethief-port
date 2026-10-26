"""The instruction census and the whole-binary lift (#33) on the real armv7 slice (local
only: needs the IPA).

* ``tests/recomp/integer_forms.txt`` is exactly the census's list of integer forms, so the CI
  test that every listed form has a snippet covers the binary.
* No integer instruction lifts to an unsupported-instruction trap; the only traps left are
  NEON and the float<->integer ``vcvt`` (#34).
* The C of every function (all 2,655, the ARM entry stub included) compiles with clang
  ``-std=c17 -Wall -Wextra -Werror -Wpedantic`` (an object file, not a syntax check), and
  passes the syntax check for the MSVC target with both macro paths of the support headers.
"""

from __future__ import annotations

import json
import shutil
import subprocess

import pytest

from recomp import census, prototype
from recomp.__main__ import main
from recomp.discover import discover
from recomp.imports import ImportTable
from recomp.loader import Image
from recomp.output import write_program
from recomp_harness import INCLUDE, MSVC_SHIM
from repo_paths import REPO

pytestmark = pytest.mark.real_assets

SLICE = REPO / "work" / "slices" / "MapleThief.armv7"
FORM_LIST = REPO / "tests" / "recomp" / "integer_forms.txt"
VFP_TRAPS = {"vcvt.s32.f32 s,s", "vcvt.f32.s32 s,s", "vcvt.f64.s32 d,s", "vcvt.s32.f64 s,d",
             "vcvt.u32.f64 s,d", "neon <undecoded>"}  # fmt: skip


@pytest.fixture(scope="module")
def run(tmp_path_factory):
    out = tmp_path_factory.mktemp("census") / "census.json"
    return census.run(SLICE, out), out


def test_form_list_is_the_census(run) -> None:
    found, _ = run
    assert FORM_LIST.read_text(encoding="utf-8") == census.integer_form_list(found), (
        "regenerate: python -m recomp census --forms tests/recomp/integer_forms.txt"
    )


def test_no_integer_instruction_is_unsupported(run) -> None:
    found, out = run
    c = found.counts()
    assert c["functions"] == 2655 and c["instructions"] > 327000
    assert c["unsupported_integer_instructions"] == 0
    trapped = {f.form for f in found.forms.values() if f.unsupported}
    assert trapped <= VFP_TRAPS  # #34
    assert all(found.forms[f].unsupported == found.forms[f].count for f in trapped)
    doc = json.loads(out.read_text(encoding="utf-8"))
    assert doc["schema"] == 1 and doc["counts"] == c and len(doc["forms"]) == c["forms"]


def test_census_command(tmp_path) -> None:
    out, forms = tmp_path / "census.json", tmp_path / "forms.txt"
    assert main(["census", "--slice", str(SLICE), "--out", str(out), "--forms", str(forms)]) == 0
    assert forms.read_text(encoding="utf-8") == FORM_LIST.read_text(encoding="utf-8")


@pytest.fixture(scope="module")
def program(tmp_path_factory):
    image = Image.load(SLICE)
    found = discover(image)
    lifted = prototype.lift(image, list(image.function_starts), discovery=found)
    gen = tmp_path_factory.mktemp("gen")
    return lifted, write_program(gen, lifted, ImportTable.from_image(image)), gen


def _clang() -> str:
    path = shutil.which("clang")
    if path is None:
        pytest.skip("clang not found")
    return path


def test_every_function_is_lifted(program) -> None:
    lifted, _, _ = program
    assert len(lifted) == 2655
    stub = next(f for f in lifted if f.ptr == 0x27E0)  # the ARM entry stub
    assert stub.clean and "MT_CALL_IMPORT(" in stub.source  # b _exit
    assert sum(f.clean for f in lifted) >= 2525
    assert {why.split()[0] for f in lifted for _, why in f.unsupported} <= {
        "undefined", "vcvt.s32.f32", "vcvt.f32.s32", "vcvt.f64.s32", "vcvt.s32.f64",
        "vcvt.u32.f64",
    }  # fmt: skip


def test_whole_program_compiles_with_clang(program, tmp_path) -> None:
    _, sources, gen = program
    clang = _clang()
    for src in sources:
        cmd = [clang, "-std=c17", "-Wall", "-Wextra", "-Werror", "-Wpedantic", "-O0"]
        cmd += ["-ffp-contract=off", f"-I{INCLUDE}", f"-I{gen}", "-c", str(src)]
        cmd += ["-o", str(tmp_path / f"{src.stem}.o")]
        done = subprocess.run(cmd, capture_output=True, text=True)
        assert done.returncode == 0 and not done.stderr, done.stderr[:4000]


@pytest.mark.parametrize("msc_path", [False, True], ids=["clang-macros", "msc-macros"])
def test_whole_program_compiles_for_the_msvc_target(program, msc_path: bool) -> None:
    _, sources, gen = program
    cmd = [_clang(), "--target=x86_64-pc-windows-msvc", "-fms-compatibility", "-fms-extensions"]
    cmd += ["-std=c17", "-Wall", "-Wextra", "-Werror", f"-isystem{MSVC_SHIM}"]
    cmd += [f"-I{INCLUDE}", f"-I{gen}", "-fsyntax-only"]
    if msc_path:
        cmd += ["-U__clang__", "-Wno-unknown-pragmas"]
    done = subprocess.run([*cmd, *map(str, sources)], capture_output=True, text=True)
    assert done.returncode == 0, done.stderr[:4000]
