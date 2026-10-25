"""Unit tests for the asset pipeline (mtport.pipeline) on a synthetic .app tree."""

from __future__ import annotations

import json
import os
import plistlib
import struct
from pathlib import Path

import pytest

from mtport import (
    attfile,
    cgbi,
    datatables,
    formats,
    mapfile,
    mapprj,
    nib,
    objfile,
    png,
    pngdat,
    sprprj,
)
from mtport import pipeline as pl
from mtport import scriptout as so
from pngfixtures import make_cgbi, make_png, random_rgba

KEY = "Chapter1_Quest_Test.dialog.1"


def rec(table: str, /, **values):
    record = datatables.TABLES[table].record
    base = {c.name: "" if c.kind == "str" else 0 for c in datatables.layout(record)}
    base.update(values)
    return record(**base)


def palette_png(width: int, seed: int) -> bytes:
    plte = bytes([seed % 256, 0, 0, 0, seed % 256, 255])
    return pngdat.encode_indexed(pngdat.Indexed(width, 1, (bytes(width),), plte, None))


def script(key: str) -> bytes:
    """``PUSHSTRING key; RETCODE0`` as a one-function .out chunk."""
    name = b"Temp.dat\0"
    code = bytes([so.BY_NAME["PUSHSTRING"].code, 0, 0, so.BY_NAME["RETCODE0"].code])
    head = b"F" + struct.pack("<HHH", 0, len(code), 0) + struct.pack("<H", len(name)) + name
    raw = key.encode() + b"\0"
    return b"\x1b" + head + code + b"S" + struct.pack("<HH", 1, len(raw)) + raw


def nib_file() -> bytes:
    keys = ["UIBounds", "UITag", "UIParent"]
    v = nib.Value
    objs = [
        nib.NibObject(
            0,
            0,
            [
                v("UIBounds", nib.DATA, b"\x06" + struct.pack("<4f", 0, 0, 320, 480)),
                v("UITag", nib.I8, 3),
                v("UIParent", nib.OBJECT, nib.Ref(0)),
            ],
        )
    ]
    return nib.encode(nib.NibArchive(1, 9, objs, keys, [nib.NibClass("UIWindow")]))


class FakeImage:
    """Stands in for the armv7 Mach-O: the png-dat length table rows."""

    def __init__(self, rows: dict[int, list[int]]):
        self.rows = rows

    def read(self, addr, size):
        pack_id = (addr - pngdat.TABLE_ADDR) // pngdat.ROW_STRIDE
        row = list(self.rows.get(pack_id, [])) + [0] * pngdat.ROW_LEN
        return struct.pack("<255I", *row[: pngdat.ROW_LEN])


MAP = mapfile.MapFile(3, 2, ((0, 1, 2, -1, -1, 5), (-1,) * 6))
OBJ = objfile.ObjFile((objfile.Record(-1, objfile.NO_KIND, "Map00"), objfile.Record(4, 1, "east")))
SPRITE = sprprj.SpriteProject(
    (sprprj.Image("a.png"), sprprj.Image("b.png", bytes.fromhex("102030"))),
    (sprprj.Action((sprprj.Frame(3, (sprprj.Layer(0, 255, False, True, -10, 20),)),)),),
)
PIXELS = random_rgba(3, 2, seed=1, alphas=(0, 128, 255))
# The bundle check decodes audio with mtport.audio: one MPEG-1 Layer III frame (128 kbit/s,
# 44.1 kHz, 417 bytes) and a 16-bit mono PCM WAV with two samples.
MP3 = b"\xff\xfb\x90\x40" + b"\0" * 413
_WAV_BODY = (
    b"WAVE"
    + b"fmt "
    + struct.pack("<IHHIIHH", 16, 1, 1, 22050, 44100, 2, 16)
    + b"data"
    + struct.pack("<I", 4)
    + b"\0\0\0\0"
)
WAV = b"RIFF" + struct.pack("<I", len(_WAV_BODY)) + _WAV_BODY


