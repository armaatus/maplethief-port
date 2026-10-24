"""Split and extract every shipped png/**/*.dat pack (docs/formats/png-dat.md -> Validation).

Skipped when work/ipa/ is absent (CI); run scripts/bootstrap first.
"""

import json
from collections import Counter

import pytest

from mtport import macho, pngdat
from repo_paths import IPA_DIR, REPO

pytestmark = pytest.mark.real_assets

APP = IPA_DIR / "Payload" / "MapleThief.app"
BINARY = REPO / "work" / "slices" / "MapleThief.armv7"


@pytest.fixture(scope="module")
def image():
    if not BINARY.is_file():
        pytest.skip("work/slices/MapleThief.armv7 absent (run scripts/bootstrap)")
    return macho.load(BINARY)


def test_the_40_shipped_packs_are_the_pack_list():
    shipped = sorted(str(p.relative_to(APP)) for p in (APP / "png").rglob("*.dat"))
    assert shipped == sorted(pngdat.BY_PATH)


def test_every_pack_splits_into_the_loaders_image_count():
    for pack in pngdat.PACKS:
        images = pngdat.split((APP / pack.path).read_bytes(), pack.path)
        assert len(images) == pack.count, pack.path


def test_length_table_matches_the_png_framing(image):
    for pack in pngdat.PACKS:
        data = (APP / pack.path).read_bytes()
        lengths = pngdat.length_row(image, pack.id)
        assert list(lengths) == [im.length for im in pngdat.split(data)], pack.path
        assert sum(lengths) == len(data)


def test_loaded_packs_have_a_path_string_and_the_rest_do_not(image):
    binary = BINARY.read_bytes()
    for pack in pngdat.PACKS:
        present = b"\0/" + pack.path.encode() + b"\0" in binary
        assert present == (pack.loader is not None), pack.path


def test_every_sub_image_decodes_and_digit_strips_divide_by_ten():
    depths = Counter()
    for pack in pngdat.PACKS:
        for im in pngdat.split((APP / pack.path).read_bytes(), pack.path):
            img = pngdat.decode_indexed(im.data, f"{pack.path}[{im.index}]")
            depths[im.info.bit_depth] += 1
            if pack.parts == pngdat.PARTS_DIGITS:
                assert img.width % 10 == 0
            if pack.parts == pngdat.PARTS_CORNERS:
                assert img.width == img.height
    assert depths == {8: 465, 4: 8}  # all palette; 4-bit: static/arrow.dat 0, 1, 8-11, 18, 19


def test_extract_writes_all_sub_images_without_errors(tmp_path, image, capsys):
    out = tmp_path / "png-dat"
    status = pngdat.main(["extract", "--app", str(APP), "--out", str(out), "--binary", str(BINARY)])
    summary = capsys.readouterr().out
    assert status == 0, summary
    assert (
        "40/40 packs, 473 sub-images, 336 parts, 40 checked against the length table, 0 errors"
        in (summary)
    )
    manifest = json.loads((out / "index.json").read_text())
    assert len(manifest["packs"]) == 40
    written = {p.relative_to(out) for p in out.rglob("*.png")}
    assert len(written) == 473 + 336
    for entry in manifest["packs"]:
        folder = out / entry["path"][len("png/") : -len(".dat")]
        for im in entry["images"]:
            assert (folder / im["file"]).is_file()


def test_call_sites_and_table_refs_in_the_code(image):
    """The pack list's addresses, checked against the armv7 code itself (no Ghidra needed)."""
    capstone = pytest.importorskip("capstone")
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB)
    for pack in pngdat.PACKS:
        if pack.call is None:
            continue
        (ins,) = md.disasm(image.read(pack.call, 4), pack.call, 1)
        assert (ins.mnemonic, ins.op_str) == ("bl", f"#0x{pack.helper & ~1:x}"), pack.path
    for helper, adds in pngdat.TABLE_REFS.items():
        for add in adds:
            # ldr rN, [pc, #imm] immediately before add rN, pc
            ldr, addpc = md.disasm(image.read(add - 2, 4), add - 2, 2)
            reg = addpc.op_str.split(",")[0]
            assert (addpc.mnemonic, addpc.op_str) == ("add", f"{reg}, pc"), hex(add)
            assert ldr.mnemonic == "ldr" and ldr.op_str.startswith(f"{reg}, [pc, #")
            literal = ((ldr.address + 4) & ~3) + int(ldr.op_str.split("#")[1].rstrip("]"), 16)
            assert (image.u32(literal) + add + 4) & 0xFFFFFFFF == pngdat.TABLE_ADDR, hex(helper)


CALLS = REPO / "work" / "re" / "pngdat_calls.txt"


@pytest.mark.skipif(not CALLS.is_file(), reason="run tools/ghidra/run-script pngdat_calls.py")
def test_pack_list_matches_the_ghidra_call_listing():
    """work/re/pngdat_calls.txt (tools/ghidra/scripts/pngdat_calls.py) agrees with PACKS."""
    helpers = {"load_flat": pngdat.LOAD_FLAT, "load_2d": pngdat.LOAD_2D, "load_3d": pngdat.LOAD_3D}
    seen = set()
    for line in CALLS.read_text().splitlines():
        loader, _, call, helper, path, *rest = line.split()
        pack = pngdat.BY_PATH[path.strip("'/")]
        if helper == "load_one":  # (index, count, id, image): one etc.dat image for the egg
            index, count, pack_id = (int(v, 16) for v in rest[:3])
            assert (pack.id, pack.count) == (pack_id, count) and index < count
            continue
        *dims, pack_id = (int(v, 16) for v in rest[1:])  # rest[0] is the array: '?'
        assert (pack.id, pack.shape) == (pack_id, tuple(dims)), path
        assert (pack.loader, pack.call, pack.helper) == (
            int(loader, 16),
            int(call, 16),
            helpers[helper],
        )
        seen.add(pack.path)
    assert seen == {p.path for p in pngdat.PACKS if p.loader is not None}
