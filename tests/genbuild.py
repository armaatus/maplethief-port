"""Build the recompiler's output with CMake + Ninja through ``tests/recomp/gen_project/``,
which uses ``cmake/GeneratedCode.cmake`` like the top-level ``maplethief_gen`` (ADR 0021).

:func:`gen_build` configures a build tree (skipping the test when CMake or Ninja is missing)
with the host C compiler of :mod:`recomp_harness` (MSVC ``cl`` on the Windows runner).
:meth:`GenBuild.pending` asks Ninja, without building, which C files it would compile, so a
test can count how many translation units an incremental build recompiles.
"""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

import pytest

from recomp_harness import host_cc
from repo_paths import REPO

GEN_PROJECT = REPO / "tests" / "recomp" / "gen_project"
_COMPILE = re.compile(r"Building C object (\S+)")


@dataclass(frozen=True)
class GenBuild:
    cmake: str
    tree: Path

    def _run(self, *args: str, timeout: int = 900) -> str:
        done = subprocess.run(
            [self.cmake, "--build", str(self.tree), *args],
            capture_output=True,
            text=True,
            timeout=timeout,
        )
        if done.returncode != 0:
            pytest.fail(f"cmake --build failed:\n{done.stdout[-6000:]}\n{done.stderr[-3000:]}")
        return done.stdout

    def build(self) -> str:
        """Build everything (Ninja uses every core) and return the log."""
        return self._run()

    def pending(self) -> list[str]:
        """The source file names (``fn_001.c``) the next build would compile (ninja -n)."""
        names = []
        for obj in _COMPILE.findall(self._run("--", "-n")):
            name = Path(obj).name
            names.append(name.rsplit(".", 1)[0] if name.endswith((".o", ".obj")) else name)
        return names

    @property
    def exe(self) -> Path:
        return self.tree / ("gen_program.exe" if sys.platform == "win32" else "gen_program")


def gen_build(
    tree: Path, gen: Path, main: Path | None = None, build_type: str = "Debug"
) -> GenBuild:
    """Configure ``tree`` for the generated code in ``gen`` (and the driver ``main``)."""
    cmake, ninja = shutil.which("cmake"), shutil.which("ninja")
    if cmake is None or ninja is None:
        pytest.skip("cmake and ninja are needed for the gen.cmake build")
    cmd = [cmake, "-S", str(GEN_PROJECT), "-B", str(tree), "-G", "Ninja"]
    cmd += [f"-DCMAKE_MAKE_PROGRAM={Path(ninja).as_posix()}", f"-DCMAKE_BUILD_TYPE={build_type}"]
    cc = host_cc()
    cmd += [f"-DCMAKE_C_COMPILER={Path(cc.path).as_posix()}"]
    if cc.msvc and build_type == "Debug":
        # No incremental link: it dates the members of maplethief_gen.lib to the second, so a
        # unit rebuilt within the same second as the last link keeps its old code in the exe.
        cmd.append("-DCMAKE_EXE_LINKER_FLAGS_DEBUG=/debug /INCREMENTAL:NO")
    cmd.append(f"-DMAPLETHIEF_GEN_DIR={gen.as_posix()}")
    if main is not None:
        cmd.append(f"-DGEN_MAIN={main.as_posix()}")
    done = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    if done.returncode != 0:
        pytest.fail(f"cmake configure failed:\n{done.stdout[-4000:]}\n{done.stderr[-4000:]}")
    return GenBuild(cmake, tree)