def files() -> dict[str, bytes]:
    """Every file of the synthetic bundle: one or more of each format the pipeline handles."""
    std = make_png(2, 1, 6, 8, [bytes(range(8))])
    num = [palette_png(10, i) for i in range(14)]
    bar = [palette_png(2, 100 + i) for i in range(2)]
    strings = plistlib.dumps({KEY: "Hello there."}, fmt=plistlib.FMT_BINARY)
    return {
        "Icon.png": make_cgbi(3, 2, PIXELS),
        "Default.png": std,
        "map/00.map": mapfile.encode(MAP),
        "map/00.att": attfile.encode(attfile.AttFile(bytes([1] * 12 + [2] * 12))),
        "map/00.obj": objfile.encode(OBJ),
        "map/106.att": bytes([1, 2, 4, 1]),
        "map/0.mapprj": mapprj.encode(mapprj.MapProject(16, 16, ("a.png",))),
        "map/tile/a.png": std,
        "sprite/mob/0.sprprj": sprprj.build(SPRITE),
        "sprite/mob/res/a.png": std,
        "English/data/mob.dat": datatables.encode("mob", [rec("mob", id=1, name="Snail")]),
        "Korean/data/mob.dat": datatables.encode("mob", [rec("mob", id=1, name="달팽이")]),
        "English/data/buildup.dat": b"1",
        "script/common/x.out": script(KEY),
        "en.lproj/Scripts.strings": strings,
        "Info.plist": plistlib.dumps({"CFBundleName": "MapleThief", "Icons": ["Icon.png"]}),
        "MainWindow.nib": nib_file(),
        "png/static/num.dat": b"".join(num),
        "png/static/scrollbar.dat": b"".join(bar),
        "sound/0.mp3": MP3,
        "sound/7.wav": WAV,
        "MapleThief": b"\xca\xfe\xba\xbe not really",
        "MapleThief_armv6": b"",
        "PkgInfo": b"APPL????",
        "README.md": b"# SDK\n",
        "_CodeSignature/CodeResources": plistlib.dumps({"files": {}}),
        "ResourceRules.plist": plistlib.dumps({"rules": {}}),
    }


EXPECTED = {
    "Icon.png": ("cgbi-png", pl.CONVERTED, ["Icon.png"]),
    "Default.png": ("png", pl.COPIED, ["Default.png"]),
    "map/00.map": ("map", pl.CONVERTED, ["map/00.map.json"]),
    "map/00.att": ("att", pl.CONVERTED, ["map/00.att.json"]),
    "map/00.obj": ("obj", pl.CONVERTED, ["map/00.obj.json"]),
    "map/106.att": ("att", pl.CONVERTED, ["map/106.att.json"]),
    "map/0.mapprj": ("mapprj", pl.CONVERTED, ["map/0.mapprj.json"]),
    "map/tile/a.png": ("png", pl.COPIED, ["map/tile/a.png"]),
    "sprite/mob/0.sprprj": ("sprprj", pl.CONVERTED, ["sprite/mob/0.sprprj.json"]),
    "sprite/mob/res/a.png": ("png", pl.COPIED, ["sprite/mob/res/a.png"]),
    "English/data/mob.dat": ("data-table", pl.CONVERTED, ["English/data/mob.dat.json"]),
    "Korean/data/mob.dat": ("data-table", pl.CONVERTED, ["Korean/data/mob.dat.json"]),
    "English/data/buildup.dat": ("data-table", pl.CONVERTED, ["English/data/buildup.dat.json"]),
    "script/common/x.out": ("script-out", pl.CONVERTED, ["script/common/x.out.dis"]),
    "en.lproj/Scripts.strings": ("plist", pl.CONVERTED, ["en.lproj/Scripts.strings.json"]),
    "Info.plist": ("plist", pl.CONVERTED, ["Info.plist.json"]),
    "MainWindow.nib": ("nib", pl.CONVERTED, ["MainWindow.nib.json"]),
    "png/static/num.dat": (
        "png-dat",
        pl.CONVERTED,
        ["png/static/num/index.json"]
        + [f"png/static/num/{i:02d}.png" for i in range(14)]
        + [f"png/static/num/{i:02d}_d{d}.png" for i in range(14) for d in range(10)],
    ),
    "png/static/scrollbar.dat": (
        "png-dat",
        pl.CONVERTED,
        ["png/static/scrollbar/00.png", "png/static/scrollbar/01.png"]
        + ["png/static/scrollbar/index.json"],
    ),
    "sound/0.mp3": ("mp3", pl.COPIED, ["sound/0.mp3"]),
    "sound/7.wav": ("wav", pl.COPIED, ["sound/7.wav"]),
    "MapleThief": ("macho", pl.IGNORED, []),
    "MapleThief_armv6": ("empty", pl.IGNORED, []),
    "PkgInfo": ("pkginfo", pl.IGNORED, []),
    "README.md": ("text", pl.IGNORED, []),
    "_CodeSignature/CodeResources": ("plist", pl.IGNORED, []),
    "ResourceRules.plist": ("plist", pl.IGNORED, []),
}


