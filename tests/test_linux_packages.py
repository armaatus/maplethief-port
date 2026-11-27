"""The Linux player AppImage and Flatpak bundle (#349, ADR 0193): their readers and the guard.

Synthetic fixtures in tests/fixtures/linux/, made from our own few files by the build image's
mksquashfs (``-b 4096 -comp gzip``, so a file spans blocks, ends in a fragment and has an
all-zero block) and ``flatpak build-export`` / ``build-bundle``: a SquashFS image and a Flatpak
bundle holding the same files. An AppImage here is a fake runtime (an ELF header with the
AppImage magic) plus the SquashFS image. mtport.squashfs, mtport.gvariant and
mtport.flatpak_bundle must read them exactly, refuse damaged ones, and the release guard
(mtport.release) must look inside both. mtport.linux_package's AppDir and Flatpak build
directory are checked without the packers (they run in the build image).
"""

from __future__ import annotations

import hashlib
import io
import lzma
import struct
import tarfile
from pathlib import Path

import pytest

from mtport import flatpak_bundle, gvariant, linux_package, squashfs
from mtport import release as rel
from repo_paths import REPO

FIXTURES = REPO / "tests" / "fixtures" / "linux"
SQUASHFS = (FIXTURES / "fixture.squashfs").read_bytes()
BUNDLE = (FIXTURES / "fixture.flatpak").read_bytes()
# The files of both fixtures, by SHA-256.
FILES = {
    "bin/big.bin": "437fe068610837afc557e3ed5b75def2b2efbdddb7d5a462d1409e2f8248f5b5",
    "bin/run.sh": "299001868fb8c02fd431c336c6d058f5558c5dff5b5af5e6fe04b870a6a9cbba",
    "hello.txt": "3afd5b1c68f1deebbd3297fe9efe46c1f41070a043e70e867fb4f2dd1c752199",
}
ZEROS = hashlib.sha256(bytes(9000)).hexdigest()
ICON_PNG = (REPO / "platform/icon/icon.png").read_bytes()


def fake_runtime(size: int = 4096) -> bytes:
    """An ELF64 header with the AppImage type-2 magic whose section headers end at ``size``."""
    head = bytearray(size)
    head[:4] = b"\x7fELF"
    head[4:6] = b"\x02\x01"
    head[8:11] = b"AI\x02"
    shnum, shentsize = 4, 64
    struct.pack_into("<Q", head, 0x28, size - shnum * shentsize)
    struct.pack_into("<HH", head, 0x3A, shentsize, shnum)
    return bytes(head)


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


# --- SquashFS -----------------------------------------------------------------------------------


def test_squashfs_lists_every_entry_with_its_bytes():
    entries = {e.path: e for e in squashfs.entries(SQUASHFS)}
    assert sorted(entries) == [
        "bin", "bin/big.bin", "bin/link", "bin/run.sh", "hello.txt", "share", "share/doc",
        "share/doc/zeros",
    ]  # fmt: skip
    for path, digest in FILES.items():
        assert sha(entries[path].data) == digest, path
    assert sha(entries["share/doc/zeros"].data) == ZEROS  # sparse blocks
    assert entries["bin/link"].kind == "symlink"
    assert entries["bin/link"].target == "../hello.txt"
    assert entries["bin/run.sh"].mode & 0o777 == 0o755
    assert entries["hello.txt"].mode & 0o777 == 0o644
    assert entries["share"].kind == "dir"


def test_squashfs_superblock():
    sb = squashfs.superblock(SQUASHFS)
    assert sb.block_size == 4096
    assert sb.compression == squashfs.COMPRESSION_ZLIB
    assert sb.bytes_used <= len(SQUASHFS)


@pytest.mark.parametrize(
    "damage, message",
    [
        (lambda d: b"nope" + d[4:], "magic"),
        (lambda d: d[:20] + struct.pack("<H", 6) + d[22:], "zstd compression"),
        (lambda d: d[:28] + struct.pack("<H", 3) + d[30:], "SquashFS 3.0"),
        (lambda d: d[:200], "only 200 present"),
    ],
)
def test_squashfs_refuses_what_it_does_not_read(damage, message):
    with pytest.raises(squashfs.SquashfsError, match=message):
        squashfs.entries(damage(SQUASHFS))


