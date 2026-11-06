"""The compendium's inputs bundle and the deploy on a push to main (issue #248, ADR 0121
addendum): the extraction fingerprint, packing and unpacking the bundle, and a site build
from an unpacked bundle in a checkout without work/ or the IPA. Everything is synthetic
(tests/fixtures/compendium/site_kb.py); nothing here talks to GitHub."""

from __future__ import annotations

import io
import json
import shutil
import tarfile
from pathlib import Path

import pytest

from mtport.compendium import bundle, cli, inputs
from repo_paths import REPO
from tests.fixtures.compendium import site_kb as fx
from tests.test_compendium_deploy import fake_gh, needs_tools, run_steps

# --- the fingerprint ----------------------------------------------------------------------------


def test_fingerprint_hashes_the_extraction_code_and_nothing_of_the_site():
    files = {p.relative_to(REPO).as_posix() for p in inputs.source_files()}
    for need in (
        "tools/mtport/__init__.py",
        "tools/mtport/compendium/__init__.py",
        "tools/mtport/compendium/kb.py",
        "tools/mtport/compendium/images.py",
        # the KB stages, imported inside kb.py's functions
        "tools/mtport/compendium/tables.py",
        "tools/mtport/compendium/world.py",
        "tools/mtport/compendium/quests.py",
        "tools/mtport/compendium/scriptwalk.py",
        # the parsers they use
        "tools/mtport/datatables.py",
        "tools/mtport/pngdat.py",
        "tools/mtport/scriptout.py",
    ):
        assert need in files, need
    for site_only in (
        "tools/mtport/compendium/site.py",
        "tools/mtport/compendium/siteapi.py",
        "tools/mtport/compendium/cli.py",
        "tools/mtport/compendium/bundle.py",
        "tools/mtport/compendium/inputs.py",
        "tools/mtport/compendium/templates.py",
        "tools/mtport/compendium/defaults.py",
        "tools/mtport/htmlpage.py",
        "tools/mtport/prose_lint.py",
    ):
        assert site_only not in files, site_only
    assert not any("/sections/" in f for f in files)
    hashes = inputs.source_hashes()
    assert set(hashes) == files
    assert inputs.fingerprint() == inputs.combine(hashes)
    assert len(inputs.fingerprint()) == 64


def write_tree(root: Path, files: dict[str, str]) -> Path:
    tools = root / "tools"
    for rel, text in files.items():
        f = tools / rel
        f.parent.mkdir(parents=True, exist_ok=True)
        f.write_bytes(text.encode("utf-8"))
    return tools


TREE = {
    "mtport/__init__.py": "",
    "mtport/compendium/__init__.py": "",
    "mtport/compendium/kb.py": (
        "import json\n"
        "from mtport import parser\n"
        "def build():\n"
        "    from mtport.compendium import stage  # a stage, imported late\n"
    ),
    "mtport/compendium/images.py": "from . import pictures\n",
    "mtport/compendium/stage.py": "from ..helpers import util\n",
    "mtport/compendium/pictures.py": "",
    "mtport/compendium/site.py": "from mtport.compendium import kb\n",
    "mtport/parser.py": "",
    "mtport/helpers/__init__.py": "",
    "mtport/helpers/util.py": "",
    "mtport/unused.py": "",
}


def test_fingerprint_follows_late_relative_and_package_imports(tmp_path):
    tools = write_tree(tmp_path, TREE)
    got = [p.relative_to(tools).as_posix() for p in inputs.source_files(tools)]
    assert got == [
        "mtport/__init__.py",
        "mtport/compendium/__init__.py",
        "mtport/compendium/images.py",
        "mtport/compendium/kb.py",
        "mtport/compendium/pictures.py",
        "mtport/compendium/stage.py",
        "mtport/helpers/__init__.py",
        "mtport/helpers/util.py",
        "mtport/parser.py",
    ]
    # repository-relative keys
    assert "tools/mtport/compendium/kb.py" in inputs.source_hashes(tools)


