"""Nintendo Switch NROs and NACPs (mtport.nro, docs/formats/nro.md), the Switch icon's JPEG
(mtport.jpeg), and the release guard on an NRO (mtport.release, ADR 0138).

The fixtures are synthetic: a header laid out byte by byte from switchbrew's description, and
NROs from mtport.nro.build. test_switch_job_nro reads the MapleThief.nro that
scripts/build-switch (the `switch` job) leaves in build/switch-*/ and skips when there is none.
"""

from __future__ import annotations

import os
import re
import struct
import zipfile
from pathlib import Path

import pytest

from mtport import appicon, jpeg, nro
from mtport import release as rel
from repo_paths import REPO

ICON = appicon.switch_icon()
NACP = nro.build_nacp("MapleThief", "armaatus", "1.2.3-rc.4")


def handmade_nro() -> bytes:
    """An NRO written field by field from https://switchbrew.org/wiki/NRO, not with nro.build."""
    code = b"\x1f\x20\x03\xd5" * 0x400  # a page of NOPs as the text segment
    size = 0x80 + len(code)
    head = bytearray(0x80)
    struct.pack_into("<II", head, 0x00, 0, 0)  # NroStart: unused, MOD0 offset
    head[0x10:0x14] = b"NRO0"
    struct.pack_into("<III", head, 0x14, 0, size, 0)  # version, size, flags
    struct.pack_into("<IIIIII", head, 0x20, 0x80, len(code), size, 0, size, 0)  # text ro data
    struct.pack_into("<I", head, 0x38, 0x1000)  # bss
    head[0x40:0x60] = bytes(range(0x20))  # build id
    icon_off = 0x38
    nacp_off = icon_off + len(ICON)
    asset = b"ASET" + struct.pack("<I", 0)
    asset += struct.pack("<QQ", icon_off, len(ICON))
    asset += struct.pack("<QQ", nacp_off, len(NACP))
    asset += struct.pack("<QQ", 0, 0)  # no RomFS
    assert len(asset) == 0x38
    return bytes(head) + code + asset + ICON + NACP


def test_reads_a_handmade_nro():
    n = nro.parse(handmade_nro())
    assert n.size == 0x80 + 0x1000
    assert n.segments[0] == nro.Segment(0x80, 0x1000)
    assert n.bss_size == 0x1000
    assert n.build_id == bytes(range(0x20))
    assert n.has_assets and n.icon == ICON and n.nacp == NACP and n.romfs == b""
    nacp = nro.parse_nacp(n.nacp)
    assert nacp.title == nro.Title("MapleThief", "armaatus")
    assert nacp.display_version == "1.2.3-rc.4"
    assert len(nacp.titles) == 16 and all(t == nacp.title for t in nacp.titles)


def test_nacp_layout_follows_switchbrew():
    raw = nro.build_nacp("Name", "Author", "9.8.7")
    assert len(raw) == 0x4000
    assert raw[0x000:0x005] == b"Name\0"
    assert raw[0x200:0x207] == b"Author\0"
    assert raw[0x300:0x305] == b"Name\0"  # the second language's entry
    assert raw[0x3060:0x3066] == b"9.8.7\0"


def test_check_passes_and_names_each_mismatch():
    data = handmade_nro()
    ok = nro.check(data, "MapleThief", "armaatus", "1.2.3-rc.4", 256)
    assert ok == []
    bad = nro.check(data, "Other", "someone", "0.0.1", 128)
    assert len(bad) == 4
    assert any("name" in p for p in bad) and any("author" in p for p in bad)
    assert any("version" in p for p in bad) and any("256 x 256" in p for p in bad)


def test_check_reports_a_missing_or_wrong_icon():
    assert "no icon" in " ".join(nro.check(nro.build(b"x", nacp=NACP), icon_size=256))
    png_icon = b"\x89PNG\r\n\x1a\n" + bytes(32)
    assert "not a JPEG" in " ".join(
        nro.check(nro.build(b"x", icon=png_icon, nacp=NACP), icon_size=256)
    )
    assert nro.check(nro.build(b"x")) == ["no asset section (ASET): no icon and no NACP"]


@pytest.mark.parametrize(
    "mutate, message",
    [
        (lambda d: d[:0x40], "too short"),
        (lambda d: d[:0x10] + b"XXXX" + d[0x14:], "NRO0"),
        (lambda d: d[:0x18] + struct.pack("<I", 0xFFFFFF) + d[0x1C:], "outside the file"),
        (lambda d: d + b"junk", None),  # trailing bytes after the asset section are its own
    ],
)
def test_malformed_nros(mutate, message):
    data = mutate(handmade_nro())
    if message is None:
        nro.parse(data)
        return
    with pytest.raises(nro.NroError, match=message):
        nro.parse(data)


def test_bytes_after_the_code_without_aset_fail():
    data = nro.build(b"code") + b"not an asset header at all, but long enough to read......"
    with pytest.raises(nro.NroError, match="no ASET"):
        nro.parse(data)


def test_build_round_trips_and_members():
    data = nro.build(b"\x01" * 10, icon=ICON, nacp=NACP, romfs=b"RomFS!")
    names = [name for name, _ in nro.members(data)]
    assert names == ["code.bin", "icon.jpg", "control.nacp", "romfs.bin"]
    assert dict(nro.members(data))["romfs.bin"] == b"RomFS!"