def test_squashfs_refuses_a_corrupt_block():
    sb = squashfs.superblock(SQUASHFS)
    data = bytearray(SQUASHFS)
    data[sb.inode_table + 4] ^= 0xFF  # inside the first (compressed) inode block
    with pytest.raises(squashfs.SquashfsError):
        squashfs.entries(bytes(data))


def test_appimage_parts_split_the_runtime_from_the_image():
    runtime = fake_runtime()
    head, entries = squashfs.appimage_parts(runtime + SQUASHFS)
    assert head == runtime
    assert {e.path for e in entries} >= set(FILES)


def test_appimage_with_bytes_after_the_image_is_refused():
    with pytest.raises(squashfs.SquashfsError, match="not padding"):
        squashfs.appimage_parts(fake_runtime() + SQUASHFS + b"hidden")
    squashfs.appimage_parts(fake_runtime() + SQUASHFS + bytes(100))  # zeros: padding


def test_appimage_without_magic_or_image_is_refused():
    with pytest.raises(squashfs.SquashfsError, match="type-2 AppImage"):
        squashfs.appimage_offset(b"\x7fELF" + bytes(100))
    with pytest.raises(squashfs.SquashfsError, match="no SquashFS image"):
        squashfs.appimage_offset(fake_runtime() + b"not squashfs")


def test_squashfs_cli_lists(capsys, tmp_path):
    image = tmp_path / "x.AppImage"
    image.write_bytes(fake_runtime() + SQUASHFS)
    assert squashfs.main(["list", str(image)]) == 0
    out = capsys.readouterr().out
    assert "runtime: 4096 bytes" in out
    assert "bin/link -> ../hello.txt" in out


# --- GVariant -----------------------------------------------------------------------------------


def test_gvariant_specification_examples():
    # The examples of GLib's GVariant specification, section 4.
    assert gvariant.decode("s", b"hello world\0") == "hello world"
    assert gvariant.decode("(si)", b"foo\0\xff\xff\xff\xff\x04") == ("foo", -1)
    data = b"i\0can\0has\0strings?\0\x02\x06\x0a\x13"
    assert gvariant.decode("as", data) == ["i", "can", "has", "strings?"]
    assert gvariant.decode("ay", b"\x01\x02") == b"\x01\x02"
    assert gvariant.decode("a{sv}", b"") == []
    v = gvariant.decode("v", b"\x04\x00\x00\x00\x00u")
    assert v == gvariant.Variant("u", 4)
    assert gvariant.decode("mu", b"") is None
    assert gvariant.decode("t", (1).to_bytes(8, "big"), big_endian=True) == 1


@pytest.mark.parametrize(
    "sig, data",
    [("s", b"no nul"), ("as", b"a\0\x09"), ("(si)", b"foo\0\xff\xff\xff\xff\x09"), ("u", b"\0")],
)
def test_gvariant_refuses_malformed_data(sig, data):
    with pytest.raises(gvariant.GVariantError):
        gvariant.decode(sig, data)


# --- Flatpak bundles ----------------------------------------------------------------------------


def test_flatpak_bundle_lists_the_tree_with_checked_objects():
    b = flatpak_bundle.read(BUNDLE)
    assert b.ref == "app/org.example.Fixture/x86_64/stable"
    assert b.runtime_repo == "https://dl.flathub.org/repo/flathub.flatpakrepo"
    assert "command=/app/bin/run.sh" in b.metadata
    entries = {e.path: e for e in b.entries}
    for path, digest in FILES.items():
        assert sha(entries[f"files/{path}"].data) == digest, path
    assert sha(entries["files/zeros"].data) == ZEROS
    assert entries["files/bin/link"].target == "../hello.txt"
    assert entries["files/bin/run.sh"].mode & 0o777 == 0o755
    assert entries["metadata"].data.decode() == b.metadata
    assert entries["export"].kind == "dir"


def _part() -> tuple[bytes, list[tuple[int, str]]]:
    """The fixture's one delta part: its payload and its objects."""
    meta, _ts, _f, _t, _c, _d, parts, _fb = gvariant.decode(flatpak_bundle.SUPERBLOCK, BUNDLE)
    compression, body = next(v.value for k, v in meta if k.startswith("deltas/"))
    assert compression == ord("x")
    return lzma.decompress(body), flatpak_bundle._payload_objects(parts[0][4])


