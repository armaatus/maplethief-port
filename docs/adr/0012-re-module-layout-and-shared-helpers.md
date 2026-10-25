# 12. RE module layout and shared analysis helpers

- **Status:** Accepted
- **Date:** 2026-10-25
- **Issue:** #108

## Context

The reverse-engineering modules of epic #96 (`tools/mtport/`) grew one at a time, and each copied the low-level helpers it needed. The review of #96 found the same work done several ways:

- Thumb-2 `movw`/`movt` decoded by hand in `rtti` and `libtags`, next to the Capstone sweep `armscan` that ADR 0009 makes the second xref source.
- Four bisects over the function starts (`armscan.FunctionIndex`, `libtags.Program.owner`, `libtags.FunctionTags.tag_containing`, `rtti.Image.function_end`).
- Two "which data object holds this pointer" heuristics with different windows (`imports`: 32 bytes, `libtags`: 0x400).
- Four readers of the Ghidra export and the `work/re/*.json` outputs, about 19 ad-hoc `int(x, 16)` calls, four names-file writers, and `rtti`'s own copy of `macho.MachO.stub_symbol`.
- Two segment builders and two Thumb assemblers in the test fixtures.

Five modules had grown past 1,000 lines (`rtti` 1,315, `libtags` 1,317, `objcshell` 1,191, `imports` 1,112, `scriptout` 1,021). M2 adds tools that build on these (#24 PNG/CgBI, #28 table browser, #29 audio), and they should not copy the helpers again. Constraints: `python3 -m mtport.<name>` is the interface of `scripts/re-analyze` and the docs; the generated docs and ADR 0008 cite `tools/mtport/rtti.py` and friends by path ("the docstring of `tools/mtport/rtti.py`"); and every change must leave `work/re/` and `docs/re/` byte-identical (ADR 0009).

## Options considered

Layout of a large module:

1. **Leave the modules whole**: no churn, but 1,300-line files that mix parsing, rules, Markdown and argparse.
2. **A subpackage per analysis** (`mtport/rtti/__init__.py`, `analysis.py`, `doc.py`, `__main__.py`): tidy names, but `tools/mtport/rtti.py` stops existing, so the generated docs that cite it change, and so does every `Path(__file__).parents[2]` repo lookup.
3. **Sibling modules behind the old one**: `<name>.py` stays the public face (the module docstring that describes the method, the command line, and a re-export of every name of its parts in `__all__`); the code moves to `<name>_analysis.py` (or a few analysis modules, e.g. `libtags_program` + `libtags_rules`) and `<name>_doc.py` (the JSON, names file and Markdown renderers).

Shared helpers: keep each analysis' copy, or one implementation in the module that owns the concept.

## Decision

- **Large RE modules are split as in option 3.** `rtti` → `rtti_analysis`, `rtti_doc`; `libtags` → `libtags_program`, `libtags_rules`, `libtags_doc`; `imports` → `imports_analysis`, `imports_doc`; `objcshell` → `objcshell_analysis`, `objcshell_doc`; `scriptout` keeps its decoder and moves its opcode and native tables to `scriptout_isa`. The M2 tools merged alongside follow the same layout: `audio` → `audio_headers`, `audio_analysis`, `audio_doc`; the viewers `mapview` → `mapview_render`, `mapview_page` and `spriteview` → `spriteview_render`, `spriteview_page` (the HTML/JS templates and drivers are the page part). Callers keep importing the public face (`from mtport import rtti; rtti.analyze(...)`); a part imports only earlier parts, never the face. A module in `tools/mtport/` stays under about 800 lines. `pipeline.py` (814) stays whole: its rule keys hash its own source file (ADR 0011), so moving code into a sibling would take that code out of every key unless `rule_key` learns the parts.
- **Each low-level helper has one implementation**, and new code uses it instead of a copy:
  - `mtport.armscan`: the Capstone sweep; the raw Thumb-2 decoders `thumb_mov16`, `movw_movt_constants`, `pc_relative_values`; `FunctionIndex` (`of`, `next_start`), the one owner lookup over the function starts; `containing_object` with `DATA_OBJECT_MAX = 0x400`, the one data-object window. Every window from 31 to 0x400 bytes gives byte-identical outputs of both `imports` and `libtags`, so they share the larger one.
  - `mtport.ghidra_common`: `load_export` / `load_json` (schema check, and a missing file names the step that writes it), `dump_json`, `write_text`, `write_outputs`, `source_info`, `names_json` for names files built from tuple tables, and `render_blocks` for the `<!-- BEGIN generated:KEY -->` blocks of a hand-written doc (ADR 0009; `objcshell` and `audio`).
  - `mtport.names`: `hexaddr`; `parse_address` for every address in a generated document, `parse_hex` for hex typed on a command line.
  - `mtport.macho`: the Mach-O facts, including `stub_symbol` (from the indirect symbol table).
  - Tests: `machofixtures.segment` and `ImageBuilder` (which also writes `LC_SYMTAB`/`LC_DYSYMTAB`), and `tests/thumbasm.py`, the one Thumb assembler.

## Consequences

- M2+ tools import the helpers above; a helper that is missing goes into the module that owns the concept, not into the tool.
- A name added to a part must also be added to the face's re-export and `__all__` if callers outside should use it through the face. The faces are longer than the code they hold, which is the price of keeping every existing import working.
- The paths the generated docs cite still exist, and `scripts/re-analyze` reproduced `work/re/`, `docs/re/` and the Ghidra export byte for byte after the consolidation (#108).
- If a later analysis needs a different data-object window or owner rule, it is a parameter of the one function, not a second implementation.