def test_fingerprint_changes_with_the_extraction_code_only(tmp_path):
    tools = write_tree(tmp_path, TREE)
    before = inputs.source_hashes(tools)
    fp = inputs.combine(before)
    (tools / "mtport/compendium/site.py").write_text("# the generator changed\n", encoding="utf-8")
    (tools / "mtport/unused.py").write_text("x = 1\n", encoding="utf-8")
    assert inputs.fingerprint(tools) == fp
    # CRLF line endings hash as LF (a Windows checkout)
    kb = tools / "mtport/compendium/kb.py"
    kb.write_bytes(kb.read_bytes().replace(b"\n", b"\r\n"))
    assert inputs.fingerprint(tools) == fp
    (tools / "mtport/helpers/util.py").write_text("CHANGED = True\n", encoding="utf-8")
    after = inputs.source_hashes(tools)
    assert inputs.combine(after) != fp
    assert inputs.changed_files(before, after) == ["tools/mtport/helpers/util.py: changed"]
    (tools / "mtport/compendium/images.py").write_text(
        "from mtport import extra\n", encoding="utf-8"
    )
    (tools / "mtport/extra.py").write_text("", encoding="utf-8")
    lines = inputs.changed_files(before, inputs.source_hashes(tools))
    assert "tools/mtport/extra.py: new" in lines
    assert "tools/mtport/compendium/pictures.py: no longer used" in lines


# --- the bundle ---------------------------------------------------------------------------------


def make_inputs(root: Path) -> tuple[Path, Path]:
    """A synthetic knowledge base and image set, laid out as scripts/compendium build does."""
    kb = fx.write_kb(root / "build" / "compendium")
    site = root / "build" / "compendium" / "site"
    manifest = fx.write_images(site)
    return kb, manifest.parent


HASHES = {"tools/mtport/compendium/kb.py": "a" * 64, "tools/mtport/compendium/images.py": "b" * 64}


def test_pack_and_unpack_round_trip(tmp_path):
    kb, img = make_inputs(tmp_path / "src")
    files = inputs.input_files(kb, img)
    names = [rel for rel, _ in files]
    assert "kb/index.json" in names and "kb/monster.json" in names
    assert "img/manifest.json" in names and any(n.startswith("img/monster/") for n in names)
    data = inputs.pack(files, HASHES)
    assert data == inputs.pack(inputs.input_files(kb, img), HASHES)  # deterministic
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tar:
        members = tar.getmembers()
        assert sorted(m.name for m in members) == sorted(names + [inputs.INFO])
        assert all(m.isfile() and m.mtime == 0 and m.uid == 0 and m.mode == 0o644 for m in members)
    fp, sources = inputs.read_info(data)
    assert fp == inputs.combine(HASHES) and sources == HASHES
    meta = inputs.metadata(data, HASHES)
    assert meta == {
        "schema": 1,
        "asset": "compendium-inputs.tar.gz",
        "sha256": bundle.sha256(data),
        "size": len(data),
        "fingerprint": fp,
    }
    assert inputs.check_metadata(data, meta) == fp
    got = inputs.unpack(data, tmp_path / "out" / "kb", tmp_path / "out" / "img")
    n_kb = sum(n.startswith("kb/") for n in names)
    n_img = sum(n.startswith("img/") for n in names)
    assert (got.fingerprint, got.kb_files, got.img_files) == (fp, n_kb, n_img)
    for rel, path in files:
        top, _, rest = rel.partition("/")
        assert (tmp_path / "out" / top / rest).read_bytes() == path.read_bytes()


def test_pack_refuses_what_the_bundle_never_holds(tmp_path):
    kb, img = make_inputs(tmp_path)
    (img / "music.ogg").write_bytes(b"o")
    (kb / "index.json").unlink()
    (img / "link.png").symlink_to(img / "manifest.json")
    problems = "\n".join(inputs.files_problems(inputs.input_files(kb, img)))
    assert "img/music.ogg: not a file the inputs bundle may hold" in problems
    assert "kb/index.json: missing" in problems
    assert "img/link.png: a symlink" in problems
    with pytest.raises(inputs.InputsError):
        inputs.pack(inputs.input_files(kb, img), HASHES)


def raw_tar(members: list[tuple[tarfile.TarInfo, bytes]]) -> bytes:
    out = io.BytesIO()
    with tarfile.open(fileobj=out, mode="w:gz") as tar:
        for info, data in members:
            info.size = len(data) if info.isfile() else 0
            tar.addfile(info, io.BytesIO(data) if info.isfile() else None)
    return out.getvalue()


