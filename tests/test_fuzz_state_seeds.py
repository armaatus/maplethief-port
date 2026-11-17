"""#323's fuzz targets (ADR 0159): seeds/state.py names every section of a state file and every
host state serializer, as the runtime registers them, and #323's parsers all have targets."""

from __future__ import annotations

import importlib.util
import re

from repo_paths import REPO

SRC = REPO / "runtime" / "src"
FUZZ = REPO / "tests" / "fuzz"


def state_seeds():
    spec = importlib.util.spec_from_file_location("fuzz_seeds_state", FUZZ / "seeds" / "state.py")
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def runtime_text() -> str:
    return "\n".join(p.read_text(encoding="utf-8") for p in sorted(SRC.rglob("*.cpp")))


def test_the_state_seeds_name_every_section():
    text = (SRC / "snapshot" / "snapshot.cpp").read_text(encoding="utf-8")
    core = re.findall(r'\badd\("([a-z.]+)"', text)
    modules = re.findall(r'const mt_snap::Module k_snap_\w+ = \{"([a-z.]+)"', runtime_text())
    assert len(core) >= 9 and len(modules) >= 18
    assert sorted(state_seeds().SECTIONS) == sorted(core + modules)


def test_the_state_seeds_name_every_serializer():
    names = re.findall(
        r'const mt_snap::StateOps k_snap_\w+ = \{\s*&\w+,\s*"([^"]+)"', runtime_text()
    )
    assert len(names) >= 39
    assert sorted(state_seeds().SERIALIZERS) == sorted(names)


def test_323s_parsers_all_have_targets():
    # ADR 0154's second group: NIBArchive, binary plists, JSON, the saves, the state file.
    text = (FUZZ / "CMakeLists.txt").read_text(encoding="utf-8")
    registered = set(re.findall(r"^maplethief_add_fuzz_target\((\w+)", text, re.MULTILINE))
    assert {"nib", "plist", "json", "saves", "state"} <= registered
