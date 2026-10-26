"""The static recompiler: armv7 Mach-O slice -> C17 (ADR 0014).

Pipeline, one module (or subpackage) per stage; a stage imports only earlier stages::

    loader      the image: segments, read-only bytes, function starts, stubs -> imports  (#31)
    decode/     Thumb-2 / ARM / VFP decoder: bytes -> Insn                       (#33, #34)
    discover    functions: instructions, successors, labels, IT conditions           (#32)
    lift/       Insn -> C statements, one module per instruction class         (#33, #34)
    imports     import ids and work/gen/imports.c                                    (#36)
    dispatch    guest address -> host function table, work/gen/dispatch.c            (#35)
    output      translation units, mt_gen.h, gen.cmake, write-if-changed            (#38)
    snippet     lift a byte string as one function, for the snippet tests            (#39)
    prototype   the functions lifted end to end for ADR 0014                        (#30)

The generated C includes ``runtime/include/maplethief/recomp/recomp.h``, the committed
contract between generated code and the runtime. Command line: ``python -m recomp --help``
from the repository root. Outputs go to ``work/recomp/`` (JSON reports) and ``work/gen/``
(C), both git-ignored. The recompiler uses only the standard library, so the build can run
it with any Python 3.10+; Capstone (a test dependency) is the decoder's oracle in the tests.

The input is untrusted data: it is parsed, never executed.
"""

from __future__ import annotations

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]

# One Mach-O parser for tools and recompiler (ADR 0002, ADR 0012): recomp imports mtport from
# tools/. pytest puts tools/ on sys.path itself (pyproject.toml); `python -m recomp` does not.
_TOOLS = str(REPO / "tools")
if _TOOLS not in sys.path:
    sys.path.append(_TOOLS)