def write_app(app: Path, contents: dict[str, bytes]) -> Path:
    for rel, data in contents.items():
        path = app / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    return app


@pytest.fixture
def app(tmp_path, monkeypatch):
    app = write_app(tmp_path / "src" / "MapleThief.app", files())
    rows = {}
    for rel in ("png/static/num.dat", "png/static/scrollbar.dat"):
        pack = pngdat.BY_PATH[rel]
        rows[pack.id] = [len(im.data) for im in pngdat.split((app / rel).read_bytes())]
    monkeypatch.setattr(pl, "armv7_image", lambda data: FakeImage(rows))
    return app


def snapshot(root: Path) -> dict[str, tuple[bytes, int]]:
    return {
        p.relative_to(root).as_posix(): (p.read_bytes(), p.stat().st_mtime_ns)
        for p in sorted(root.rglob("*"))
        if p.is_file()
    }


def manifest(out: Path) -> dict:
    return json.loads((out / pl.MANIFEST).read_text(encoding="utf-8"))


def read_json(out: Path, rel: str) -> dict:
    return json.loads((out / rel).read_text(encoding="utf-8"))


# --- outcomes and outputs ---------------------------------------------------------------------


def test_every_file_gets_exactly_one_outcome(app, tmp_path):
    out = tmp_path / "out"
    result = pl.run(app, out)
    assert result.ok, result.plan.errors
    m = manifest(out)
    assert sorted(m["files"]) == sorted(EXPECTED)
    for rel, (fmt, outcome, outputs) in EXPECTED.items():
        entry = m["files"][rel]
        assert (entry["format"], entry["outcome"]) == (fmt, outcome), rel
        assert sorted(entry["outputs"]) == sorted(outputs), rel
        assert entry["sha256"] == pl._sha((app / rel).read_bytes())
        assert entry["size"] == (app / rel).stat().st_size
        if outcome != pl.CONVERTED:
            assert entry["reason"], rel
        for o, digest in entry["outputs"].items():
            assert pl._sha((out / o).read_bytes()) == digest, o
    on_disk = {p.relative_to(out).as_posix() for p in out.rglob("*") if p.is_file()}
    assert on_disk == {o for _, _, outs in EXPECTED.values() for o in outs} | {pl.MANIFEST}
    assert m["outcomes"] == {"converted": 16, "copied": 5, "error": 0, "ignored": 6}
    assert result.summary() == (
        "files=27 converted=16 copied=5 ignored=6 skipped(unchanged)=0 errors=0 removed=0"
    )
    assert set(m["pipeline"]["rules"]) == {*pl.RULES, "code-signing"}


def test_copies_are_byte_identical_and_cgbi_becomes_a_standard_png(app, tmp_path):
    out = tmp_path / "out"
    pl.run(app, out)
    for rel in ("Default.png", "map/tile/a.png", "sound/0.mp3", "sound/7.wav"):
        assert (out / rel).read_bytes() == (app / rel).read_bytes()
    icon = (out / "Icon.png").read_bytes()
    assert not cgbi.is_cgbi(icon)
    img = png.decode(icon)
    assert (img.width, img.height) == (3, 2)
    assert img.rgba == cgbi.decode((app / "Icon.png").read_bytes()).rgba