def member(name: str, kind: bytes = tarfile.REGTYPE, link: str = "") -> tarfile.TarInfo:
    info = tarfile.TarInfo(name)
    info.type = kind
    info.linkname = link
    return info


INFO_OK = inputs.info_bytes(HASHES)


@pytest.mark.parametrize(
    "members, message",
    [
        ([(member("../evil.json"), b"{}")], "not a file an inputs bundle holds"),
        ([(member("/etc/passwd.json"), b"{}")], "not a file an inputs bundle holds"),
        ([(member("kb/../../x.json"), b"{}")], "not a file an inputs bundle holds"),
        ([(member("img/bgm.mp3"), b"x")], "not a file an inputs bundle holds"),
        ([(member("run.sh"), b"x")], "not a file an inputs bundle holds"),
        ([(member("img/a.png", tarfile.SYMTYPE, "/etc/passwd"), b"")], "not a plain file"),
        ([(member("img/a.png", tarfile.LNKTYPE, "kb/x.json"), b"")], "not a plain file"),
        ([(member("img", tarfile.DIRTYPE), b"")], "not a plain file"),
        ([(member("kb/a.json"), b"{}"), (member("kb/a.json"), b"{}")], "twice"),
        ([(member("kb/a.json"), b"{}")], "no inputs.json"),
        ([(member("inputs.json"), b"not json")], "inputs.json: unreadable"),
        (
            [(member("inputs.json"), INFO_OK.replace(b'"fingerprint": "', b'"fingerprint": "0'))],
            "does not match its own source list",
        ),
    ],
)
def test_unpack_refuses_an_unsafe_or_malformed_bundle(tmp_path, members, message):
    if not any(m.name == "inputs.json" for m, _ in members) and message != "no inputs.json":
        members = members + [(member("inputs.json"), INFO_OK)]
    data = raw_tar(members)
    with pytest.raises(inputs.InputsError, match=message):
        inputs.unpack(data, tmp_path / "kb", tmp_path / "img")
    assert not (tmp_path / "kb").exists() and not (tmp_path / "img").exists()
    assert not (tmp_path.parent / "evil.json").exists()


def test_unpack_needs_fresh_directories_and_a_tarball(tmp_path):
    kb, img = make_inputs(tmp_path / "src")
    data = inputs.pack(inputs.input_files(kb, img), HASHES)
    (tmp_path / "kb").mkdir()
    (tmp_path / "kb" / "old.json").write_text("{}", encoding="utf-8")
    with pytest.raises(inputs.InputsError, match="not empty"):
        inputs.unpack(data, tmp_path / "kb", tmp_path / "img")
    with pytest.raises(inputs.InputsError, match="not a .tar.gz"):
        inputs.read_info(b"not a tarball")


def test_check_metadata_catches_a_partial_or_mixed_upload(tmp_path):
    kb, img = make_inputs(tmp_path)
    data = inputs.pack(inputs.input_files(kb, img), HASHES)
    meta = inputs.metadata(data, HASHES)
    with pytest.raises(inputs.InputsError, match="a partial upload"):
        inputs.check_metadata(data[:-10], meta)
    with pytest.raises(inputs.InputsError, match="its fingerprint is not the one"):
        inputs.check_metadata(data, {**meta, "fingerprint": "0" * 64})
    with pytest.raises(inputs.InputsError, match="schema"):
        inputs.check_metadata(data, {**meta, "schema": 99})


# --- a site build from the inputs, without work/ ------------------------------------------------


@pytest.fixture
def checkout(tmp_path, monkeypatch) -> Path:
    """A directory laid out like the workflow's sparse checkout: synthetic prose in
    docs/compendium/pages/, no work/, no build/. The inputs bundle was made by this code."""
    co = tmp_path / "checkout"
    fx.write_prose(co / "docs" / "compendium")
    monkeypatch.chdir(co)
    return co


def bundle_files(tmp_path: Path, hashes: dict[str, str] | None = None) -> tuple[Path, Path]:
    kb, img = make_inputs(tmp_path / "builder")
    hashes = inputs.source_hashes() if hashes is None else hashes
    data = inputs.pack(inputs.input_files(kb, img), hashes)
    src, meta = tmp_path / inputs.INPUTS_NAME, tmp_path / inputs.INPUTS_META
    src.write_bytes(data)
    meta.write_bytes(inputs.metadata_bytes(inputs.metadata(data, hashes)))
    return src, meta


