"""Lifting: discovered functions -> C (ADR 0014).

``core`` owns the function frame, labels, conditions, the flag discipline and the emission
API (:class:`~recomp.lift.core.Ctx`); each instruction class has its own module that
registers one lifter per mnemonic: ``alu`` (data processing), ``mul`` (multiplies), ``ldst``
(loads and stores), ``branch`` (branches, calls, returns, IT), ``misc`` (bit fields,
extends, hints, traps), ``vfp`` and ``neon`` (Advanced SIMD, registered as
``neon:<mnemonic>``). A new instruction form goes into its class's module; a
mnemonic with no lifter becomes an unsupported-instruction trap.
"""

from __future__ import annotations

from recomp.lift import alu, branch, ldst, misc, mul, neon, vfp  # noqa: F401  (register lifters)
from recomp.lift.core import (
    LIFTERS,
    Ctx,
    Lifted,
    LiftOptions,
    Program,
    Unsupported,
    c_name,
    lift_function,
    lifter,
)

__all__ = [
    "LIFTERS",
    "Ctx",
    "LiftOptions",
    "Lifted",
    "Program",
    "Unsupported",
    "c_name",
    "lift_function",
    "lifter",
]
