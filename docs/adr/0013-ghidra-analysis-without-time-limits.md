# 13. Ghidra auto-analysis without time limits

- **Status:** Accepted
- **Date:** 2026-10-25
- **Issue:** #112

## Context

[ADR 0007](0007-re-tooling-ghidra.md) promises a deterministic Ghidra export, and two clean runs on one Mac are byte-identical. A run on Windows 10 with the same Ghidra 12.2, JDK, slice and scripts was not ([#112](https://github.com/armaatus/maplethief-port/issues/112)). `LC_FUNCTION_STARTS` matched the same, but less code was decoded around `0x3F102`–`0x40EE0`. As a result `docs/re/libraries.md` showed 623,134 engine body bytes instead of 628,356, `docs/re/imports.md` showed 327 of 333 imports agreeing with Capstone instead of 333 of 333, and three real-asset tests failed there.

What was measured on an Apple M4 (macOS, 2026-10-25):

- The missing code is in one function, `CDataMgr::OnServerPacket` (0x3ED18, 29,427 bytes), the packet dispatcher with nested `tbb`/`tbh` switches. *Decompiler Switch Analysis* decompiles a function again after each round of newly recovered switch targets. One decompile of this function takes about 17 s.
- Ghidra's analyzers run each decompile with a **timeout of 60 s** by default. These are *Decompiler Switch Analysis* and *Call Convention ID* (`Analysis Decompiler Timeout (sec)`), and *Decompiler Parameter ID*, which is off here. A decompile that runs out of time is dropped **without a message** (the analysis log stays empty), and the switch cases it would have found never get disassembled.
- With the switch timeout set to 15 s on the Mac, only `OnServerPacket` changes (−5,222 bytes). The generated docs then show the Windows numbers exactly: 623,134 bytes, 327 of 333, with the same only-Capstone call sites (`0x3FC02`, `0x40502`, `0x4604E`, …). With 5 s, `CStateGame::Paint` (0x87500) and `CStateGame::OnKeyPressed` (0x8D4C8) lose code too.
- With Ghidra's default timeouts, a run on the M4's efficiency cores (`taskpolicy -c background`, about 2.7× slower) loses the code of all three functions (86,180 xrefs instead of 87,654). The analysis result depends on how fast the host is.
- The *Demangler GNU* analyzer has a per-name timeout of 3 s (`Timeout (seconds)`). On a slow host, a timed-out name would stay mangled. Unlike the decompiler, this option has no "no timeout" value: 0 times out at once.
- No other analysis option is time-based. The `Max Threads` options are fixed numbers, and the remaining limits are counts and sizes, which are the same on every host.

## Options considered

1. **Raise the decompiler timeouts** (e.g. to 600 s). This works on today's hosts, but it is still a limit that a slow enough machine (a CI runner, a VM, a Steam Deck) can hit, and a hit stays silent.
2. **No decompiler timeouts** (`0`, which `DecompileProcess.getTimeoutMs` turns into "never"). The result no longer depends on speed. If a decompile never ended, the import would hang instead of finishing with silently different output, and a hang is easy to see.
3. **Leave Ghidra as is and tolerate the difference**: regenerate committed docs only on macOS, and skip the "committed doc is current" tests elsewhere. M3 tools that read the export would still give different answers on different hosts.

## Decision

Option 2. Every time limit of the auto-analysis is removed. The non-default analysis options now live in `mtport.ghidra_common.ANALYSIS_OPTIONS`, and `tools/ghidra/driver.py` sets them before seeding, as before:

| Option | Ghidra default | Ours |
|---|---|---|
| `Objective-C Message Analyzer` | on | off (ADR 0007) |
| `Decompiler Switch Analysis.Analysis Decompiler Timeout (sec)` | 60 | 0 (none) |
| `Call Convention ID.Analysis Decompiler Timeout (sec)` | 60 | 0 (none) |
| `Demangler GNU.Timeout (seconds)` | 3 | 86,400 (one day: the option has no "none") |

The driver sets each option with the type Ghidra registered for it (boolean, int or long). It stops with an error if Ghidra doesn't know the name, so an option renamed in a future Ghidra can't be ignored silently. A CI test (`tests/test_ghidra_common.py::test_analysis_options_have_no_time_limits`) checks that every timeout option in the table means "no limit". If an analyzer with a decompiler timeout is turned on later (e.g. *Decompiler Parameter ID*), its timeout goes into the table as 0.

This amends ADR 0007's pipeline: ObjC off is no longer the only non-default analysis option.

## Consequences

- The export no longer depends on the host's speed. On the Mac the default run is unchanged and byte-identical to the committed numbers, and so is a run on the efficiency cores, which takes about 4.5 min instead of 1 min. On Windows it should give the macOS export. That check needs Joris's Windows machine (#112).
- `scripts/re-analyze` gives the committed `docs/re/` on any host, so the README no longer says to regenerate them only on macOS.
- An import on a slow machine takes as long as it needs. If a future Ghidra or binary makes a decompile run away, the import hangs and nothing is written. The fix is then to find that function, not to bring back a timeout.
- Other host differences (the natives built from source on macOS versus prebuilt on Windows, a different core count) are not ruled out by this ADR. They are ruled out by measurement: a run on the efficiency cores with the JVM limited to 2 CPUs (`-XX:ActiveProcessorCount=2`) was byte-identical as well. A Windows run that still differs would be a new finding for `docs/ANALYSIS.md`.