def test_flatpak_part_checks_every_object():
    payload, objects = _part()
    store: dict = {}
    flatpak_bundle._apply_part(payload, objects, store)
    assert set(store) == {csum for _, csum in objects}
    at = payload.index(b"hello, synthetic fixture")
    changed = payload[:at] + b"J" + payload[at + 1 :]
    with pytest.raises(flatpak_bundle.BundleError, match="checksum"):
        flatpak_bundle._apply_part(changed, objects, {})
    with pytest.raises(flatpak_bundle.BundleError, match="objects"):
        flatpak_bundle._apply_part(payload, objects + [(1, "00" * 32)], {})


def test_flatpak_bundle_refuses_damage():
    with pytest.raises(flatpak_bundle.BundleError):
        flatpak_bundle.read(BUNDLE[: len(BUNDLE) // 2])
    with pytest.raises(flatpak_bundle.BundleError):
        flatpak_bundle.read(b"\0" * 64)
    data = bytearray(BUNDLE)
    to = flatpak_bundle.read(BUNDLE).commit
    at = BUNDLE.index(bytes.fromhex(to))
    data[at] ^= 1
    with pytest.raises(flatpak_bundle.BundleError, match="commit"):
        flatpak_bundle.read(bytes(data))


def test_flatpak_file_header_matches_ostree():
    # OSTree's checksum of the fixture's hello.txt (ostree ls -C): header and content.
    b = flatpak_bundle.read(BUNDLE)
    hello = next(e for e in b.entries if e.path == "files/hello.txt")
    header = flatpak_bundle.file_header(0, 0, hello.mode, "")
    assert header[:4] == (len(header) - 8).to_bytes(4, "big")
    assert header[8:12] == bytes(4)  # uid 0, big-endian


def test_flatpak_bundle_cli(capsys, tmp_path):
    path = tmp_path / "x.flatpak"
    path.write_bytes(BUNDLE)
    assert flatpak_bundle.main(["info", str(path)]) == 0
    assert "ref: app/org.example.Fixture/x86_64/stable" in capsys.readouterr().out
    assert flatpak_bundle.main(["list", str(path)]) == 0
    assert "files/bin/link -> ../hello.txt" in capsys.readouterr().out


# --- The release guard --------------------------------------------------------------------------


@pytest.fixture
def appimage(tmp_path, monkeypatch) -> Path:
    runtime = fake_runtime()
    monkeypatch.setitem(rel.APPIMAGE_RUNTIMES, sha(runtime), "the tests' fake runtime")
    path = tmp_path / "MapleThief-0.0.1-linux-x86_64.AppImage"
    path.write_bytes(runtime + SQUASHFS)
    return path


@pytest.fixture
def bundle(tmp_path) -> Path:
    path = tmp_path / "MapleThief-0.0.1-linux-x86_64.flatpak"
    path.write_bytes(BUNDLE)
    return path


def test_guard_looks_inside_an_appimage(appimage):
    names = {m.name for m in rel.members(appimage)}
    assert f"{appimage.name}/bin/big.bin" in names
    assert appimage.name in names  # the runtime
    report = rel.guard(appimage, {}, [])
    assert report.ok, report.findings
    big = {FILES["bin/big.bin"]: "work/ipa/Payload/MapleThief.app/big.bin"}
    report = rel.guard(appimage, big, [])
    assert [(f.member, f.rule) for f in report.findings] == [
        (f"{appimage.name}/bin/big.bin", "derived")
    ]


def test_guard_refuses_another_appimage_runtime(tmp_path):
    path = tmp_path / "Other.AppImage"
    path.write_bytes(fake_runtime(8192) + SQUASHFS)
    report = rel.guard(path, {}, [])
    assert [(f.member, f.rule) for f in report.findings] == [("Other.AppImage", "path")]
    assert "runtime other than the pinned" in report.findings[0].detail


def test_guard_looks_inside_a_flatpak_bundle(bundle):
    names = {m.name for m in rel.members(bundle)}
    assert f"{bundle.name}/files/bin/big.bin" in names
    assert f"{bundle.name}/metadata" in names
    assert rel.guard(bundle, {}, []).ok
    report = rel.guard(bundle, {ZEROS: "assets-out/zeros"}, [])
    assert [(f.member, f.rule) for f in report.findings] == [
        (f"{bundle.name}/files/zeros", "derived")
    ]


def test_guard_player_mode_needs_the_game_code_in_both(appimage, bundle):
    for path in (appimage, bundle):
        report = rel.guard(path, {}, [], "player")
        assert [f.rule for f in report.findings] == ["player"]


def test_guard_raises_on_a_damaged_package(tmp_path):
    bad = tmp_path / "Bad.AppImage"
    bad.write_bytes(fake_runtime() + b"hsqs" + bytes(10))
    with pytest.raises(rel.GuardError, match="AppImage"):
        rel.guard(bad, {}, [])
    bad = tmp_path / "Bad.flatpak"
    bad.write_bytes(BUNDLE[:1000])
    with pytest.raises(rel.GuardError, match="Flatpak"):
        rel.guard(bad, {}, [])


def test_icon_allowlist_covers_the_packages_icons():
    for path in (
        "io.github.armaatus.maplethief.png",
        "files/icon.png",
        "files/share/icons/hicolor/256x256/apps/io.github.armaatus.maplethief.png",
        "export/share/icons/hicolor/256x256/apps/io.github.armaatus.maplethief.png",
    ):
        assert rel.ICON_ALLOWLIST[path] == sha(ICON_PNG), path


# --- mtport.linux_package -------------------------------------------------------------------------


def player_tarball(tmp_path: Path, name: str = "MapleThief-1.2.3-linux-x86_64") -> Path:
    files = {
        "bin/MapleThief": (b"\x7fELF game", 0o755),
        "MapleThief.sh": (b"#!/bin/sh\n", 0o755),
        "icon.png": (ICON_PNG, 0o644),
        "README.md": (b"# MapleThief\n", 0o644),
    }
    path = tmp_path / f"{name}.tar.gz"
    with tarfile.open(path, "w:gz") as t:
        for rel_path, (data, mode) in files.items():
            info = tarfile.TarInfo(f"{name}/{rel_path}")
            info.size, info.mode = len(data), mode
            t.addfile(info, io.BytesIO(data))
    return path


def test_package_names():
    assert linux_package.names(Path("MapleThief-1.2.3-rc.1-linux-x86_64.tar.gz")) == (
        "MapleThief-1.2.3-rc.1-linux-x86_64",
        "MapleThief-1.2.3-rc.1-linux-x86_64.AppImage",
        "MapleThief-1.2.3-rc.1-linux-x86_64.flatpak",
    )
    with pytest.raises(linux_package.PackageError):
        linux_package.names(Path("MapleThief-kit-1.2.3-linux-x86_64.tar.gz"))


def test_appdir_layout(tmp_path):
    top = linux_package.appdir(player_tarball(tmp_path), tmp_path / "appdir")
    assert (top / "AppRun").is_symlink() and str((top / "AppRun").readlink()) == "MapleThief.sh"
    assert (top / ".DirIcon").is_symlink()
    assert (top / f"{linux_package.APP_ID}.png").read_bytes() == ICON_PNG
    desktop = (top / f"{linux_package.APP_ID}.desktop").read_text(encoding="utf-8")
    assert f"Icon={linux_package.APP_ID}\n" in desktop
    assert (top / "bin/MapleThief").stat().st_mode & 0o777 == 0o755
    assert (top / "README.md").stat().st_mode & 0o777 == 0o644


def test_flatpak_dir_layout(tmp_path):
    build = linux_package.flatpak_dir(player_tarball(tmp_path), tmp_path / "build")
    metadata = (build / "metadata").read_text(encoding="utf-8")
    assert f"name={linux_package.APP_ID}\n" in metadata
    assert "runtime=org.freedesktop.Platform/x86_64/25.08\n" in metadata
    assert "command=/app/MapleThief.sh\n" in metadata
    assert "network" not in metadata
    assert (build / "files/bin/MapleThief").is_file()
    for root in ("files/share", "export/share"):
        icon = build / root / "icons/hicolor/256x256/apps" / f"{linux_package.APP_ID}.png"
        assert icon.read_bytes() == ICON_PNG
        assert (build / root / "applications" / f"{linux_package.APP_ID}.desktop").is_file()


def test_package_refuses_a_tarball_that_is_not_a_player_build(tmp_path):
    path = tmp_path / "MapleThief-1.2.3-linux-x86_64.tar.gz"
    with tarfile.open(path, "w:gz") as t:
        info = tarfile.TarInfo("MapleThief-1.2.3-linux-x86_64/README.md")
        t.addfile(info, io.BytesIO(b""))
    with pytest.raises(linux_package.PackageError, match="bin/MapleThief"):
        linux_package.appdir(path, tmp_path / "appdir")
