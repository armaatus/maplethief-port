"""Writes a fuzz target's seed corpus (#322, ADR 0158): make_corpus.py NAME OUT_DIR.

The seeds come from ``seeds/NAME.py``, whose ``seeds()`` returns ``{file name: bytes}``, all
built in code or read from the synthetic fixtures under ``tests/`` (never a game file). OUT_DIR
is emptied first, so a seed taken out of the module leaves the corpus too.
"""

from __future__ import annotations

import importlib.util
import shutil
import sys
from pathlib import Path
from types import ModuleType

HERE = Path(__file__).resolve().parent
SEEDS = HERE / "seeds"
# A seed name is a plain file name: letters, digits, '-', '_' and '.'.
NAME_CHARS = frozenset("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.")
# libFuzzer's -max_len unless the module sets MAX_LEN.
DEFAULT_MAX_LEN = 65536


def targets() -> list[str]:
    """Every target that has a seed module (helpers start with '_')."""
    return sorted(p.stem for p in SEEDS.glob("*.py") if not p.stem.startswith("_"))


def load(name: str) -> ModuleType:
    """seeds/NAME.py, with tests/ and seeds/ importable (pngfixtures, the _ helpers)."""
    path = SEEDS / f"{name}.py"
    if not path.is_file():
        raise SystemExit(f"make_corpus: no seed module {path}")
    for p in (str(HERE.parent), str(SEEDS)):
        if p not in sys.path:
            sys.path.insert(0, p)
    spec = importlib.util.spec_from_file_location(f"fuzz_seeds_{name}", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def max_len(name: str) -> int:
    return int(getattr(load(name), "MAX_LEN", DEFAULT_MAX_LEN))


def corpus(name: str) -> dict[str, bytes]:
    """The checked seeds of NAME: at least one, plain names, each within MAX_LEN."""
    module = load(name)
    seeds = module.seeds()
    limit = int(getattr(module, "MAX_LEN", DEFAULT_MAX_LEN))
    if not seeds:
        raise SystemExit(f"make_corpus: {name} has no seeds")
    for file, data in seeds.items():
        if not file or not set(file) <= NAME_CHARS or file.startswith("."):
            raise SystemExit(f"make_corpus: {name}: bad seed name {file!r}")
        if not isinstance(data, bytes):
            raise SystemExit(f"make_corpus: {name}: seed {file} is not bytes")
        if len(data) > limit:
            raise SystemExit(f"make_corpus: {name}: seed {file} is over MAX_LEN ({limit})")
    return seeds


def write(name: str, out: Path) -> int:
    seeds = corpus(name)
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    for file, data in seeds.items():
        (out / file).write_bytes(data)
    return len(seeds)


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print("usage: make_corpus.py NAME OUT_DIR", file=sys.stderr)
        return 2
    n = write(argv[0], Path(argv[1]))
    print(f"make_corpus: {n} seeds for {argv[0]} in {argv[1]}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
