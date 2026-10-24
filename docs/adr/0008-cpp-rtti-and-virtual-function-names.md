# 8. C++ RTTI recovery and virtual-function names

- **Status:** Accepted
- **Date:** 2026-10-24
- **Issue:** #9

## Context

The engine is C++ built by GCC 4.2 (Itanium ABI, -O0, SjLj exceptions) and stripped: no symbol names survive, only the RTTI `type_info` objects of its 63 polymorphic classes ([docs/ANALYSIS.md → C++ engine](../ANALYSIS.md#c-engine)). Their vtables are the only map from class to code. Later work builds on that map: program flow ([#12](https://github.com/armaatus/maplethief-port/issues/12)) follows the `IState` virtual calls, and the script VM ([#19](https://github.com/armaatus/maplethief-port/issues/19)) starts from `Lua`, `TFunc` and `Undump`. The names have to be stable across re-imports (ADR 0007's names contract), the same for an override and the function it overrides, and never claim more than the binary shows.

## Options considered

1. **Ghidra's RTTI analysis**: Ghidra 12.2 has no GCC/Itanium RTTI analyzer that runs on this stripped Mach-O, and it would tie the class map to a Ghidra run (ADR 0007 keeps analyses Ghidra-free so CI can test them).
2. **A Python analysis of the Mach-O** (`tools/mtport/rtti.py`): the `type_info` vptrs are dyld binds to the three `__cxxabiv1` vtables, which makes every `type_info` and from there every vtable findable exactly. Testable in CI on a synthetic image, deterministic, and the output can be diffed.

For the names:

1. **Number every slot per class** (`CStateGame::vf9`): simple, but an override and the function it overrides get unrelated names if numbering restarts per class.
2. **Number by the slot in the class that declared it** and replace the number with a method name only when the binary itself spells that name.

## Decision

`python3 -m mtport.rtti generate` (option 2) recovers classes, bases, vtables, slot owners, destructors and thunks from the Mach-O and its binds alone, and writes `work/re/vtables.json`, `work/re/names/rtti.json` and `docs/re/cpp-classes.md`.

Names (Ghidra namespace = the class):

- `Class::vfN`: slot N of the owning class's primary vtable. A function belongs to the most-base class whose vtables hold it, and N is the slot's index, which an override shares with the virtual it overrides (`IStateAdaptor::vf13`, `CStateGame::vf2` overriding the pure `IState` slot 2).
- `Class::~Class` and `Class::~Class_deleting`: the complete-object (D1) and deleting (D0) destructors, the slot pair whose two functions both call `operator delete` (at -O0 GCC emits both from one body, with the delete behind a constant flag), kept at the primary base's position in derived classes.
- `Name@thunk-N`: a non-virtual thunk in a secondary vtable that adds -N to `this` and calls `Name`.
- `Class::Method` replaces `vfN` when a slot function of `Class` looks up `Class.Method.N` localization keys (`ScriptExtra.strings`, e.g. `CStateGame.Paint.7`). The method name then names that slot in the declaring class and in every override. Conflicting keys leave the number.
- Data labels: `Class::typeinfo`, `Class::typeinfo_name`, `Class::vtable`, `Class::vtable_for_Base` (a secondary vtable).

## Consequences

- 189 virtual functions, destructors and thunks get stable names in Ghidra; slots named from keys so far: `IState` 6 = `OnKeyPressed`, 9 = `Paint`; `CMapleObject` 4 = `Flush`. #12 and #19 can rename further slots by adding evidence (a new names file or a new evidence rule here), and the numbering stays valid for every slot they don't rename.
- The class map does not depend on Ghidra. The PC-relative string scan that finds the keys is deliberately cheap (movw/movt or a literal load, then `add rX, pc`); `tests/test_rtti_real.py` checks it against Ghidra's references.
- If a future binary had virtual bases, VTTs or construction vtables, the layout checks (`layout_problems`) would report them; this one has none.
