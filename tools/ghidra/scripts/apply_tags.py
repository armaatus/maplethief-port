# Put the function tags of work/re/function-tags.json (#10, tools/mtport/libtags.py) on the
# program's functions as Ghidra function tags: app-objc, engine, unknown or lib:<name>.
#
# Every tagged function gets exactly its one tag. Tags of this scheme that a function no
# longer has are removed; other function tags (made by hand in the GUI) are left alone. Each
# tag's comment says what it means. Idempotent: a second run reports "0 changes".
#
# Then it reads this scheme's tags back from every function in the program and writes them
# to work/re/ghidra-tags.json (libtags.readback_json, deterministic), so tests can check that
# Ghidra carries exactly the tags of TAGS_JSON without starting Ghidra.
# Run with: tools/ghidra/run-script apply_tags.py [TAGS_JSON [READBACK_JSON]]
# @category MapleThief
# @runtime PyGhidra
# ruff: noqa: F821  (currentProgram, getScriptArgs, ... are GhidraScript globals)

import sys
from pathlib import Path

_HERE = Path(str(getSourceFile().getAbsolutePath())).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))
import mtport_loader  # noqa: E402  (loads mtport; see mtport_loader.py)

from mtport import ghidra_common as gc  # noqa: E402
from mtport import libtags  # noqa: E402


def main():
    args = list(getScriptArgs())
    path = Path(args[0]) if args else mtport_loader.REPO / libtags.JSON_OUT
    tags = libtags.load(path)
    plan = libtags.ghidra_plan(tags.doc)
    fm = currentProgram.getFunctionManager()
    tm = fm.getFunctionTagManager()
    space = currentProgram.getAddressFactory().getDefaultAddressSpace()
    changes = 0
    for name, comment in sorted(libtags.tag_comments(tags.doc).items()):
        tag = tm.getFunctionTag(name)
        if tag is None:
            tm.createFunctionTag(name, comment)
            changes += 1
        elif tag.getComment() != comment:
            tag.setComment(comment)
            changes += 1
    missing = []
    for addr, want in sorted(plan.items()):
        f = fm.getFunctionAt(space.getAddress(addr))
        if f is None:
            missing.append(f"0x{addr:X}")
            continue
        have = {t.getName() for t in f.getTags()}
        for old in sorted(n for n in have if libtags.is_scheme_tag(n) and n != want):
            f.removeTag(old)
            changes += 1
        if want not in have:
            f.addTag(want)
            changes += 1
    if missing:
        raise SystemExit(f"{len(missing)} tagged functions are not in the program: {missing[:10]}")
    counts = {}
    for want in plan.values():
        counts[want] = counts.get(want, 0) + 1
    summary = ", ".join(f"{k} {v}" for k, v in sorted(counts.items()))
    print(f"tagged {len(plan)} functions from {path} ({changes} changes): {summary}")

    held = {
        f.getEntryPoint().getOffset(): [t.getName() for t in f.getTags()]
        for f in fm.getFunctions(True)
    }
    out = Path(args[1]) if len(args) > 1 else mtport_loader.REPO / libtags.READBACK_JSON
    gc.write_text(out, libtags.readback_json(tags.doc, held))
    print(f"read the tags back into {out}")


main()
