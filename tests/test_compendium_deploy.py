"""Packing and deploying the compendium (issue #226, ADR 0121), with ``gh`` stubbed: nothing
here talks to GitHub. Also the deploy workflow's shape and its shell steps against a fake
``gh``."""

from __future__ import annotations

import io
import json
import os
import re
import shutil
import subprocess
import tarfile
import threading
import urllib.error
import urllib.request
from pathlib import Path

import pytest

from mtport.compendium import bundle, cli
from repo_paths import REPO

WORKFLOW = REPO / ".github/workflows/compendium.yml"


def make_site(root: Path, extra: dict[str, bytes] | None = None) -> Path:
    s = root / cli.SITE_DIR
    files = {
        "index.html": b"<!doctype html><p>home</p>",
        "404.html": b"<!doctype html><p>not found</p>",
        "site.css": b"body{}",
        "robots.txt": b"User-agent: *\n",
        "monster/1.html": b"<p>m</p>",
        "img/monster/1/move.png": b"\x89PNG" + b"x" * 5000,
        "img/manifest.json": b"{}",
        ".mtport-compendium-site": b"compendium site\n",
        **(extra or {}),
    }
    for rel, data in files.items():
        f = s / rel
        f.parent.mkdir(parents=True, exist_ok=True)
        f.write_bytes(data)
    return s


# --- bundle -------------------------------------------------------------------------------------


def test_pack_is_deterministic_and_leaves_out_hidden_files(tmp_path):
    s = make_site(tmp_path)
    files = bundle.site_files(s)
    assert ".mtport-compendium-site" not in {rel for rel, _ in files}
    a = bundle.pack(files)
    os.utime(s / "index.html", (1_000_000, 1_000_000))
    b = bundle.pack(bundle.site_files(s))
    assert a == b
    with tarfile.open(fileobj=io.BytesIO(a), mode="r:gz") as tar:
        members = tar.getmembers()
        assert [m.name for m in members] == sorted(rel for rel, _ in files)
        assert all(m.isfile() and m.mtime == 0 and m.uid == 0 and m.mode == 0o644 for m in members)
        assert tar.extractfile("index.html").read() == b"<!doctype html><p>home</p>"


def test_bundle_refuses_what_the_site_never_publishes(tmp_path):
    s = make_site(tmp_path, {"audio/bgm.ogg": b"o", "raw/mob.dat": b"d"})
    (s / "404.html").unlink()
    (s / "link.html").symlink_to(s / "index.html")
    problems = "\n".join(bundle.bundle_problems(bundle.site_files(s)))
    assert "audio/bgm.ogg: file type not published" in problems
    assert "raw/mob.dat: file type not published" in problems
    assert "404.html: missing" in problems
    assert "link.html: a symlink" in problems
    assert bundle.bundle_problems(bundle.site_files(make_site(tmp_path / "ok"))) == []


def test_size_report(tmp_path):
    r = bundle.size_report(bundle.site_files(make_site(tmp_path)))
    assert r.count == 7
    assert r.largest[0] == ("img/monster/1/move.png", 5004)
    assert r.by_dir[0][0] == "img/monster/"
    text = "\n".join(r.lines())
    assert "size: 7 files" in text and "largest files" in text
    assert "over the GitHub Pages limit" not in text


def test_find_release_by_listing():
    releases = [
        {"id": 1, "tag_name": "v1.0", "draft": False},
        {"id": 2, "tag_name": "compendium-site", "draft": True},
    ]
    assert bundle.find_release(releases)["id"] == 2
    assert bundle.find_release(releases[:1]) is None
    with pytest.raises(bundle.ReleaseError, match="published"):
        bundle.find_release([{"id": 3, "tag_name": "compendium-site", "draft": False}])
    with pytest.raises(bundle.ReleaseError, match="keep one"):
        bundle.find_release(releases + [{"id": 4, "tag_name": "compendium-site", "draft": True}])


def test_preview_serves_the_sub_path_and_the_404_page(site_dir):
    server = cli.preview_server(0)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    base = f"http://127.0.0.1:{server.server_address[1]}/maplethief-port/"
    try:
        with opener.open(base) as r:
            assert r.status == 200 and b"home" in r.read()
        with opener.open(base + "monster/1.html") as r:
            assert r.status == 200
        with pytest.raises(urllib.error.HTTPError) as exc:
            opener.open(base + "no/such/page.html")
        assert exc.value.code == 404
        assert exc.value.read() == b"<!doctype html><p>not found</p>"
    finally:
        server.shutdown()
        server.server_close()


# --- deploy -------------------------------------------------------------------------------------


class FakeGh:
    def __init__(self, releases: list[dict]):
        self.releases = releases
        self.calls: list[list[str]] = []

    def __call__(self, args):
        self.calls.append(list(args))
        if args[0] == "api":
            return "".join(json.dumps(r) + "\n" for r in self.releases)
        return ""


