"""The pinned Ghidra/JDK/Gradle downloads and their unpacking (mtport.ghidra_setup), offline."""

import hashlib
import io
import os
import re
import tarfile
import zipfile
from pathlib import Path

import pytest

from mtport import ghidra_setup as gs
from repo_paths import REPO


def test_pins_are_well_formed():
    archives = [gs.GHIDRA, gs.GRADLE, *gs.JDKS.values()]
    for a in archives:
        assert re.fullmatch(r"[0-9a-f]{64}", a.sha256), a.url
        assert a.url.startswith("https://github.com/") and a.size > 10_000_000
        assert a.filename.endswith((".zip", ".tar.gz"))
    assert {j.top for j in gs.JDKS.values()} == {f"jdk-{gs.JDK_VERSION}"}
    assert gs.GHIDRA.top == f"ghidra_{gs.GHIDRA_VERSION}_PUBLIC"
    assert set(gs.JDKS) == set(gs._NATIVE_OS)


def test_versions_match_third_party_md():
    text = (REPO / "THIRD_PARTY.md").read_text(encoding="utf-8")
    assert f"| Ghidra | {gs.GHIDRA_VERSION} |" in text
    assert f"| Temurin JDK | {gs.JDK_VERSION} |" in text
    assert gs.GHIDRA.sha256 in text


@pytest.mark.parametrize(
    ("system", "machine", "home"),
    [("Darwin", "arm64", "Contents/Home"), ("Linux", "x86_64", ""), ("Windows", "AMD64", "")],
)
def test_jdk_for(system, machine, home):
    assert gs.jdk_for(system, machine).home == home


def test_jdk_for_unknown_host():
    with pytest.raises(gs.SetupError, match="no pinned JDK"):
        gs.jdk_for("Plan9", "mips")


def test_layout_and_environment(tmp_path):
    layout = gs.Layout.for_repo(tmp_path, env={})
    assert layout.downloads == tmp_path / "build" / "_downloads"
    assert layout.root == tmp_path / "build" / "ghidra"
    env = gs.environment(layout, gs.JDKS[("Darwin", "arm64")])
    assert env["GHIDRA_INSTALL_DIR"] == str(tmp_path / "build/ghidra/ghidra_12.2_PUBLIC")
    assert Path(env["JAVA_HOME"]).parts[-3:] == ("jdk-25.0.4.1+1", "Contents", "Home")
    assert all(str(tmp_path) in v for v in env.values())
    other = gs.Layout.for_repo(tmp_path, env={"MAPLETHIEF_DOWNLOAD_DIR": "/cache"})
    assert other.downloads == Path("/cache")


def _archive(tmp_path, kind, entries):
    """A zip or tar.gz with ``(name, data, mode)`` entries, plus its pin."""
    path = tmp_path / f"t.{kind}"
    if kind == "zip":
        with zipfile.ZipFile(path, "w") as z:
            for name, data, mode in entries:
                info = zipfile.ZipInfo(name)
                info.filename = name  # ZipInfo turns "\\" into "/" on Windows; keep it raw
                info.external_attr = mode << 16
                z.writestr(info, data)
    else:
        with tarfile.open(path, "w:gz") as t:
            for name, data, mode in entries:
                info = tarfile.TarInfo(name)
                info.size, info.mode = len(data), mode
                t.addfile(info, io.BytesIO(data))
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    return path, gs.Archive(f"https://example.invalid/{path.name}", digest, 1, "top")


@pytest.mark.parametrize("kind", ["zip", "tar.gz"])
def test_unpack_keeps_exec_bit_and_stamps(tmp_path, kind):
    cached, pin = _archive(
        tmp_path,
        kind,
        [("top/bin/tool", b"#!/bin/sh\n", 0o100755), ("top/doc.txt", b"hi", 0o100644)],
    )
    root = tmp_path / "root"
    target = gs.unpack(pin, cached, root, log=lambda _: None)
    assert (target / "doc.txt").read_bytes() == b"hi"
    if os.name != "nt":  # Windows has no exec bit
        assert (target / "bin" / "tool").stat().st_mode & 0o111
        assert not (target / "doc.txt").stat().st_mode & 0o111
    assert (root / "top.sha256").read_text().strip() == pin.sha256
    (target / "doc.txt").write_text("changed")
    gs.unpack(pin, cached, root, log=lambda _: None)  # stamp matches: left alone
    assert (target / "doc.txt").read_text() == "changed"


@pytest.mark.parametrize("bad", ["../evil", "/abs/evil", "other/file", "top/../../evil", "top\\x"])
def test_unpack_rejects_escaping_entries(tmp_path, bad):
    cached, pin = _archive(tmp_path, "zip", [("top/ok", b"", 0o100644), (bad, b"x", 0o100644)])
    with pytest.raises(gs.SetupError):
        gs.unpack(pin, cached, tmp_path / "root", log=lambda _: None)
    assert not (tmp_path / "root" / "top").exists()
    assert not (tmp_path / "evil").exists()


def test_fetch_uses_matching_cache_and_never_the_network(tmp_path, monkeypatch):
    cached, pin = _archive(tmp_path, "zip", [("top/a", b"a", 0o100644)])

    def no_network(*args, **kwargs):
        raise AssertionError("network used")

    monkeypatch.setattr(gs.urllib.request, "urlopen", no_network)
    assert gs.fetch(pin, tmp_path, log=lambda _: None) == cached


def test_has_decompiler(tmp_path):
    assert not gs.has_decompiler(tmp_path, "mac_arm_64")
    built = tmp_path / "Ghidra/Features/Decompiler/build/os/mac_arm_64/decompile"
    built.parent.mkdir(parents=True)
    built.write_bytes(b"")
    assert gs.has_decompiler(tmp_path, "mac_arm_64")
    assert not gs.has_decompiler(tmp_path, "win_x86_64")
