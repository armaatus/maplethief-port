# Apply every work/re/names/*.json to the current program (the names contract, ADR 0007).
#
# Schema, merging rules and the mapping of names such as "-[Class sel:]" or "Class::vf3"
# to Ghidra symbols: tools/mtport/names.py and tools/ghidra/README.md. Idempotent: a second
# run with the same files changes nothing and reports "0 changes".
#
#   function  creates the function if missing (Thumb bit -> TMode, disassembles first),
#             renames it (source USER_DEFINED) and sets the comment as its plate comment
#   label     adds a USER_DEFINED label at the code address and makes it primary; EOL comment
#   data      the same at the exact address
#
# Every applied address also gets its exact contract name in the program's string property
# map "mtport.name", which export.py reports as the name.
# Args: [NAMES_DIR] (default work/re/names). Fails, listing every problem, if any entry
# can't be applied.
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
from ghidra.program.model.listing import CommentType  # noqa: E402
from ghidra.program.model.symbol import SourceType  # noqa: E402
from java.math import BigInteger  # noqa: E402

from mtport import names  # noqa: E402

PROPERTY = "mtport.name"
USER = SourceType.USER_DEFINED


class Applier:
    def __init__(self, program):
        self.program = program
        self.space = program.getAddressFactory().getDefaultAddressSpace()
        self.st = program.getSymbolTable()
        self.fm = program.getFunctionManager()
        self.listing = program.getListing()
        self.ctx = program.getProgramContext()
        self.tmode = self.ctx.getRegister("TMode")
        pm = program.getUsrPropertyManager()
        self.prop = pm.getStringPropertyMap(PROPERTY) or pm.createStringPropertyMap(PROPERTY)
        self.changes = 0
        self.errors = []

    def namespace(self, parts):
        ns = self.program.getGlobalNamespace()
        for part in parts:
            child = self.st.getNamespace(part, ns)
            if child is None:
                child = self.st.createNameSpace(ns, part, USER)
                self.changes += 1
            ns = child
        return ns

    def mode_at(self, addr):
        insn = self.listing.getInstructionAt(addr)
        value = (
            insn.getValue(self.tmode, False) if insn else self.ctx.getValue(self.tmode, addr, False)
        )
        return value is not None and value.intValue() == 1

    def function(self, entry, addr, ns, symbol):
        f = self.fm.getFunctionAt(addr)
        if f is None:
            if self.listing.getInstructionAt(addr) is None:
                self.listing.clearCodeUnits(addr, addr, False)
                value = BigInteger.ONE if entry.thumb else BigInteger.ZERO
                self.ctx.setValue(self.tmode, addr, addr, value)
                ArmDisassembleCommand(addr, None, entry.thumb).applyTo(self.program, monitor)
            if self.listing.getInstructionAt(addr) is None:
                return "no instruction could be decoded there"
            if self.mode_at(addr) != entry.thumb:
                return "existing code at that address is in the other ARM/Thumb mode"
            CreateFunctionCmd(addr).applyTo(self.program, monitor)
            f = self.fm.getFunctionAt(addr)
            if f is None:
                return "Ghidra could not create a function there"
            self.changes += 1
        elif self.mode_at(addr) != entry.thumb:
            return "the existing function there is in the other ARM/Thumb mode"
        sym = f.getSymbol()
        if not sym.getParentNamespace().equals(ns):
            f.setParentNamespace(ns)
            self.changes += 1
        if f.getName() != symbol or not sym.getSource().equals(USER):
            # A plain label with the wanted name at the entry (e.g. the loader's "start")
            # would make the rename fail as a duplicate; the function symbol replaces it.
            clash = self.st.getSymbol(symbol, addr, ns)
            if clash is not None and not clash.equals(sym):
                clash.delete()
            f.setName(symbol, USER)
            self.changes += 1
        return None

    def label(self, addr, ns, symbol):
        sym = self.st.getSymbol(symbol, addr, ns)
        if sym is None:
            sym = self.st.createLabel(addr, symbol, ns, USER)
            self.changes += 1
        if not sym.isPrimary() and self.fm.getFunctionAt(addr) is None:
            sym.setPrimary()
            self.changes += 1
        return None

    def comment(self, addr, kind, text):
        ctype = CommentType.PLATE if kind == "function" else CommentType.EOL
        if text and self.listing.getComment(ctype, addr) != text:
            self.listing.setComment(addr, ctype, text)
            self.changes += 1

    def apply(self, merged):
        e = merged.entry
        addr = self.space.getAddress(e.location)
        where = f"0x{e.address:X} {e.kind} {e.name!r} ({', '.join(merged.sources)})"
        if not self.program.getMemory().contains(addr):
            self.errors.append(f"{where}: address is not in the program's memory")
            return
        parts, symbol = names.ghidra_symbol(e.name)
        try:
            ns = self.namespace(parts)
            if e.kind == "function":
                problem = self.function(e, addr, ns, symbol)
            else:
                problem = self.label(addr, ns, symbol)
        except Exception as ex:  # Java exceptions (InvalidInputException, ...) included
            problem = f"{type(ex).__name__}: {ex}"
        if problem:
            self.errors.append(f"{where}: {problem}")
            return
        self.comment(addr, e.kind, merged.comment)
        if self.prop.getString(addr) != e.name:
            self.prop.add(addr, e.name)
            self.changes += 1


def main():
    args = list(getScriptArgs())
    directory = Path(args[0]) if args else _TOOLS.parent / names.NAMES_DIR
    try:
        merged = names.load_dir(directory)
    except names.NamesError as e:
        raise SystemExit("names files are invalid:\n  " + "\n  ".join(e.problems)) from None
    applier = Applier(currentProgram)
    for m in merged:
        applier.apply(m)
    if applier.errors:
        raise SystemExit(
            f"{len(applier.errors)} of {len(merged)} names could not be applied:\n  "
            + "\n  ".join(applier.errors)
        )
    print(f"applied {len(merged)} names from {directory} ({applier.changes} changes)")


main()