def test_site_builds_from_an_unpacked_bundle_without_work(tmp_path, checkout, capsys):
    src, meta = bundle_files(tmp_path)
    out = tmp_path / "site.tar.gz"
    rc = cli.cmd_from_inputs(src, meta, Path("build/compendium"), out)
    log = capsys.readouterr()
    assert rc == 0, log.out + log.err
    assert not (checkout / "work").exists()
    assert "0 problems" in log.out  # the site check
    with tarfile.open(out, mode="r:gz") as tar:
        names = set(tar.getnames())
    assert {"index.html", "404.html", "search-index.js", "img/manifest.json"} <= names
    assert "monster/1.html" in names and any(n.startswith("img/monster/") for n in names)
    assert not any(n.startswith(("kb/", ".")) or n == inputs.INFO for n in names)
    assert f"sha256 {bundle.sha256(out.read_bytes())}" in log.out
    # The same bytes as a build from the same files the usual way (scripts/compendium build).
    kb, img = make_inputs(tmp_path / "usual")
    site_dir = img.parent
    args = ["--kb", str(kb), "--out", str(site_dir)]
    from mtport.compendium import site as sitegen

    assert sitegen.main(["build", *args]) == 0
    assert bundle.pack(bundle.site_files(site_dir)) == out.read_bytes()


def test_a_stale_bundle_is_reported_not_built(tmp_path, checkout, capsys):
    here = inputs.source_hashes()
    old = dict(here)
    old["tools/mtport/compendium/kb.py"] = "0" * 64
    src, meta = bundle_files(tmp_path, old)
    rc = cli.cmd_from_inputs(src, meta, Path("build/compendium"), tmp_path / "site.tar.gz")
    out = capsys.readouterr().out
    assert rc == cli.STALE == 3
    assert "run scripts/compendium deploy locally" in out
    assert "stale:   tools/mtport/compendium/kb.py: changed" in out
    assert not (checkout / "build").exists() and not (tmp_path / "site.tar.gz").exists()


def test_from_inputs_refuses_a_bad_download_or_a_used_root(tmp_path, checkout, capsys):
    src, meta = bundle_files(tmp_path)
    good = json.loads(meta.read_text(encoding="utf-8"))
    meta.write_text(json.dumps({**good, "sha256": "0" * 64}), encoding="utf-8")
    assert cli.cmd_from_inputs(src, meta, Path("build/compendium"), tmp_path / "s.tgz") == 1
    assert "a partial upload" in capsys.readouterr().err
    meta.write_text(json.dumps(good), encoding="utf-8")
    (checkout / "build/compendium/kb").mkdir(parents=True)
    assert cli.cmd_from_inputs(src, meta, Path("build/compendium"), tmp_path / "s.tgz") == 1
    assert "needs a fresh --root" in capsys.readouterr().err


def test_pack_inputs_and_fingerprint_commands(tmp_path, monkeypatch, capsys):
    monkeypatch.chdir(tmp_path)
    make_inputs(tmp_path)
    assert cli.main(["pack-inputs"]) == 0
    meta = json.loads(cli.INPUTS_META.read_text(encoding="utf-8"))
    assert meta["sha256"] == bundle.sha256(cli.INPUTS.read_bytes())
    assert meta["fingerprint"] == inputs.fingerprint()
    capsys.readouterr()
    assert cli.main(["fingerprint", "--files"]) == 0
    lines = capsys.readouterr().out.splitlines()
    assert lines[-1] == inputs.fingerprint()
    assert any(line.endswith("  tools/mtport/compendium/kb.py") for line in lines)


def test_the_guard_refuses_an_inputs_bundle_and_knowledge_base_files():
    kb_doc = json.dumps({"schema": 1, "kind": "monster", "entities": []}).encode()
    index = json.dumps({"kinds": {}, "relations": [], "schema": 1}).encode()
    read = {"notes/monster.json": kb_doc, "x/index.json": index, "CMakePresets.json": b"{}"}.get
    assert "inputs bundle" in bundle.guard_problem("dist/compendium-inputs.tar.gz")
    assert "inputs bundle" in bundle.guard_problem("compendium-inputs.json")
    assert "knowledge-base file" in bundle.guard_problem("notes/monster.json", read)
    assert "knowledge-base file" in bundle.guard_problem("x/index.json", read)
    assert bundle.guard_problem("CMakePresets.json", read) is None
    assert bundle.guard_problem("tests/golden/device_screens.json", lambda _: b"[1, 2]") is None


