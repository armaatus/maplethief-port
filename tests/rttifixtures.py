"""A synthetic C++ (Itanium ABI) image built in code (nothing derived from the IPA).

Classes, laid out like GCC 4.2 lays out the game's::

    Shape              __class_type_info, name in __TEXT,__const; vtable in __DATA,__data:
                       [~Shape D1, ~Shape D0, pure (bound to ___cxa_pure_virtual), vf3]
    Circle : Shape     __si_class_type_info: [~Circle D1, D0, Draw (looks up the
                       "Circle.Draw.1" CFString), Shape's vf3 inherited]
    ui::Named          __class_type_info, nested name N2ui5NamedE: [~Named D1, D0]
    Badge : Shape, ui::Named at +8
                       __vmi_class_type_info: primary [~Badge D1, D0, vf2, Shape's vf3],
                       secondary for ui::Named (offset_to_top -8): two thunks to ~Badge
    Plain              __class_type_info with no vtable (a non-polymorphic class)

Every destructor calls ``operator delete`` through the one ``__symbolstub1`` stub
(``ldr pc, [pc, #0xFFC]`` → ``__DATA,__lazy_symbol`` bound to ``__ZdlPv``; LC_SYMTAB and the
LC_DYSYMTAB indirect table name both slots), like the -O0 D1/D0 bodies. Each function is
Thumb and in LC_FUNCTION_STARTS. Words after a vtable end it: Shape's vtable is followed by
Shape's type_info, Badge's secondary vtable by a data word.
"""

from __future__ import annotations

from machofixtures import BIND_ADDEND_SLEB, ImageBuilder, bind_stream
from thumbasm import BX_LR, NOP, POP_R7_PC, PUSH_R7_LR, add_pc, h, movt, movw, thumb_bl

TEXT = ("__TEXT", "__text")
CSTRING = ("__TEXT", "__cstring")
TCONST = ("__TEXT", "__const")
STUBS = ("__TEXT", "__symbolstub1")
LAZY = ("__DATA", "__lazy_symbol")
CFSTRING = ("__DATA", "__cfstring")
DATA = ("__DATA", "__data")
CONST = ("__DATA", "__const")

LAYOUT = {
    TEXT: (0x1800, 0x400),
    CSTRING: (0x2000, 0x200),
    TCONST: (0x2200, 0x100),
    STUBS: (0x3FFC, 4),  # 0xFFC bytes below the lazy pointer: the reach of ldr pc, [pc, #imm]
    LAZY: (0x5000, 0x10),
    CFSTRING: (0x5010, 0x40),
    DATA: (0x5100, 0x100),
    CONST: (0x5200, 0x600),
}

CLASS_TI = "__ZTVN10__cxxabiv117__class_type_infoE"
SI_TI = "__ZTVN10__cxxabiv120__si_class_type_infoE"
VMI_TI = "__ZTVN10__cxxabiv121__vmi_class_type_infoE"


SUBS_R0_8 = 0x3808


