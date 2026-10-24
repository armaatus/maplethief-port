"""MapleThief port tooling (stdlib-only Python 3.10+).

Modules:
    fat        Portable fat (universal) Mach-O parser and slice splitter.
    bootstrap  Splits the extracted main binary into work/slices/ and writes
               work/bootstrap.json. Normally run through scripts/bootstrap.
"""

__version__ = "0.1.0"
