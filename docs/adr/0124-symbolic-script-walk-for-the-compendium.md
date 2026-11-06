# 124. A symbolic walk of the scripts for the compendium

- **Status:** Accepted
- **Date:** 2026-11-06
- **Issue:** #217 (epic #210)

## Context

The compendium's quests, dialogue, crafting, job advancements and warps live in the 307 script files (Lua 2.4 bytecode, [formats/script-out.md](../formats/script-out.md), [re/script-vm.md](../re/script-vm.md)). The knowledge base needs, for each of them, what it does and under which conditions: who gives a quest and what must hold first, when a portal lets the hero through, which answer of which box opens a shop. A conversation spans several functions: `dialog` opens a box and registers a callback, and the callback reads the answer.

The scripts use 24 of the VM's 67 opcodes: no locals, tables, loops or concatenation, and every jump goes forward. Every value is a constant, a global or a native's result. The game decides the natives' results (quest states, the level, the player's answers).

## Options considered

1. **Pattern matching on the call lists** (`scriptout.calls`), as the world stage does for constant `changeMap`: simple, but blind to conditions and to which callback continues which box.
2. **Running the scripts in the port's VM** with scripted answers: exact for one playthrough, but it needs the runtime and covers only the states it is driven through.
3. **A symbolic walk**: interpret the bytecode with the natives' results as symbols, split each branch into the cases the conditions so far allow, and follow `regCallBack` into the callback with the answer bound to the box's possible values.

## Decision

The quests stage uses a symbolic walk (`tools/mtport/compendium/scriptwalk.py`, option 3). With forward jumps only, each function has finitely many paths, and each path carries the conditions under which the game takes it. A native that changes the game state makes later reads new symbols.

Paths that meet again merge only when the result is exact. They must have the same calls and stack, and they must differ only in their conditions on one value, which together allow every value of it. A global they disagree on becomes a conditional value, which splits again where it is read. A path's conditions therefore always say exactly when it runs.

A conversation past the exact budget (100,000 paths, 20,000 in one function) is walked again with approximate merges. Those merges carry an explicit `approximate` condition into the knowledge base. In 1.5 that is one NPC-state pass of 307 files, and no quest's prerequisites.

The bytecode is untrusted data: it is decoded by `mtport.scriptout` and interpreted, never executed.

## Consequences

- Quests, recipes, trades, job advancements, warps, events and shop links come with their conditions as data. The site can say "needs main quest 21 done and level 30" without hand-written tables. Synthetic bytecode tests cover the walk (`tests/test_compendium_scriptwalk.py`). A real-bundle test checks every quest, key and target (`tests/test_compendium_quests_real.py`).
- The full build walks every script in about a minute. A new script shape that defeats the exact merge shows up as `approximate_entries` on its `script` entity, not as wrong conditions.
- Reachability per chapter follows every warp whatever its conditions, so `used` and `chapters` are upper bounds (`probable`).
- If a later version ships loops or locals in its scripts, `Script.code` rejects them (`WalkError`) and this decision needs revisiting.