def build() -> tuple[bytes, dict[str, int]]:
    """The image bytes and a map of the interesting addresses (functions with Thumb bit)."""
    b = ImageBuilder(LAYOUT)
    a: dict[str, int] = {}
    starts: list[int] = []

    a["stub_delete"] = b.words(STUBS, 0xE59FF000 | (LAYOUT[LAZY][0] - (LAYOUT[STUBS][0] + 8)))
    a["lazy_delete"] = b.words(LAZY, 0)
    key = b.cstring("__cstring", "Circle.Draw.1")
    a["cfstr_key"] = b.words(CFSTRING, 0, 0x7C8, key, len("Circle.Draw.1"))

    def func(name: str, make) -> int:
        at = LAYOUT[TEXT][0] + len(b.bufs[TEXT])
        code = make(at)
        assert len(code) % 4 == 0
        b.put(TEXT, code)
        a[name] = at | 1
        starts.append(at | 1)
        return at | 1

    def dtor(at: int) -> bytes:
        return h(PUSH_R7_LR) + thumb_bl(at + 2, a["stub_delete"], blx=True) + h(POP_R7_PC)

    def draw(at: int) -> bytes:
        add_at = at + 8
        imm = a["cfstr_key"] - (add_at + 4)
        return movw(0, imm & 0xFFFF) + movt(0, imm >> 16) + add_pc(0) + h(BX_LR)

    def thunk(target: str):
        return lambda at: h(PUSH_R7_LR, SUBS_R0_8) + thumb_bl(at + 4, a[target]) + h(POP_R7_PC, NOP)

    for cls in ("Shape", "Circle", "Named", "Badge"):
        func(f"{cls}::D1", dtor)
        func(f"{cls}::D0", dtor)
        if cls == "Shape":
            func("Shape::vf3", lambda at: h(BX_LR, NOP))
        if cls == "Circle":
            func("Circle::Draw", draw)
    func("Badge::vf2", lambda at: h(BX_LR, NOP))
    func("Badge::D1@thunk", thunk("Badge::D1"))
    func("Badge::D0@thunk", thunk("Badge::D0"))

    # Shape: vtable in __data, then its type_info (name in __TEXT,__const).
    a["Shape.vtable"] = b.words(DATA, 0, 0, a["Shape::D1"], a["Shape::D0"], 0, a["Shape::vf3"])
    a["Shape.pure_slot"] = a["Shape.vtable"] + 16
    a["Shape.name"] = b.put(TCONST, b"5Shape\0")
    a["Shape"] = b.words(DATA, 0, a["Shape.name"])
    b.patch(a["Shape.vtable"] + 4, a["Shape"])

    # Circle : Shape
    a["Circle.vtable"] = b.words(
        CONST, 0, 0, a["Circle::D1"], a["Circle::D0"], a["Circle::Draw"], a["Shape::vf3"]
    )
    a["Circle"] = b.words(CONST, 0, b.cstring("__cstring", "6Circle"), a["Shape"])
    b.patch(a["Circle.vtable"] + 4, a["Circle"])

    # ui::Named
    a["Named.vtable"] = b.words(CONST, 0, 0, a["Named::D1"], a["Named::D0"])
    a["Named"] = b.words(CONST, 0, b.cstring("__cstring", "N2ui5NamedE"))
    b.patch(a["Named.vtable"] + 4, a["Named"])

    # Badge : Shape, ui::Named at +8 (primary + secondary vtable, then a data word)
    a["Badge.vtable"] = b.words(
        CONST, 0, 0, a["Badge::D1"], a["Badge::D0"], a["Badge::vf2"], a["Shape::vf3"]
    )
    a["Badge.vtable2"] = b.words(
        CONST, (-8) & 0xFFFFFFFF, 0, a["Badge::D1@thunk"], a["Badge::D0@thunk"], 0x12345678
    )
    a["Badge"] = b.words(
        CONST,
        0,
        b.cstring("__cstring", "5Badge"),
        0,
        2,
        a["Shape"],
        0 << 8 | 2,
        a["Named"],
        8 << 8 | 2,
    )
    b.patch(a["Badge.vtable"] + 4, a["Badge"])
    b.patch(a["Badge.vtable2"] + 4, a["Badge"])

    a["Plain"] = b.words(CONST, 0, b.cstring("__cstring", "5Plain"))

    binds = bind_stream(
        [
            (a["lazy_delete"], "__ZdlPv", 0),
            (a["Shape.pure_slot"], "___cxa_pure_virtual", 0),
            (a["Shape"], CLASS_TI, 8),
            (a["Named"], CLASS_TI, 8),
            (a["Plain"], CLASS_TI, 8),
            (a["Circle"], SI_TI, 8),
            (a["Badge"], VMI_TI, 8),
        ]
    )
    assert BIND_ADDEND_SLEB in binds
    image = b.build(
        bind=binds,
        function_starts=starts,
        undefined=["__ZdlPv"],
        indirect={STUBS: ["__ZdlPv"], LAZY: ["__ZdlPv"]},
    )
    return image, a
