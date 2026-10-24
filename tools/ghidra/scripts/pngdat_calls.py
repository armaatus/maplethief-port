# List every call to the png/**/*.dat pack loaders with its constant arguments.
#
# The pack loaders (docs/formats/png-dat.md) are 0xF5A9 (flat array), 0xF6A5 (2-D),
# 0xF445 (3-D) and 0xF34D (one image). For each call this prints the call address, the
# helper and the arguments after `this`: the path string, the array (`?`, not constant), the
# dimensions or index, and the pack id (the length-table row). mtport.pngdat.PACKS was built
# from this output. Commits the helpers' decompiled parameters first, so the decompiler shows
# the stack arguments; nothing else in the program changes.
# Args: OUT_FILE
# @category MapleThief
# @runtime PyGhidra
# ruff: noqa: F821  (currentProgram, getScriptArgs, getReferencesTo are GhidraScript globals)

from ghidra.app.decompiler import DecompInterface
from ghidra.program.model.pcode import HighFunctionDBUtil, PcodeOp
from ghidra.program.model.symbol import SourceType
from ghidra.util.task import ConsoleTaskMonitor

HELPERS = {0xF5A8: "load_flat", 0xF6A4: "load_2d", 0xF34C: "load_one", 0xF444: "load_3d"}

out_file = list(getScriptArgs())[0]
program = currentProgram
space = program.getAddressFactory().getDefaultAddressSpace()
functions = program.getFunctionManager()
memory = program.getMemory()
monitor = ConsoleTaskMonitor()
decomp = DecompInterface()
decomp.openProgram(program)


def addr(value):
    return space.getAddress(value)


def resolve(varnode, depth=0):
    """A varnode's value when it folds to a constant (literal-pool loads included), else None."""
    if varnode is None or depth > 12:
        return None
    if varnode.isConstant():
        return varnode.getOffset()
    if varnode.isAddress():
        return memory.getInt(varnode.getAddress()) & 0xFFFFFFFF
    op = varnode.getDef()
    if op is None:
        return None
    code = op.getOpcode()
    ins = [resolve(op.getInput(i), depth + 1) for i in range(op.getNumInputs())]
    if code in (PcodeOp.COPY, PcodeOp.CAST, PcodeOp.INT_ZEXT, PcodeOp.INT_SEXT):
        return ins[0]
    if code in (PcodeOp.INT_ADD, PcodeOp.PTRSUB) and None not in ins:
        return (ins[0] + ins[1]) & 0xFFFFFFFF
    if code == PcodeOp.LOAD and ins[1] is not None:
        return memory.getInt(addr(ins[1])) & 0xFFFFFFFF
    if code == PcodeOp.MULTIEQUAL and len(set(ins)) == 1:
        return ins[0]
    return None


def show(value):
    if value is None:
        return "?"
    block = memory.getBlock(addr(value)) if value > 0x1000 else None
    if block is not None:
        text = []
        for i in range(80):
            byte = memory.getByte(addr(value + i)) & 0xFF
            if byte == 0:
                break
            text.append(chr(byte))
        if "".join(text).startswith("/png/"):
            return repr("".join(text))
    return hex(value)


for entry in HELPERS:
    result = decomp.decompileFunction(functions.getFunctionAt(addr(entry)), 120, monitor)
    HighFunctionDBUtil.commitParamsToDatabase(
        result.getHighFunction(),
        True,
        HighFunctionDBUtil.ReturnCommitOption.NO_COMMIT,
        SourceType.USER_DEFINED,
    )

callers = set()
for entry in HELPERS:
    for ref in getReferencesTo(addr(entry)):
        function = functions.getFunctionContaining(ref.getFromAddress())
        if function is not None:
            callers.add(function.getEntryPoint().getOffset())

lines = []
for caller in sorted(callers):
    function = functions.getFunctionAt(addr(caller))
    high = decomp.decompileFunction(function, 300, monitor).getHighFunction()
    if high is None:
        lines.append(f"0x{caller | 1:X} decompile failed")
        continue
    for op in high.getPcodeOps():
        if op.getOpcode() != PcodeOp.CALL:
            continue
        target = op.getInput(0).getAddress().getOffset()
        if target not in HELPERS:
            continue
        args = [show(resolve(op.getInput(i))) for i in range(2, op.getNumInputs())]
        at = op.getSeqnum().getTarget().getOffset()
        lines.append(f"0x{caller | 1:X} call 0x{at:X} {HELPERS[target]} {' '.join(args)}")

with open(out_file, "w") as handle:
    handle.write("\n".join(sorted(lines)) + "\n")
print(f"{len(lines)} pack loads written to {out_file}")
