# Seed functions from LC_FUNCTION_STARTS before auto-analysis (ADR 0007).
#
# For every start, sets the ARM TMode context register (1 = Thumb, from the start's low bit)
# over the code from that start up to the next one, disassembles from the start in that mode
# and creates a function there. Without this, Ghidra decodes the Thumb-2 code as ARM.
# Args: SLICE (the armv7 Mach-O the program was imported from).
# @category MapleThief
# @runtime PyGhidra
# ruff: noqa: F821  (currentProgram, monitor, getScriptArgs, ... are GhidraScript globals)

import sys
from pathlib import Path

_HERE = Path(str(getSourceFile().getAbsolutePath())).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))
import mtport_loader  # noqa: E402  (loads mtport; see mtport_loader.py)

_TOOLS = mtport_loader.TOOLS

from ghidra.app.cmd.disassemble import ArmDisassembleCommand  # noqa: E402
from ghidra.app.cmd.function import CreateFunctionCmd  # noqa: E402
from ghidra.program.model.address import AddressSet  # noqa: E402
from ghidra.program.model.symbol import SourceType  # noqa: E402
from java.math import BigInteger  # noqa: E402

from mtport import macho  # noqa: E402
from mtport.ghidra_common import seed_ranges  # noqa: E402


def main() -> None:
    args = list(getScriptArgs())
    if len(args) != 1:
        raise SystemExit("usage: seed_functions.py SLICE")
    image = macho.load(args[0])
    plan = seed_ranges(image)
    program = currentProgram
    space = program.getAddressFactory().getDefaultAddressSpace()
    ctx = program.getProgramContext()
    tmode = ctx.getRegister("TMode")

    thumb, arm, entries = AddressSet(), AddressSet(), AddressSet()
    for r in plan:
        start, last = space.getAddress(r.start), space.getAddress(r.end - 1)
        ctx.setValue(tmode, start, last, BigInteger.ONE if r.thumb else BigInteger.ZERO)
        (thumb if r.thumb else arm).add(start)
        entries.add(start)

    for starts, is_thumb in ((thumb, True), (arm, False)):
        cmd = ArmDisassembleCommand(starts, None, is_thumb)
        if not cmd.applyTo(program, monitor):
            raise SystemExit(f"disassembly failed: {cmd.getStatusMsg()}")
    cmd = CreateFunctionCmd(entries, SourceType.ANALYSIS)
    cmd.applyTo(program, monitor)

    fm = program.getFunctionManager()
    made = sum(1 for r in plan if fm.getFunctionAt(space.getAddress(r.start)) is not None)
    print(
        f"seeded {len(plan)} starts ({thumb.getNumAddresses()} Thumb, "
        f"{arm.getNumAddresses()} ARM); {made} functions created"
    )


main()
