# Export functions, xrefs and strings from the current program as deterministic JSON (ADR 0007).
#
# Writes OUT_DIR/functions.json, OUT_DIR/xrefs.json and OUT_DIR/strings.json. The schema is
# in tools/ghidra/README.md; the layout (sorted, one record per line, no timestamps) comes
# from mtport.ghidra_common.dumps, so two exports of equal programs are byte-identical.
# Also prints how Ghidra's functions compare with LC_FUNCTION_STARTS.
# Args: OUT_DIR SLICE [NAMES_DIR]
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

from ghidra.framework import Application  # noqa: E402

from mtport import macho  # noqa: E402
from mtport.ghidra_common import (  # noqa: E402
    SCHEMA_VERSION,
    compare_starts,
    dumps,
    hexaddr,
    macho_strings,
    pointer,
    write_text,
)

PROPERTY = "mtport.name"


class Exporter:
    def __init__(self, program, image):
        self.program = program
        self.image = image
        self.listing = program.getListing()
        self.fm = program.getFunctionManager()
        self.rm = program.getReferenceManager()
        self.ctx = program.getProgramContext()
        self.tmode = self.ctx.getRegister("TMode")
        self.names = program.getUsrPropertyManager().getStringPropertyMap(PROPERTY)
        self._thumb = {}

    def is_thumb(self, addr):
        key = addr.getOffset()
        if key not in self._thumb:
            insn = self.listing.getInstructionAt(addr)
            value = (
                insn.getValue(self.tmode, False)
                if insn is not None
                else self.ctx.getValue(self.tmode, addr, False)
            )
            self._thumb[key] = value is not None and value.intValue() == 1
        return self._thumb[key]

    def func_ref(self, f):
        """A function as JSON: its pointer, or "EXTERNAL:<library>::<name>"."""
        if f.isExternal():
            loc = f.getExternalLocation()
            return f"EXTERNAL:{loc.getLibraryName()}::{loc.getLabel()}"
        entry = f.getEntryPoint()
        return pointer(entry.getOffset(), self.is_thumb(entry))

    def name_of(self, f):
        entry = f.getEntryPoint()
        contract = self.names.getString(entry) if self.names is not None else None
        if contract is not None:
            return str(contract), "names"
        sym = f.getSymbol()
        return str(f.getName(True)), str(sym.getSource()).lower()

    def section_of(self, offset):
        s = self.image.section_at(offset)
        return f"{s.segname},{s.sectname}" if s else None

    def block_of(self, addr):
        """Ghidra's memory block: a Mach-O section, or a block Ghidra made up (EXTERNAL,
        the ObjC message analyzer's selector stubs, ...)."""
        block = self.program.getMemory().getBlock(addr)
        return str(block.getName()) if block is not None else None

    def functions(self, starts):
        start_set = {raw & ~1 for raw in starts}
        records, modes = [], {}
        for f in self.fm.getFunctions(True):
            entry = f.getEntryPoint()
            off = entry.getOffset()
            thumb = self.is_thumb(entry)
            modes[off] = thumb
            body = [
                [hexaddr(r.getMinAddress().getOffset()), hexaddr(r.getMaxAddress().getOffset() + 1)]
                for r in f.getBody().getAddressRanges()
            ]
            name, source = self.name_of(f)
            thunk = f.getThunkedFunction(False) if f.isThunk() else None
            records.append(
                {
                    "address": pointer(off, thumb),
                    "mode": "thumb" if thumb else "arm",
                    "size": int(f.getBody().getNumAddresses()),
                    "body": body,
                    "name": name,
                    "name_source": source,
                    "section": self.section_of(off),
                    "block": self.block_of(entry),
                    "in_function_starts": off in start_set,
                    "thunk_of": self.func_ref(thunk) if thunk is not None else None,
                    "no_return": bool(f.hasNoReturn()),
                    "callers": sorted({self.func_ref(c) for c in f.getCallingFunctions(monitor)}),
                    "callees": sorted({self.func_ref(c) for c in f.getCalledFunctions(monitor)}),
                }
            )
        records.sort(key=lambda r: int(r["address"], 16) & ~1)
        return records, compare_starts(starts, modes)

    def to_string(self, ref):
        to = ref.getToAddress()
        if ref.isExternalReference():
            loc = ref.getExternalLocation()
            return f"EXTERNAL:{loc.getLibraryName()}::{loc.getLabel()}"
        if to.isMemoryAddress():
            return hexaddr(to.getOffset())
        return str(to)

    def xrefs(self):
        records = []
        memory = self.program.getMemory()
        for src in self.rm.getReferenceSourceIterator(memory, True):
            func = self.fm.getFunctionContaining(src)
            func_s = self.func_ref(func) if func is not None else None
            for ref in self.rm.getReferencesFrom(src):
                if ref.isStackReference() or ref.isRegisterReference():
                    continue
                records.append(
                    {
                        "from": hexaddr(src.getOffset()),
                        "to": self.to_string(ref),
                        "type": str(ref.getReferenceType().getName()),
                        "operand": int(ref.getOperandIndex()),
                        "function": func_s,
                    }
                )
        records.sort(key=lambda r: (int(r["from"], 16), r["operand"], r["to"], r["type"]))
        return records

    def strings(self):
        space = self.program.getAddressFactory().getDefaultAddressSpace()
        records = macho_strings(self.image)
        for rec in records:
            addr = space.getAddress(int(rec["address"], 16))
            rec["refs"] = sorted(
                {r.getFromAddress().getOffset() for r in self.rm.getReferencesTo(addr)}
            )
            rec["refs"] = [hexaddr(a) for a in rec["refs"]]
        return records