def test_cli_check_and_unpack(tmp_path, capsys):
    path = tmp_path / "MapleThief.nro"
    path.write_bytes(handmade_nro())
    args = ["check", str(path), "--name", "MapleThief", "--author", "armaatus"]
    assert nro.main([*args, "--version", "1.2.3-rc.4", "--icon-size", "256"]) == 0
    assert "OK" in capsys.readouterr().out
    assert nro.main([*args, "--version", "0.0.1"]) == 1
    assert nro.main(["unpack", str(path), str(tmp_path / "out")]) == 0
    assert (tmp_path / "out" / "icon.jpg").read_bytes() == ICON
    assert (tmp_path / "out" / "control.nacp").read_bytes() == NACP


# --- The JPEG encoder and reader ------------------------------------------------------------


def test_jpeg_encoder_writes_a_baseline_jfif():
    rgb = b"".join(bytes((x * 16, y * 16, 128)) for y in range(16) for x in range(16))
    data = jpeg.encode_rgb(16, 16, rgb, 75)
    assert data[:4] == b"\xff\xd8\xff\xe0" and data[6:11] == b"JFIF\0" and data[-2:] == b"\xff\xd9"
    assert jpeg.info(data) == jpeg.JpegInfo(16, 16, 3, True)
    assert data == jpeg.encode_rgb(16, 16, rgb, 75)


def test_jpeg_odd_sizes_and_quality_tables():
    rgb = bytes(range(256)) * 3 * 5
    rgb = rgb[: 13 * 7 * 3]
    assert jpeg.info(jpeg.encode_rgb(13, 7, rgb)).width == 13
    assert jpeg.quant_table((16, 255), 50) == (16, 255)
    assert jpeg.quant_table((16, 255), 100) == (1, 1)
    with pytest.raises(ValueError):
        jpeg.quant_table((16,), 0)


def test_jpeg_info_rejects_other_files():
    with pytest.raises(jpeg.JpegError):
        jpeg.info(b"\x89PNG\r\n\x1a\n")
    with pytest.raises(jpeg.JpegError):
        jpeg.info(b"\xff\xd8\xff\xda\x00\x02")  # a scan before any frame header


# --- The release guard on an NRO ------------------------------------------------------------


def guard(path: Path, table=()) -> rel.Report:
    return rel.guard(path, {}, list(table))


def test_guard_passes_our_nro(tmp_path):
    path = tmp_path / "MapleThief.nro"
    path.write_bytes(nro.build(b"our own code", icon=ICON, nacp=NACP))
    report = guard(path)
    assert report.ok, report.findings
    assert report.files == 3  # code, icon, NACP


def test_guard_rejects_another_icon_a_romfs_and_generated_code(tmp_path):
    other = jpeg.encode_rgb(8, 8, bytes(8 * 8 * 3))
    path = tmp_path / "MapleThief.nro"
    path.write_bytes(nro.build(b"\0 f_0002A3C1 \0", icon=other, nacp=NACP, romfs=b"RomFS"))
    found = {(f.member, f.rule) for f in guard(path).findings}
    assert ("MapleThief.nro/icon.jpg", "derived") in found
    assert ("MapleThief.nro/data.romfs", "path") in found
    assert ("MapleThief.nro", "symbols") in found


def test_guard_looks_inside_an_nro_in_a_kit(tmp_path):
    kit = tmp_path / "MapleThief-kit-0.0.1-switch.zip"
    with zipfile.ZipFile(kit, "w") as z:
        z.writestr("MapleThief-kit-0.0.1-switch/README.md", "# MapleThief\n")
        z.writestr(
            "MapleThief-kit-0.0.1-switch/MapleThief.nro",
            nro.build(b"code", icon=ICON, nacp=NACP),
        )
    assert guard(kit).ok
    with zipfile.ZipFile(kit, "a") as z:
        z.writestr("MapleThief-kit-0.0.1-switch/Other.nro", nro.build(b"code", icon=ICON))
    found = {(f.member, f.rule) for f in guard(kit).findings}
    assert found == {("MapleThief-kit-0.0.1-switch/Other.nro/icon.jpg", "derived")}


def test_guard_fails_on_an_unreadable_nro(tmp_path):
    path = tmp_path / "broken.nro"
    path.write_bytes(b"not an NRO" * 20)
    with pytest.raises(rel.GuardError, match="not an NRO"):
        guard(path)


# --- The NRO the switch job left ------------------------------------------------------------

BUILT = [REPO / "build" / p / "MapleThief.nro" for p in ("switch-release", "switch-debug")]


@pytest.mark.parametrize("path", BUILT, ids=[p.parent.name for p in BUILT])
def test_switch_job_nro(path: Path):
    if not path.is_file():
        pytest.skip(f"no {path.relative_to(REPO)} (scripts/build-switch builds it)")
    data = path.read_bytes()
    assert nro.check(data, "MapleThief", "armaatus", None, 256) == []
    version = nro.parse_nacp(nro.parse(data).nacp).display_version
    assert re.fullmatch(r"\d+\.\d+\.\d+(-rc\.\d+)?", version)
    if os.environ.get("MAPLETHIEF_VERSION"):
        assert version == os.environ["MAPLETHIEF_VERSION"]
    assert guard(path).ok
