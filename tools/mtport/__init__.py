"""MapleThief port tooling (stdlib-only Python 3.10+).

Modules:
    fat        Portable fat (universal) Mach-O parser and slice splitter.
    bootstrap  Verifies and extracts the IPA into work/ipa/, splits the main binary into
               work/slices/ and writes work/bootstrap.json. Run through scripts/bootstrap.
"""

__version__ = "0.1.0"