def main():
    args = list(getScriptArgs())
    if len(args) < 2:
        raise SystemExit("usage: export.py OUT_DIR SLICE [NAMES_DIR]")
    out, slice_path = Path(args[0]), Path(args[1])
    image = macho.load(slice_path)
    program = currentProgram
    exporter = Exporter(program, image)
    try:
        slice_rel = slice_path.resolve().relative_to(_TOOLS.parent).as_posix()
    except ValueError:
        slice_rel = slice_path.name
    header = {
        "schema": SCHEMA_VERSION,
        "ghidra": str(Application.getApplicationVersion()),
        "program": {
            "name": str(program.getName()),
            "slice": slice_rel,
            "sha256": str(program.getExecutableSHA256()),
            "language": str(program.getLanguageID()),
            "compiler": str(program.getCompilerSpec().getCompilerSpecID()),
            "image_base": hexaddr(program.getImageBase().getOffset()),
        },
    }

    functions, report = exporter.functions(image.function_starts)
    extra_by_section = {}
    for rec in functions:
        if not rec["in_function_starts"]:
            key = rec["section"] or f"ghidra block {rec['block']}"
            extra_by_section[key] = extra_by_section.get(key, 0) + 1
    report["extra_by_section"] = extra_by_section
    thumb = sum(1 for r in functions if r["mode"] == "thumb")
    counts = {"functions": len(functions), "thumb": thumb, "arm": len(functions) - thumb}
    write_text(
        out / "functions.json",
        dumps({**header, "counts": counts, "function_starts": report}, "functions", functions),
    )
    xrefs = exporter.xrefs()
    write_text(out / "xrefs.json", dumps({**header, "count": len(xrefs)}, "xrefs", xrefs))
    strings = exporter.strings()
    write_text(out / "strings.json", dumps({**header, "count": len(strings)}, "strings", strings))

    print(
        f"exported {len(functions)} functions ({thumb} Thumb), {len(xrefs)} xrefs, "
        f"{len(strings)} strings to {out}"
    )
    print(
        f"LC_FUNCTION_STARTS {report['function_starts']}: matched {report['matched']}, "
        f"missing {len(report['missing'])}, wrong mode {len(report['wrong_mode'])}, "
        f"extra {len(report['extra'])} {extra_by_section}"
    )
    if report["missing"] or report["wrong_mode"]:
        print(f"  missing: {report['missing'][:20]}  wrong mode: {report['wrong_mode'][:20]}")


main()