def test_map_att_obj_json(app, tmp_path):
    out = tmp_path / "out"
    pl.run(app, out)
    m = read_json(out, "map/00.map.json")
    assert m["source"] == "map/00.map"
    assert (m["width"], m["height"], m["empty"]) == (3, 2, -1)
    assert m["layers"][0] == [[0, 1, 2], [-1, -1, 5]]
    att = read_json(out, "map/00.att.json")
    assert (att["grid_width"], att["grid_height"], att["packed"]) == (6, 4, True)
    assert att["rows"][0] == [1] * 6 and att["rows"][3] == [2] * 6
    assert att["flags"]["LADDER"] == attfile.LADDER
    raw = read_json(out, "map/106.att.json")
    assert (raw["packed"], raw["grid_width"], raw["values"]) == (False, None, [1, 2, 4, 1])
    obj = read_json(out, "map/00.obj.json")
    assert obj["map_script"] == "Map00"
    script_rec, portal = obj["records"]
    assert "tile" not in script_rec and script_rec["category"] == "map_script"
    assert (portal["tile"], portal["position"], portal["category"]) == ([1, 1], [24, 31], "portal")
    prj = read_json(out, "map/0.mapprj.json")
    assert prj["tileset_paths"] == ["map/tile/a.png"]


def test_sprite_table_script_strings_nib_json(app, tmp_path):
    out = tmp_path / "out"
    pl.run(app, out)
    spr = read_json(out, "sprite/mob/0.sprprj.json")
    assert [i["file"] for i in spr["images"]] == ["sprite/mob/res/a.png", "sprite/mob/res/b.png"]
    assert spr["images"][1]["palette"] == "102030" and spr["images"][0]["palette"] is None
    assert spr["actions"][0]["frames"][0]["layers"][0]["x"] == -10
    ko = (out / "Korean/data/mob.dat.json").read_text(encoding="utf-8")
    assert '"name": "달팽이"' in ko  # UTF-8, not \u escapes
    table = json.loads(ko)
    assert table["records"][0]["id"] == 1
    assert [c["name"] for c in table["columns"]][:2] == ["id", "name"]
    assert read_json(out, "English/data/buildup.dat.json")["buildup"] == 1
    dis = (out / "script/common/x.out.dis").read_text(encoding="utf-8")
    assert f'PUSHSTRING    "{KEY}"  ; "Hello there."' in dis
    assert read_json(out, "en.lproj/Scripts.strings.json")["strings"] == {KEY: "Hello there."}
    assert read_json(out, "Info.plist.json")["plist"]["Icons"] == ["Icon.png"]
    n = read_json(out, "MainWindow.nib.json")
    (obj,) = n["objects"]
    assert obj["class"] == "UIWindow"
    bounds, tag, parent = obj["values"]
    assert bounds["floats"] == [0.0, 0.0, 320.0, 480.0] and bounds["type"] == "data"
    assert (tag["type"], tag["value"], parent["value"]) == ("i8", 3, {"ref": 0})


def test_packs_are_split_and_checked_against_the_length_table(app, tmp_path):
    out = tmp_path / "out"
    pl.run(app, out)
    data = (app / "png/static/num.dat").read_bytes()
    images = pngdat.split(data)
    for im in images:
        assert (out / f"png/static/num/{im.index:02d}.png").read_bytes() == im.data
    index = read_json(out, "png/static/num/index.json")
    assert index["length_table_checked"] is True
    assert index["path"] == "png/static/num.dat" and len(index["images"]) == 14
    assert index["images"][0]["parts"]["d3"]["rect"] == [3, 0, 1, 1]


def test_pack_that_disagrees_with_the_length_table_fails(app, tmp_path, monkeypatch):
    monkeypatch.setattr(pl, "armv7_image", lambda data: FakeImage({4: [1, 2, 3]}))
    result = pl.run(app, tmp_path / "out")
    assert [e.split(":")[0] for e in result.plan.errors] == [
        "png/static/num.dat",
        "png/static/scrollbar.dat",
    ]


def test_without_the_binary_packs_are_split_unchecked(app, tmp_path):
    (app / "MapleThief").unlink()
    out = tmp_path / "out"
    assert pl.run(app, out).ok
    assert read_json(out, "png/static/num/index.json")["length_table_checked"] is False
    assert manifest(out)["files"]["png/static/num.dat"]["deps"] == {}


