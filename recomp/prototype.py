"""The functions ADR 0014's prototype lifts end to end, kept as a regression set (#30).

Each is lifted from the armv7 slice, compiled with clang ``-std=c17 -Wall -Wextra -Werror``
and run against a reference by ``tests/test_recomp_prototype_real.py`` (local only: it needs
the IPA). The generated C goes to ``work/gen/proto/`` and is never committed.
"""

from __future__ import annotations

from pathlib import Path

from recomp import REPO
from recomp.discover import discover_function
from recomp.imports import ImportTable
from recomp.lift.core import Lifted, c_name, lift_function
from recomp.loader import Image
from recomp.output import write_program

OUT_DIR = REPO / "work" / "gen" / "proto"

# (function pointer, name, what it exercises)
FUNCTIONS = (
    (0x225E5, "Rand_Next", "leaf: MSVC rand() LCG on a global, literal pool, mul, shifts"),
    (0x226B9, "Rand_Percent", "IT block (movle), direct call, smmul + mls, push/pop {pc}"),
    (0xB45FD, "TouchKeyPadView_Distance", "VFP: f32 sub/mul/add, f32<->f64, vsqrt.f64"),
    (0xCA049, "-[CBReachability networkStatusForFlags:]", "5 IT blocks, bxeq lr, ubfx"),
    (0x22685, "Rand_Chance", "call through an import stub (___modsi3)"),
)


class ImageProgram:
    """:class:`recomp.lift.core.Program` over an image and the functions being lifted."""

    def __init__(self, image: Image, lifted: set[int], imports: ImportTable) -> None:
        self.image, self.lifted, self.imports = image, lifted, imports

    def const_word(self, addr: int, size: int) -> int | None:
        return self.image.const_word(addr, size)

    def callee(self, ptr: int) -> str | None:
        return c_name(ptr) if ptr in self.lifted else None

    def import_at(self, addr: int) -> tuple[int, str] | None:
        return self.imports.at(addr)


def lift(image: Image, ptrs: list[int], names: dict[int, str] | None = None) -> list[Lifted]:
    imports = ImportTable.from_image(image)
    program = ImageProgram(image, set(ptrs), imports)
    out = []
    for ptr in ptrs:
        fn = discover_function(image.code, image.text_base, ptr, image.function_end(ptr))
        label = (names or {}).get(ptr)
        comment = f"{ptr:#x} {label}" if label else f"{ptr:#x}"
        out.append(lift_function(fn, program, comment=comment))
    return out


def build(
    slice_path: str | Path | None = None, out: Path = OUT_DIR
) -> tuple[list[Lifted], list[Path]]:
    """Lift :data:`FUNCTIONS` into ``out``; returns the lifted functions and C sources."""
    image = Image.load(slice_path) if slice_path else Image.load()
    names = {p: n for p, n, _ in FUNCTIONS}
    lifted = lift(image, [p for p, _, _ in FUNCTIONS], names)
    return lifted, write_program(out, lifted, ImportTable.from_image(image))