@pytest.fixture
def site_dir(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    return make_site(tmp_path)


def deploy(gh, *, check=0, dry_run=False, yes=True, answer=True):
    asked = []

    def ask(prompt):
        asked.append(prompt)
        return answer

    rc = cli.cmd_deploy(
        "owner/repo", dry_run, yes, check=lambda app: check, gh=gh, ask=ask
    )  # fmt: skip
    return rc, asked


def test_deploy_refuses_when_check_fails(site_dir, capsys):
    gh = FakeGh([])
    rc, asked = deploy(gh, check=1)
    assert rc == 1 and gh.calls == [] and asked == []
    assert "check failed; nothing is published" in capsys.readouterr().err
    assert not cli.BUNDLE.exists()


def test_dry_run_calls_no_gh(site_dir, capsys):
    gh = FakeGh([])
    rc, asked = deploy(gh, dry_run=True)
    assert rc == 0 and gh.calls == [] and asked == []
    out = capsys.readouterr().out
    assert "dry run; gh is not called" in out
    assert "gh release upload compendium-site" in out
    assert "size: 7 files" in out and "sha256" in out
    assert cli.BUNDLE.is_file()


def test_deploy_asks_and_stops_when_declined(site_dir, capsys):
    gh = FakeGh([])
    rc, asked = deploy(gh, yes=False, answer=False)
    assert rc == 1 and gh.calls == []
    assert len(asked) == 1 and "https://armaatus.github.io/maplethief-port/" in asked[0]
    assert "nothing is published" in capsys.readouterr().out


def test_deploy_creates_the_draft_uploads_and_runs_the_workflow(site_dir, capsys):
    gh = FakeGh([{"id": 9, "tag_name": "v1.0", "draft": False}])
    rc, _ = deploy(gh)
    assert rc == 0
    digest = bundle.sha256(cli.BUNDLE.read_bytes())
    assert [c[:2] for c in gh.calls] == [
        ["api", "--paginate"],
        ["release", "create"],
        ["release", "upload"],
        ["workflow", "run"],
    ]
    assert gh.calls[0][2] == "repos/owner/repo/releases"
    create = gh.calls[1]
    assert create[2] == "compendium-site" and "--draft" in create
    assert gh.calls[2] == [
        "release", "upload", "compendium-site", str(cli.BUNDLE), "--repo", "owner/repo",
        "--clobber",
    ]  # fmt: skip
    assert gh.calls[3] == [
        "workflow", "run", "compendium.yml", "--repo", "owner/repo", "--ref", "main",
        "-f", f"sha256={digest}",
    ]  # fmt: skip


def test_deploy_reuses_the_draft(site_dir):
    gh = FakeGh([{"id": 2, "tag_name": "compendium-site", "draft": True}])
    assert deploy(gh)[0] == 0
    assert [c[1] for c in gh.calls] == ["--paginate", "upload", "run"]


def test_deploy_refuses_a_published_release(site_dir, capsys):
    gh = FakeGh([{"id": 2, "tag_name": "compendium-site", "draft": False}])
    assert deploy(gh)[0] == 1
    assert len(gh.calls) == 1  # only the listing
    assert "must sit on a draft release" in capsys.readouterr().err


@pytest.mark.skipif(os.name == "nt", reason="a shell-script stub")
def test_run_gh_uses_mtport_gh(tmp_path, monkeypatch):
    log = tmp_path / "calls.txt"
    stub = tmp_path / "gh"
    stub.write_text(
        "#!/bin/sh\n"
        f'printf "%s\\n" "$*" >> "{log}"\n'
        """echo '{"id": 5, "tag_name": "compendium-site", "draft": true, "name": "x"}'\n""",
        encoding="utf-8",
    )
    stub.chmod(0o755)
    monkeypatch.setenv("MTPORT_GH", str(stub))
    assert cli.list_releases("o/r") == [
        {"id": 5, "tag_name": "compendium-site", "draft": True, "name": "x"}
    ]
    assert log.read_text(encoding="utf-8").startswith("api --paginate repos/o/r/releases --jq ")


# --- the workflow -------------------------------------------------------------------------------


def workflow() -> str:
    return WORKFLOW.read_text(encoding="utf-8")


def test_workflow_is_manual_main_only_and_builds_nothing():
    text = workflow()
    on = text[text.index("\non:") : text.index("\npermissions:")]
    assert "workflow_dispatch:" in on
    for trigger in ("pull_request", "push", "schedule", "workflow_run", "release:"):
        assert trigger not in on
    perms = text[text.index("\npermissions:") : text.index("\nconcurrency:")]
    for p in ("contents: write", "pages: write", "id-token: write"):
        assert p in perms
    assert "if: github.ref == 'refs/heads/main'" in text
    # nothing checked out, so no LFS and nothing built from the IPA
    code = "\n".join(line for line in text.splitlines() if not line.lstrip().startswith("#"))
    for banned in (
        "actions/checkout",
        "lfs",
        "run: scripts/",
        "mtport",
        "work/ipa",
        "pip ",
        "python",
    ):
        assert banned not in code.lower()
    assert "retention-days: 1" in text
    assert "Accept: application/octet-stream" in text
    assert re.search(r"gh api --paginate \"repos/\$REPO/releases\"", text)


def test_workflow_actions_are_pinned_to_shas():
    uses = re.findall(r"uses: (\S+)(.*)", workflow())
    assert {u.split("@")[0] for u, _ in uses} == {
        "actions/upload-pages-artifact",
        "actions/deploy-pages",
    }
    for ref, comment in uses:
        assert re.fullmatch(r"[\w./-]+@[0-9a-f]{40}", ref), ref
        assert re.fullmatch(r" # v\d+\.\d+\.\d+", comment), comment


def run_blocks(text: str) -> list[str]:
    """The ``run: |`` scripts of the workflow, dedented."""
    blocks, lines = [], text.splitlines()
    for i, line in enumerate(lines):
        if line.strip() == "run: |":
            indent = len(line) - len(line.lstrip()) + 2
            body = []
            for nxt in lines[i + 1 :]:
                if nxt.strip() and len(nxt) - len(nxt.lstrip()) < indent:
                    break
                body.append(nxt[indent:])
            blocks.append("\n".join(body) + "\n")
    return blocks


NEEDS = ("bash", "jq", "tar", "sha256sum")


@pytest.mark.skipif(
    os.name == "nt" or not all(shutil.which(t) for t in NEEDS), reason="needs " + ", ".join(NEEDS)
)
@pytest.mark.parametrize(
    "case", ["ok", "no-draft", "published", "wrong-sha", "audio", "bad-sha-input"]
)
def test_workflow_steps_with_a_fake_gh(tmp_path, case):
    """The workflow's two shell steps, run as a runner would, with ``gh`` stubbed."""
    s = make_site(tmp_path / "src", {"audio/bgm.ogg": b"o"} if case == "audio" else None)
    names = [rel for rel, _ in bundle.site_files(s)]
    data = bundle.pack(bundle.site_files(s))
    asset = tmp_path / "asset.tar.gz"
    asset.write_bytes(data)
    release = {
        "id": 7,
        "draft": case != "published",
        "assets": [{"id": 42, "name": "compendium-site.tar.gz", "size": len(data)}],
    }
    listing = "" if case == "no-draft" else json.dumps(release) + "\n"
    (tmp_path / "listing.jsonl").write_text(listing, encoding="utf-8")
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    gh = bin_dir / "gh"
    gh.write_text(
        "#!/usr/bin/env bash\n"
        'case "$*" in\n'
        f'  *releases/assets/42*) cat "{asset}" ;;\n'
        f'  *--paginate*) cat "{tmp_path / "listing.jsonl"}" ;;\n'
        '  *) echo "unexpected gh $*" >&2; exit 9 ;;\n'
        "esac\n",
        encoding="utf-8",
    )
    gh.chmod(0o755)
    runner_temp = tmp_path / "runner-temp"
    runner_temp.mkdir()
    work = tmp_path / "work"
    work.mkdir()
    want = {"wrong-sha": "0" * 64, "bad-sha-input": "x; rm -rf /"}.get(case, bundle.sha256(data))
    env = {
        **os.environ,
        "PATH": f"{bin_dir}{os.pathsep}{os.environ['PATH']}",
        "RUNNER_TEMP": str(runner_temp),
        "RELEASE_TAG": "compendium-site",
        "ASSET": "compendium-site.tar.gz",
        "REPO": "owner/repo",
        "GH_TOKEN": "x",
        "WANT": want,
    }
    statuses = []
    for script in run_blocks(workflow()):
        res = subprocess.run(
            ["bash", "-e", "-c", script], cwd=work, env=env, capture_output=True, text=True
        )
        statuses.append((res.returncode, res.stdout + res.stderr))
        if res.returncode:
            break
    if case == "ok":
        assert [rc for rc, _ in statuses] == [0, 0], statuses
        got = sorted(p.relative_to(work / "site").as_posix() for p in (work / "site").rglob("*")
                     if p.is_file())  # fmt: skip
        assert got == sorted(names)
        return
    rc, log = statuses[-1]
    assert rc != 0
    expect = {
        "no-draft": "expected one release",
        "published": "must sit on a draft release",
        "wrong-sha": "not the one the deploy printed",
        "audio": "file type the site never publishes",
        "bad-sha-input": "is not a SHA-256",
    }[case]
    assert expect in log, log
    assert not (work / "site").exists() or case not in ("wrong-sha", "audio")
