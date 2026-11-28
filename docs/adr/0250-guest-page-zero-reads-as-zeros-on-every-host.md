# 250. Guest page zero reads as zeros on every host

- **Status:** Accepted (delegated by Joris, 2026-11-28)
- **Date:** 2026-11-28
- **Issue:** #441 (epic #292)
- **Revises:** [ADR 0025](0025-guest-memory-and-address-space.md) (its rule that page zero is never mapped on a 4 KiB host)

## Context

ADR 0025 maps the image at host-page granularity. On a 16 KiB host (macOS and iOS on Apple silicon) the page 0x0–0x3FFF holds page zero and the start of `__TEXT`, so a null read returns zero bytes and a null write faults. On a 4 KiB host (Linux, Windows, the Switch) page zero was never mapped, so a null read faulted too. The game's behaviour depended on the host's page size.

The real-asset suite in the arm64 Linux container (#441, first run for #362) showed where that matters. Five play tests crashed with SIGSEGV on Linux and pass on macOS. Each is a guest read of a null pointer that returns zero on macOS:

- `play.autosave_map_6_2`, `play.systems2_portal620_synthetic` and `play.systems2_portal620_fixed_synthetic` enter 6.2 in a chapter that doesn't ship the script of its NPC `remem2` (it exists only in `script/chapter3/`). The script loader (`0xA2C1`) then returns a null buffer and a length of 0, and the NPC-state pass (`0x839BD`) hands both to the chunk loader (`0x9D0A9`). Its first step (`0x98EE1`) reads the buffer's first byte, to tell a precompiled chunk from source, from guest address 0. On macOS that is zero, the chunk is empty, and the game carries on.
- `play.map_page_3_8_3_synthetic`: from the map-page portal start on 3.8, `0x7B151` (under the hooked `0x9B37D`) reads a field at +0x24 of a null object.
- `play.host_events`: one of its teardown steps (ADR 0242) calls a destructor, `0x97455`, on a zeroed object made by the host, and the destructor follows a null member pointer.

Windows, with 4 KiB pages too, would fault the same way.

The original on a 4 KiB-page iPhone would fault in these places too. But the port reaches them in states the original never shows (synthetic starts, the restored maps of ADR 0238, the portal620 fix, host-driven calls), and every behaviour the tests check was established on macOS.

## Decision

Guest page zero (0x0–0xFFF) is committed and sealed read-only on every host. `mt_mem_seal_image` commits it when no segment shares its host page, which happens on a 4 KiB host, and gives it read access. A null read in generated code returns zero bytes on every host. A null write still faults in hardware, except on the Switch, where sealing is a no-op (ADR 0025's Horizon addendum). HLE accessors (`mt_g2h`, `mt_g2h_str` and the runtime's other checks against `MT_GUEST_PAGE_ZERO_END`) still refuse page zero and trap with the guest caller, and the crash reporter still describes an address there as "page zero (a null pointer)".

## Alternatives

1. **Fault on every host**: leave page zero unmapped on a 16 KiB host as well. That needs a host page of its own below `__TEXT`, which the fixed layout doesn't have on a 16 KiB host. It would also turn the five tests' maps into crashes on macOS and iOS for players. Rejected.
2. **Fix the one call site**: hook the NPC-state pass, or the script loader, to skip a missing script. That fixes the five tests, but any other null read the game makes would still depend on the host's page size. A rule that holds for the whole address space fixes the whole class. Rejected as the only fix. A hook can still come later if a missing script should also skip the NPC's state.

## Consequences

- Linux, Windows and macOS run the same guest behaviour for null reads. The five tests pass in the arm64 container, and still pass on macOS, where nothing changed.
- A null read in generated code no longer stops the guest on a 4 KiB host. It returns zero, as it already did on macOS. A null write still stops it with the crash report.
- `memory_test`'s "sealing makes __TEXT read-only and keeps __DATA writable" checks page zero as readable zeros and not writable on every host page size.
