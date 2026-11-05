"""mtport.hudextras (#191): the names file is valid, names nothing another names generator names,
and the hero.dat facts the port's EXP figure reads agree with platform/src/exp_percent.h."""

import json
import re
import subprocess
import sys

from mtport import audio_analysis, autosave, flashes, flow, network, savedata, screenshake, timing
from mtport import hudextras as he
from mtport import names as nm
from repo_paths import REPO

HEADER = REPO / "platform" / "src" / "exp_percent.h"


def test_names_file_is_valid():
    source, entries = nm.parse(json.loads(he.names_json()), "hudextras.json")
    assert source == "hudextras"
    assert len(entries) == len(he.NAMES)
    assert len({e.address for e in entries}) == len(entries)
    assert len({e.name for e in entries}) == len(entries)
    assert all(e.thumb for e in entries if e.kind == "function")


def test_names_do_not_collide_with_the_other_generators():
    ours = {int(a, 16) & ~1 for a, _, _, _ in he.NAMES}
    names = {n for _, _, n, _ in he.NAMES}
    for module in (savedata, timing, autosave, network, screenshake, audio_analysis, flashes):
        rows = module.NAMES
        assert not ours & {int(a, 16) & ~1 for a, _, _, _ in rows}, module.__name__
        assert not names & {n for _, _, n, _ in rows}, module.__name__
    assert not ours & {a & ~1 for a, _, _ in flow.FLOW_NAMES}
    assert not names & {n for _, n, _ in flow.FLOW_NAMES}


def test_the_figure_reads_the_documented_fields():
    """exp_percent.h reads hero.dat's buffer (docs/formats/data-tables.md) and CHero's fields."""
    text = HEADER.read_text(encoding="utf-8")
    fields = dict(re.findall(r"inline constexpr std::uint32_t (kExp\w+) = (0x[0-9A-F]+);", text))
    assert int(fields["kExpHeroTable"], 16) == 0x11A9A8
    assert int(fields["kExpHeroLevel"], 16) == 0x80
    assert int(fields["kExpHeroExp"], 16) == 0x90
    from mtport import datatables

    assert datatables.TABLES["hero"].pointer == 0x11A9A8
    # The EXP column is the i32 right after the u8 key (CDataMgr::ReadI32's offset 1).
    columns = datatables.layout(datatables.HeroLevel)
    assert [(c.name, c.kind) for c in columns[:2]] == [("level", "u8"), ("exp", "i32")]


def test_cli_writes_the_names_file(tmp_path):
    out = tmp_path / "hudextras.json"
    subprocess.run(
        [sys.executable, "-m", "mtport.hudextras", "names", str(out)],
        check=True,
        capture_output=True,
        env={"PYTHONPATH": str(REPO / "tools")},
    )
    assert out.read_text(encoding="utf-8") == he.names_json()