# --- determinism and incremental runs ---------------------------------------------------------


def test_two_fresh_runs_are_byte_identical(app, tmp_path):
    a, b = tmp_path / "a", tmp_path / "b"
    pl.run(app, a)
    pl.run(app, b)
    assert {k: v[0] for k, v in snapshot(a).items()} == {k: v[0] for k, v in snapshot(b).items()}


def test_second_run_is_a_no_op(app, tmp_path):
    out = tmp_path / "out"
    pl.run(app, out)
    before = snapshot(out)
    result = pl.run(app, out)
    assert snapshot(out) == before  # same bytes, nothing rewritten (mtimes unchanged)
    assert result.written == [] and result.stale == []
    assert result.summary() == (
        "files=27 converted=0 copied=0 ignored=0 skipped(unchanged)=27 errors=0 removed=0"
    )


def test_changed_input_reconverts_it_and_its_dependents(app, tmp_path):
    out = tmp_path / "out"
    pl.run(app, out)
    bigger = mapfile.MapFile(4, 2, ((7,) * 8,))
    (app / "map/00.map").write_bytes(mapfile.encode(bigger))
    result = pl.run(app, out)
    # The .att and .obj read the .map's width: they are redone too (the .att now fails).
    assert result.plan.counts[pl.CONVERTED] == 2 and result.plan.counts["skipped"] == 24
    assert result.plan.errors == ["map/00.att: map/00.att: 24 cells, the 4x2 map needs 8x4"]
    assert read_json(out, "map/00.map.json")["layers"][0][0] == [7, 7, 7, 7]
    assert not (out / "map/00.att.json").exists()  # stale output of a failed input is removed
    assert result.stale == ["map/00.att.json"]


def test_removed_input_removes_its_outputs(app, tmp_path):
    out = tmp_path / "out"
    pl.run(app, out)
    (app / "sprite/mob/0.sprprj").unlink()
    (app / "png/static/scrollbar.dat").unlink()
    result = pl.run(app, out)
    assert result.ok
    assert "sprite/mob/0.sprprj" not in manifest(out)["files"]
    assert not (out / "sprite/mob/0.sprprj.json").exists()
    assert not (out / "png/static/scrollbar").exists()  # emptied directories are pruned
    assert (out / "sprite/mob/res/a.png").is_file()
    assert len(result.stale) == 4 and result.plan.counts["skipped"] == 25


def test_missing_or_edited_output_is_rewritten(app, tmp_path):
    out = tmp_path / "out"
    pl.run(app, out)
    good = (out / "map/00.map.json").read_bytes()
    (out / "map/00.map.json").write_bytes(b"{}")
    (out / "sound/0.mp3").unlink()
    result = pl.run(app, out)
    assert result.plan.counts[pl.CONVERTED] == 1 and result.plan.counts[pl.COPIED] == 1
    assert (out / "map/00.map.json").read_bytes() == good
    assert (out / "sound/0.mp3").is_file()


def test_code_change_invalidates_that_rules_outputs(app, tmp_path, monkeypatch):
    out = tmp_path / "out"
    pl.run(app, out)
    monkeypatch.setitem(pl._KEYS, "map", "map:changed")
    result = pl.run(app, out)
    assert result.plan.counts[pl.CONVERTED] == 1 and result.plan.counts["skipped"] == 26
    assert manifest(out)["pipeline"]["rules"]["map"] == "map:changed"


def test_rule_key_hashes_the_parser_source(tmp_path, monkeypatch):
    mod_dir = tmp_path / "mods"
    mod_dir.mkdir()
    (mod_dir / "fakeparser.py").write_text("X = 1\n", encoding="utf-8")
    monkeypatch.syspath_prepend(str(mod_dir))
    import fakeparser

    rule = pl.Rule("fake", pl.CONVERTED, modules=(fakeparser,))
    monkeypatch.setattr(pl, "_KEYS", {})
    first = pl.rule_key(rule)
    (mod_dir / "fakeparser.py").write_text("X = 2\n", encoding="utf-8")
    monkeypatch.setattr(pl, "_KEYS", {})
    assert pl.rule_key(rule) != first and first.startswith("fake:")
    # Imported mtport modules count too: formats pulls in every parser.
    files = {m.__name__ for m in pl._mtport_modules(formats)}
    assert {"mtport.mapfile", "mtport.bytereader", "mtport.png"} <= files


