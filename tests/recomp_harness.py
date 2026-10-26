"""Compile and run recompiled C in the tests (ADR 0014; the snippet suite #39 builds on it).

:func:`host_cc` finds the host C compiler: ``$MAPLETHIEF_CC`` or ``$CC`` if set, MSVC ``cl``
on Windows when it is on PATH (CI runs the tests in a Visual Studio developer shell), else
``clang``, ``cc`` or ``gcc``. Generated code is built with the strict flags of ADR 0002
(``-std=c17 -Wall -Wextra -Werror`` / ``/std:c17 /W4 /WX``), plus FP contraction off.

:func:`run_snippet` lifts a byte string with :func:`recomp.snippet.lift_snippet`, links it
with ``tests/recomp/test_runtime.c`` and a generated ``main`` that sets the initial state,
runs it, and returns the final state as a dict (``r0``..``r15``, ``cpsr``, ``fpscr``,
``d0``..``d31``, plus ``mem`` reads requested by the test).
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

import pytest

from recomp.snippet import lift_snippet
from repo_paths import REPO

INCLUDE = REPO / "runtime" / "include"
RUNTIME_SRC = REPO / "runtime" / "src"
# The runtime's import table code (ADR 0019): table linking, the unimplemented-import handler,
# data objects and binds, and the HLE implementations it registers. C++; build them with
# RUNTIME_SRC on the include path too.
IMPORT_RUNTIME = [RUNTIME_SRC / "hle" / n for n in ("imports.cpp", "libgcc.cpp", "registry.cpp")]
IMPORT_RUNTIME.append(RUNTIME_SRC / "sjlj.c")  # the SjLj handlers registry.cpp lists (#37)
SUPPORT = REPO / "tests" / "recomp"
TEST_RUNTIME = SUPPORT / "test_runtime.c"
MSVC_SHIM = SUPPORT / "msvc_shim"
WINDOW = 0x200000  # guest memory window of the snippet tests: 2 MiB
SNIPPET_BASE = 0x1000  # where snippets are mapped and lifted
DATA_BASE = 0x100000  # free memory for tests to use


@dataclass(frozen=True)
class CCompiler:
    path: str
    msvc: bool
    clang: bool

    def build(self, sources: list[Path], exe: Path, include: list[Path]) -> None:
        """Compile and link ``sources`` into ``exe``; fails the test on any diagnostic.
        ``.cpp`` sources (the runtime's, e.g. :data:`IMPORT_RUNTIME`) are compiled as C++20
        without exceptions or RTTI, so the C program links without a C++ runtime library."""
        objects = [self._cxx_object(s, exe.parent, include) for s in sources if s.suffix == ".cpp"]
        c_sources = [s for s in sources if s.suffix != ".cpp"]
        if self.msvc:
            cmd = [self.path, "/nologo", "/std:c17", "/W4", "/WX", "/O1", "/fp:precise", "/utf-8"]
            cmd += ["/D_CRT_SECURE_NO_WARNINGS", *(f"/I{p}" for p in include)]
            cmd += [*map(str, c_sources + objects), f"/Fe{exe}", f"/Fo{exe.parent}{os.sep}"]
        else:
            cmd = [self.path, "-std=c17", "-Wall", "-Wextra", "-Werror", "-O1"]
            cmd += ["-ffp-contract=off", *(f"-I{p}" for p in include)]
            cmd += [*map(str, c_sources + objects), "-o", str(exe)]
            if sys.platform.startswith("linux"):
                cmd.append("-lm")
        self._run(cmd, exe.parent)

    def _cxx_object(self, source: Path, out_dir: Path, include: list[Path]) -> Path:
        obj = out_dir / f"{source.stem}_cxx{'.obj' if self.msvc else '.o'}"
        if self.msvc:
            cmd = [self.path, "/nologo", "/c", "/std:c++20", "/W4", "/WX", "/O1", "/EHsc"]
            cmd += ["/utf-8", "/permissive-", *(f"/I{p}" for p in include)]
            cmd += [str(source), f"/Fo{obj}"]
        else:
            cmd = [self.path, "-x", "c++", "-std=c++20", "-fno-exceptions", "-fno-rtti"]
            cmd += ["-fno-threadsafe-statics", "-Wall", "-Wextra", "-Werror", "-O1"]
            cmd += [*(f"-I{p}" for p in include), "-c", str(source), "-o", str(obj)]
        self._run(cmd, out_dir)
        return obj

    def _run(self, cmd: list[str], cwd: Path) -> None:
        done = subprocess.run(cmd, capture_output=True, text=True, cwd=cwd)
        if done.returncode != 0 or (self.msvc and "warning" in done.stdout):
            pytest.fail(f"C build failed: {' '.join(cmd)}\n{done.stdout}\n{done.stderr}")


def host_cc() -> CCompiler:
    """The host C compiler, or skip the test when there is none."""
    for env in ("MAPLETHIEF_CC", "CC"):
        if os.environ.get(env):
            path = os.environ[env]
            name = Path(path).name.lower()
            return CCompiler(path, name.startswith("cl"), "clang" in name)
    if sys.platform == "win32" and shutil.which("cl"):
        return CCompiler(shutil.which("cl"), True, False)
    for name in ("clang", "cc", "gcc"):
        path = shutil.which(name)
        if path:
            return CCompiler(path, False, name == "clang")
    pytest.skip("no C compiler found (set MAPLETHIEF_CC)")


def run_exe(exe: Path) -> tuple[int, str]:
    done = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
    return done.returncode, done.stdout


def parse_state(text: str) -> dict[str, int]:
    state: dict[str, int] = {}
    for line in text.splitlines():
        key, sep, value = line.partition("=")
        if sep and value.startswith("0x"):
            state[key] = int(value, 16)
    return state


def _assign(key: str, value: int) -> str:
    if key.startswith("r"):
        return f"cpu.r[{int(key[1:])}] = 0x{value:08X}u;"
    if key.startswith("d"):
        return f"cpu.d[{int(key[1:])}] = 0x{value:016X}ull;"
    if key.startswith("s"):
        return f"mt_s_set(&cpu, {int(key[1:])}u, 0x{value:08X}u);"
    if key in ("cpsr", "fpscr"):
        return f"cpu.{key} = 0x{value:08X}u;"
    raise ValueError(f"unknown state key {key}")


def run_cases(
    tmp_path: Path, code: bytes, cases: list[dict[str, int]], arm: bool = False
) -> list[dict[str, int]]:
    """Lift ``code`` once and run it from each start state of ``cases`` (``r0``..``r12``
    and ``cpsr``; anything else starts at 0, ``sp`` and ``lr`` as in :func:`run_snippet`).
    Returns ``r0``..``r12`` and ``cpsr`` after each run: one build for many vectors, so the
    flag-model tests can compare hundreds of operand pairs with the ARM ARM pseudocode."""
    snippet = lift_snippet(code, SNIPPET_BASE, arm=arm)
    assert not snippet.unsupported and not snippet.problems, snippet
    (tmp_path / "snippet.c").write_text(snippet.source, encoding="utf-8")
    keys = [f"r{n}" for n in range(13)] + ["cpsr"]
    rows = [", ".join(f"0x{case.get(k, 0) & 0xFFFFFFFF:08X}u" for k in keys) for case in cases]
    lines = [
        '#include "test_runtime.h"',
        "#include <stdio.h>",
        "",
        "void snippet(mt_cpu *MT_RESTRICT cpu);",
        "mt_import_fn mt_import_table[1];",
        'const char *const mt_import_names[1] = {""};',
        "const mt_fn_entry mt_fn_table[1] = {{0u, 0}};",
        "const uint32_t mt_fn_count = 0u;",
        f"static const uint32_t cases[{len(cases)}][14] = {{",
        *(f"    {{{row}}}," for row in rows),
        "};",
        "",
        "int main(void) {",
        "    static mt_cpu cpu;",
        "    unsigned i, r;",
        f"    mt_test_init(&cpu, 0x{WINDOW:X}u, 0u);",
        f"    for (i = 0; i < {len(cases)}u; i++) {{",
        "        for (r = 0; r < 13u; r++) {",
        "            cpu.r[r] = cases[i][r];",
        "        }",
        "        cpu.cpsr = cases[i][13];",
        f"        cpu.r[13] = 0x{WINDOW - 64:X}u;",
        "        cpu.r[14] = MT_TEST_RETURN;",
        "        snippet(&cpu);",
        "        for (r = 0; r < 13u; r++) {",
        '            printf("%u r%u=0x%08X\\n", i, r, cpu.r[r]);',
        "        }",
        '        printf("%u cpsr=0x%08X\\n", i, cpu.cpsr);',
        "    }",
        "    return 0;",
        "}",
        "",
    ]
    (tmp_path / "main.c").write_text("\n".join(lines), encoding="utf-8")
    exe = tmp_path / ("cases.exe" if sys.platform == "win32" else "cases")
    sources = [tmp_path / "main.c", tmp_path / "snippet.c", TEST_RUNTIME]
    host_cc().build(sources, exe, [INCLUDE, SUPPORT])
    status, out = run_exe(exe)
    assert status == 0, f"exited with {status}:\n{out[-2000:]}\n--- C ---\n{snippet.source}"
    results: list[dict[str, int]] = [{} for _ in cases]
    for line in out.splitlines():
        index, _, rest = line.partition(" ")
        key, _, value = rest.partition("=")
        results[int(index)][key] = int(value, 16)
    return results


def run_snippet(
    tmp_path: Path,
    code: bytes,
    state: dict[str, int] | None = None,
    memory: dict[int, bytes] | None = None,
    read: dict[str, tuple[int, int]] | None = None,
    extra_c: str = "",
    functions: dict[int, str] | None = None,
    arm: bool = False,
    expect_trap: bool = False,
) -> dict[str, int]:
    """Lift ``code`` at :data:`SNIPPET_BASE`, run it from ``state`` (register -> value) with
    ``memory`` (guest address -> bytes) preloaded, and return the final state. ``read``
    names guest words to report: ``{"name": (address, size)}``. ``extra_c`` is added to the
    driver, e.g. definitions of functions the snippet calls; ``functions`` (guest function
    pointer -> C name, defined in ``extra_c``) is the dispatch table for indirect calls.
    ``arm``: the code is A32, not Thumb. ``expect_trap``: the code ends in an ``mt_trap``
    (``trap_kind``, ``trap_pc`` and ``trap_detail`` are in the result, and the state is
    the one at the trap); unsupported instructions are allowed then."""
    snippet = lift_snippet(code, SNIPPET_BASE, arm=arm)
    assert expect_trap or not snippet.unsupported, snippet.unsupported
    (tmp_path / "snippet.c").write_text(snippet.source, encoding="utf-8")
    lines = [
        '#include "test_runtime.h"',
        "#include <stdio.h>",
        "",
        "void snippet(mt_cpu *MT_RESTRICT cpu);",
        extra_c,
        "mt_import_fn mt_import_table[1];",
        'const char *const mt_import_names[1] = {""};',
    ]
    table = sorted((functions or {}).items())
    entries = [f"{{0x{ptr:08X}u, {name}}}" for ptr, name in table] or ["{0u, 0}"]
    lines += [
        f"const mt_fn_entry mt_fn_table[{len(entries)}] = {{{', '.join(entries)}}};",
        f"const uint32_t mt_fn_count = {len(table)}u;",
        "",
        "static mt_cpu cpu;  /* static: unchanged by the longjmp of an expected trap */",
        "static jmp_buf trap_jmp;" if expect_trap else "",
        "",
        "int main(void) {",
        f"    mt_test_init(&cpu, 0x{WINDOW:X}u, 0u);",
    ]
    for addr, data in sorted((memory or {}).items()):
        for i, b in enumerate(data):
            lines.append(f"    cpu.mem[0x{addr + i:X}u] = 0x{b:02X}u;")
    lines += [f"    {_assign(k, v)}" for k, v in sorted((state or {}).items())]
    if expect_trap:
        lines.append("    mt_test_trap_jmp = &trap_jmp;")
        lines.append("    if (setjmp(trap_jmp) == 0) {")
        lines.append("        snippet(&cpu);")
        lines.append('        printf("MT-NO-TRAP\\n");')
        lines.append("        return 1;")
        lines.append("    }")
    else:
        lines.append("    snippet(&cpu);")
    lines.append("    mt_test_print_state(&cpu);")
    for name, (addr, size) in sorted((read or {}).items()):
        fn = {1: "mt_ld8", 2: "mt_ld16", 4: "mt_ld32"}[size]
        lines.append(f'    printf("{name}=0x%08X\\n", {fn}(&cpu, 0x{addr:X}u, 0u));')
    lines += ["    return 0;", "}", ""]
    (tmp_path / "main.c").write_text("\n".join(lines), encoding="utf-8")
    exe = tmp_path / ("snippet.exe" if sys.platform == "win32" else "snippet")
    cc = host_cc()
    cc.build([tmp_path / "main.c", tmp_path / "snippet.c", TEST_RUNTIME], exe, [INCLUDE, SUPPORT])
    status, out = run_exe(exe)
    assert status == 0, f"snippet exited with {status}:\n{out}\n--- C ---\n{snippet.source}"
    return parse_state(out)