# --- the workflow's push path, with a fake gh --------------------------------------------------


def fake_checkout(tmp_path: Path) -> Path:
    """The sparse checkout the push path runs in: scripts/compendium, tools/ (this code) and
    synthetic prose; no work/."""
    co = tmp_path / "checkout"
    (co / "scripts").mkdir(parents=True)
    shutil.copy2(REPO / "scripts" / "compendium", co / "scripts" / "compendium")
    (co / "tools").symlink_to(REPO / "tools", target_is_directory=True)
    fx.write_prose(co / "docs" / "compendium")
    return co


@needs_tools
@pytest.mark.parametrize(
    "case", ["ok", "stale", "partial-upload", "no-inputs", "no-meta", "no-release", "published"]
)
def test_workflow_push_steps_with_a_fake_gh(tmp_path, case):
    hashes = inputs.source_hashes()
    if case == "stale":
        hashes = {**hashes, "tools/mtport/compendium/images.py": "0" * 64}
    src, meta = bundle_files(tmp_path / "upload", hashes)
    data, meta_bytes = src.read_bytes(), meta.read_bytes()
    if case == "partial-upload":
        data = data[: len(data) // 2]
    assets = [
        {"id": 50, "name": "compendium-inputs.tar.gz", "size": len(data)},
        {"id": 51, "name": "compendium-inputs.json", "size": len(meta_bytes)},
        {"id": 52, "name": "compendium-site.tar.gz", "size": 1},
    ]
    if case == "no-inputs":
        assets = assets[2:]
    if case == "no-meta":
        assets = [assets[0], assets[2]]
    release = {"id": 7, "draft": case != "published", "assets": assets}
    listed = None if case == "no-release" else release
    bin_dir = fake_gh(tmp_path, listed, {50: data, 51: meta_bytes, 52: b"x"})
    co = fake_checkout(tmp_path)
    ran, outputs, runner_temp = run_steps(tmp_path, "push", bin_dir, co)
    names = [(n.split()[0], rc) for n, rc, _ in ran]
    log = "\n".join(text for _, _, text in ran)
    summary = tmp_path / "summary.md"
    assert not (co / "work").exists()
    if case == "ok":
        assert names == [("Find", 0), ("Build", 0), ("Check", 0)], log
        assert outputs["deploy"] == "true"
        assert outputs["sha256"] == bundle.sha256(
            (runner_temp / "compendium-site.tar.gz").read_bytes()
        )
        assert (runner_temp / "site" / "index.html").is_file()
        assert (runner_temp / "site" / "img" / "manifest.json").is_file()
        return
    if case == "stale":
        # skipped, not failed: a warning, a job summary, and no site for the Pages steps
        assert names == [("Find", 0), ("Build", 0)], log
        assert outputs == {"deploy": "false"}
        assert "::warning::" in log and "Run scripts/compendium deploy locally" in log
        text = summary.read_text(encoding="utf-8")
        assert "Compendium not deployed" in text
        assert "tools/mtport/compendium/images.py: changed" in text
        assert not (runner_temp / "site").exists()
        return
    if case in ("no-inputs", "no-meta", "no-release"):
        # no inputs bundle yet (before the first deploy after #248): skipped, not failed
        assert names == [("Find", 0)], log
        assert outputs == {"inputs": "missing"}
        assert "::warning::" in log
        assert "scripts/compendium build && scripts/compendium deploy on the Mac once" in log
        text = summary.read_text(encoding="utf-8")
        assert "Compendium not deployed: no inputs bundle yet" in text
        assert not (runner_temp / "compendium-inputs.tar.gz").exists()
        assert not (runner_temp / "site").exists()
        return
    assert names[-1][1] != 0
    expect = {
        "partial-upload": "a partial upload",
        "published": "must sit on a draft release",
    }[case]
    assert expect in log, log
    assert not (runner_temp / "site").exists()