def test_manifest_from_another_schema_is_ignored(app, tmp_path):
    out = tmp_path / "out"
    pl.run(app, out)
    m = manifest(out)
    m["pipeline"]["schema"] = 0
    (out / pl.MANIFEST).write_text(json.dumps(m), encoding="utf-8")
    result = pl.run(app, out)
    assert result.plan.counts["skipped"] == 0 and result.written == [pl.MANIFEST]


# --- errors ------------------------------------------------------------------------------------


def test_errors_are_listed_and_the_rest_still_converts(app, tmp_path, capsys):
    out = tmp_path / "out"
    pl.run(app, out)
    (app / "sprite/mob/0.sprprj").write_bytes(b"\x05")
    (app / "junk").mkdir()
    (app / "junk/new.bin").write_bytes(b"?")
    (app / "Default.png").write_bytes(b"\x89PNG\r\n\x1a\nbroken")
    status = pl.main(["run", "--app", str(app), "--out", str(out)])
    captured = capsys.readouterr()
    assert status == 1
    errors = [line for line in captured.err.splitlines() if line.startswith("error: ")]
    assert [e.split(":")[1].strip() for e in errors] == [
        "Default.png",
        "junk/new.bin",
        "sprite/mob/0.sprprj",
    ]
    assert "junk/new.bin: unclassified: no format in mtport.formats matches it" in captured.err
    assert "errors=3" in captured.out and "removed=2" in captured.out
    files = manifest(out)["files"]
    assert files["junk/new.bin"]["outcome"] == pl.ERROR and files["junk/new.bin"]["format"] is None
    assert files["sprite/mob/0.sprprj"]["reason"]
    assert files["sprite/mob/0.sprprj"]["outputs"] == {}
    assert manifest(out)["outcomes"]["error"] == 3
    assert not (out / "sprite/mob/0.sprprj.json").exists()
    # A failed file is retried on the next run, not skipped.
    assert pl.run(app, out).plan.counts["skipped"] == 25


def test_every_format_has_a_rule():
    assert set(formats.FORMATS) <= set(pl.RULES)
    for rule in (*pl.RULES.values(), *pl.OVERRIDES.values()):
        assert rule.outcome in (pl.CONVERTED, pl.COPIED, pl.IGNORED)
        assert (rule.convert is None) == (rule.outcome == pl.IGNORED)
        assert rule.reason or rule.outcome == pl.CONVERTED, rule.name


def test_a_format_without_a_rule_is_an_error(app, tmp_path, monkeypatch):
    rules = dict(pl.RULES)
    del rules["wav"]
    monkeypatch.setattr(pl, "RULES", rules)
    result = pl.run(app, tmp_path / "out")
    assert result.plan.errors == [
        "sound/7.wav: format 'wav' has no pipeline rule (mtport.pipeline.RULES)"
    ]


def test_output_clashes_fail_the_later_source(app, tmp_path):
    write_app(app, {"png/static/scrollbar/00.png": make_png(1, 1, 0, 8, [b"\0"])})
    result = pl.run(app, tmp_path / "out")
    assert len(result.plan.errors) == 1
    assert result.plan.errors[0].startswith("png/static/scrollbar/00.png: output ")
    assert "of png/static/scrollbar.dat" in result.plan.errors[0]


def test_symlinks_in_the_bundle_are_not_followed(app, tmp_path):
    (app / "link.png").symlink_to(app / "Default.png")
    result = pl.run(app, tmp_path / "out")
    assert result.plan.errors == ["link.png: not a regular file (symlinks are not followed)"]


# --- assets-out/ ownership ---------------------------------------------------------------------


