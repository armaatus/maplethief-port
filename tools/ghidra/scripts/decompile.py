# Decompile functions to a text file, for reading (the output is derived data: keep it in
# work/ or a scratch directory, never in git).
#
# Each argument after OUT_FILE is a function pointer or code address in hex (the Thumb bit is
# ignored); the function containing it is decompiled. An argument @FILE reads whitespace-separated
# addresses from FILE. With no addresses, nothing is written.
# Read-only: nothing in the program changes.
# Args: OUT_FILE ADDRESS...
# @category MapleThief
# @runtime PyGhidra
# ruff: noqa: F821  (currentProgram, getScriptArgs are GhidraScript globals)

from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

args = list(getScriptArgs())
out_file, wanted = args[0], []
for arg in args[1:]:
    if arg.startswith("@"):
        with open(arg[1:], encoding="utf-8") as fh:
            wanted.extend(fh.read().split())
    else:
        wanted.append(arg)
program = currentProgram
space = program.getAddressFactory().getDefaultAddressSpace()
functions = program.getFunctionManager()
decomp = DecompInterface()
decomp.openProgram(program)
monitor = ConsoleTaskMonitor()

parts = []
for text in wanted:
    address = space.getAddress(int(text, 16) & ~1)
    function = functions.getFunctionContaining(address)
    if function is None:
        parts.append("// %s: no function\n" % text)
        continue
    result = decomp.decompileFunction(function, 180, monitor)
    body = result.getDecompiledFunction()
    header = "// %s %s @ %s\n" % (text, function.getName(True), function.getEntryPoint())
    parts.append(header + (body.getC() if body is not None else "// decompile failed\n"))

with open(out_file, "w", encoding="utf-8") as fh:
    fh.write("\n".join(parts))
print("decompiled %d function(s) to %s" % (len(wanted), out_file))