def test_viewers_are_never_touched(app, tmp_path):
    out = tmp_path / "out"
    viewer = out / "viewers" / "tables" / "index.html"
    viewer.parent.mkdir(parents=True)
    viewer.write_text("page", encoding="utf-8")
    pl.run(app, out)
    # A manifest that claims a viewer file (or a path outside out/) must not get it deleted.
    m = manifest(out)
    m["files"]["gone.map"] = {"outputs": {"viewers/tables/index.html": "x", "../escape": "y"}}
    (out / pl.MANIFEST).write_text(json.dumps(m), encoding="utf-8")
    (tmp_path / "escape").write_text("keep", encoding="utf-8")
    pl.run(app, out)
    assert (
        viewer.read_text(encoding="utf-8") == "page"
        and (tmp_path / "escape").read_text(encoding="utf-8") == "keep"
    )
    # And a bundle file under viewers/ is refused instead of written there.
    write_app(app, {"viewers/x.png": (app / "Default.png").read_bytes()})
    result = pl.run(app, out)
    assert result.plan.errors == [
        "viewers/x.png: output path 'viewers/x.png' is outside the pipeline's part of assets-out/"
    ]


@pytest.mark.parametrize("rel", ["", "/abs", "a/../b", "viewers/x", "manifest.json", "a\\b"])
def test_check_output_path_rejects(rel):
    with pytest.raises(pl.PipelineError):
        pl.check_output_path(rel)


def test_output_inside_the_bundle_is_refused(app):
    with pytest.raises(pl.PipelineError, match="must not overlap"):
        pl.run(app, app / "out")
    with pytest.raises(pl.PipelineError, match="no such directory"):
        pl.run(app / "missing", app.parent / "out")


# --- check command and CLI ---------------------------------------------------------------------


def test_check_is_a_dry_run(app, tmp_path, capsys):
    out = tmp_path / "out"
    args = ["--app", str(app), "--out", str(out)]
    assert pl.main(["check", *args]) == 1
    assert not out.exists()
    assert "out of date: map/00.map.json" in capsys.readouterr().out
    assert pl.main(args) == 0  # run is the default command
    capsys.readouterr()
    assert pl.main(["check", *args]) == 0
    assert "would write 0 files" in capsys.readouterr().out
    (app / "README.md").unlink()
    before = snapshot(out)
    assert pl.main(["check", *args]) == 1
    assert snapshot(out) == before


def test_cli_summary_line(app, tmp_path, capsys):
    assert pl.main(["run", "--app", str(app), "--out", str(tmp_path / "out")]) == 0
    line = capsys.readouterr().out.strip()
    assert line.startswith(
        "files=27 converted=16 copied=5 ignored=6 skipped(unchanged)=0 errors=0 removed=0 (wrote "
    )


def test_cli_missing_app(tmp_path, capsys):
    assert pl.main(["--app", str(tmp_path / "nope"), "--out", str(tmp_path / "out")]) == 1
    assert "no such directory" in capsys.readouterr().err


# --- JSON ----------------------------------------------------------------------------------------


def test_dumps_is_deterministic_json():
    value = {"b": [1, 2], "a": {"z": b"\x01\xff", "y": [{"k": None}]}, "c": "한"}
    text = pl.dumps(value).decode("utf-8")
    assert text == (
        '{\n "a": {\n  "y": [\n   {\n    "k": null\n   }\n  ],\n  "z": "01ff"\n },\n'
        ' "b": [1, 2],\n "c": "한"\n}\n'
    )
    assert json.loads(text)["a"]["z"] == "01ff"
    assert pl.dumps([]) == b"[]\n" and pl.dumps({}) == b"{}\n"
    with pytest.raises(ValueError):
        pl.dumps({"x": float("nan")})
    with pytest.raises(TypeError):
        pl.dumps({"x": object()})


def test_writes_are_atomic_and_skip_identical_bytes(tmp_path):
    path = tmp_path / "a" / "b.json"
    assert pl._write(path, b"1") is True
    mtime = path.stat().st_mtime_ns
    assert pl._write(path, b"1") is False and path.stat().st_mtime_ns == mtime
    assert pl._write(path, b"2") is True and path.read_bytes() == b"2"
    assert sorted(os.listdir(path.parent)) == ["b.json"]  # no temp file left behind
