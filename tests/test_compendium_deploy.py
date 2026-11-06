"""Packing and deploying the compendium (issues #226 and #248, ADR 0121), with ``gh`` stubbed:
nothing here talks to GitHub. Also the deploy workflow's shape and its shell steps against a
fake ``gh``, on both of its triggers (test_compendium_deploy_inputs.py has the inputs bundle)."""

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
    kb = root / cli.KB_DIR
    kb.mkdir(parents=True, exist_ok=True)
    (kb / "index.json").write_text('{"schema": 1, "kinds": {}, "relations": []}', encoding="utf-8")
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
    assert "inputs: extraction fingerprint" in out
    assert cli.BUNDLE.is_file() and cli.INPUTS.is_file() and cli.INPUTS_META.is_file()


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
        ["release", "upload"],
        ["release", "upload"],
        ["workflow", "run"],
    ]
    assert gh.calls[0][2] == "repos/owner/repo/releases"
    create = gh.calls[1]
    assert create[2] == "compendium-site" and "--draft" in create
    # the inputs bundle, then its metadata, then the site
    for call, path in zip(gh.calls[2:5], (cli.INPUTS, cli.INPUTS_META, cli.BUNDLE), strict=True):
        assert call == [
            "release", "upload", "compendium-site", str(path), "--repo", "owner/repo",
            "--clobber",
        ]  # fmt: skip
    assert gh.calls[5] == [
        "workflow", "run", "compendium.yml", "--repo", "owner/repo", "--ref", "main",
        "-f", f"sha256={digest}",
    ]  # fmt: skip


def test_deploy_reuses_the_draft(site_dir):
    gh = FakeGh([{"id": 2, "tag_name": "compendium-site", "draft": True}])
    assert deploy(gh)[0] == 0
    assert [c[1] for c in gh.calls] == ["--paginate", "upload", "upload", "upload", "run"]


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

PUSH_PATHS = [
    "tools/mtport/compendium/**",
    "tools/mtport/prose_lint.py",
    "tools/mtport/htmlpage.py",
    "docs/compendium/**",
    "scripts/compendium",
    ".github/workflows/compendium.yml",
]


def workflow() -> str:
    return WORKFLOW.read_text(encoding="utf-8")


def code_lines(text: str) -> str:
    return "\n".join(line for line in text.splitlines() if not line.lstrip().startswith("#"))


def steps(text: str) -> list[str]:
    """The workflow's steps, each as its text (from ``- name:`` to the next step)."""
    body = text[text.index("    steps:\n") :]
    parts = re.split(r"\n(?=      - name: )", body)
    return [p for p in parts if p.lstrip().startswith("- name: ")]


def step(text: str, name: str) -> str:
    hits = [s for s in steps(text) if s.lstrip().startswith(f"- name: {name}")]
    assert len(hits) == 1, name
    return hits[0]


def push_paths(text: str) -> list[str]:
    on = text[text.index("\non:") : text.index("\npermissions:")]
    block = on[on.index("    paths:\n") : on.index("  workflow_dispatch:")]
    return re.findall(r'^      - "([^"]+)"$', block, re.M)


def test_workflow_triggers_push_to_main_with_path_filters_and_dispatch():
    text = workflow()
    on = text[text.index("\non:") : text.index("\npermissions:")]
    assert "workflow_dispatch:" in on and "sha256:" in on
    assert re.search(r"^  push:\n    branches: \[main\]\n", on, re.M)
    for trigger in ("pull_request", "schedule", "workflow_run", "release:", "tags:"):
        assert trigger not in on
    assert push_paths(text) == PUSH_PATHS
    perms = text[text.index("\npermissions:") : text.index("\nconcurrency:")]
    for p in ("contents: write", "pages: write", "id-token: write"):
        assert p in perms
    assert "if: github.ref == 'refs/heads/main'" in text
    assert "cancel-in-progress: false" in text


def test_workflow_dispatch_path_checks_out_and_runs_nothing():
    """Only the push path checks out and runs repository code; dispatch runs as before #248."""
    text = workflow()
    push_only = "if: github.event_name == 'push'"
    for s in steps(text):
        code = code_lines(s)
        runs = re.search(r"^\s*(scripts/|python|\./)", code, re.M)
        if "actions/checkout" in code or runs or "mtport" in code:
            assert push_only in s, s.splitlines()[0]
    checkout = step(text, "Check out")
    assert "lfs: false" in checkout and "persist-credentials: false" in checkout
    assert "sparse-checkout-cone-mode: false" in checkout
    sparse = re.findall(r"^            (\S+)$", checkout, re.M)
    assert sparse == ["/tools/mtport/", "/docs/compendium/", "/scripts/compendium"]
    code = code_lines(text).lower()
    for banned in ("work/ipa", "pip ", "setup-python", "setup-uv", "lfs: true", "lfs pull"):
        assert banned not in code
    assert "retention-days: 1" in text
    assert "Accept: application/octet-stream" in text
    assert re.search(r"gh api --paginate \"repos/\$REPO/releases\"", text)
    # a stale inputs bundle skips the deploy steps; it does not fail the run
    for name in ("Check and unpack", "Upload the Pages artifact", "Deploy to GitHub Pages"):
        assert "if: steps.build.outputs.deploy != 'false'" in step(text, name)
    build = step(text, "Build the site")
    assert "::warning::" in build and "GITHUB_STEP_SUMMARY" in build
    assert "run scripts/compendium deploy locally" in build.lower()


def test_push_paths_cover_every_site_module_outside_the_fingerprint():
    """A file the site build imports, but the fingerprint does not hash, must trigger a push
    run; a fingerprinted file needs no trigger (its change can only skip the deploy)."""
    from mtport.compendium import inputs

    site_roots = ["mtport.compendium.cli", "mtport.compendium.site", "mtport.prose_lint"]
    site_roots += [
        f"mtport.compendium.sections.{p.stem}"
        for p in (REPO / "tools/mtport/compendium/sections").glob("*.py")
    ]
    site = {p.relative_to(REPO).as_posix() for p in inputs.source_files(roots=site_roots)}
    hashed = set(inputs.source_hashes())
    patterns = push_paths(workflow())

    def covered(path: str) -> bool:
        return any(
            path.startswith(p[:-2]) if p.endswith("/**") else path == p for p in patterns
        )  # fmt: skip

    assert [p for p in sorted(site - hashed) if not covered(p)] == []
    # what the site reads besides code: the prose and the templates under the compendium tree
    assert covered("docs/compendium/pages/index.md")
    assert covered("tools/mtport/compendium/sections/_worldmap.js")


def test_workflow_actions_are_pinned_to_the_shas_ci_uses():
    uses = re.findall(r"uses: (\S+)(.*)", workflow())
    assert {u.split("@")[0] for u, _ in uses} == {
        "actions/checkout",
        "actions/upload-pages-artifact",
        "actions/deploy-pages",
    }
    for ref, comment in uses:
        assert re.fullmatch(r"[\w./-]+@[0-9a-f]{40}", ref), ref
        assert re.fullmatch(r" # v\d+\.\d+\.\d+", comment), comment
    ci = (REPO / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    checkout = [u + c for u, c in uses if u.startswith("actions/checkout@")]
    assert checkout and all(f"uses: {c}" in ci for c in checkout)


def run_block(text: str) -> str:
    """The ``run: |`` script of one step, dedented."""
    lines = text.splitlines()
    for i, line in enumerate(lines):
        if line.strip() == "run: |":
            indent = len(line) - len(line.lstrip()) + 2
            body = []
            for nxt in lines[i + 1 :]:
                if nxt.strip() and len(nxt) - len(nxt.lstrip()) < indent:
                    break
                body.append(nxt[indent:])
            return "\n".join(body) + "\n"
    raise AssertionError("no run block")


NEEDS = ("bash", "jq", "tar", "sha256sum")
needs_tools = pytest.mark.skipif(
    os.name == "nt" or not all(shutil.which(t) for t in NEEDS), reason="needs " + ", ".join(NEEDS)
)


def fake_gh(tmp_path: Path, release: dict | None, assets: dict[int, bytes]) -> Path:
    """A ``gh`` that lists ``release`` and serves ``assets`` by id."""
    listing = tmp_path / "listing.jsonl"
    listing.write_text("" if release is None else json.dumps(release) + "\n", encoding="utf-8")
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    cases = []
    for aid, data in assets.items():
        f = tmp_path / f"asset-{aid}"
        f.write_bytes(data)
        cases.append(f'  *releases/assets/{aid}) cat "{f}" ;;\n')
    gh = bin_dir / "gh"
    gh.write_text(
        "#!/usr/bin/env bash\n"
        'case "$*" in\n' + "".join(cases) + f'  *--paginate*) cat "{listing}" ;;\n'
        '  *) echo "unexpected gh $*" >&2; exit 9 ;;\n'
        "esac\n",
        encoding="utf-8",
    )
    gh.chmod(0o755)
    return bin_dir


def run_steps(
    tmp_path: Path, event: str, bin_dir: Path, cwd: Path, want: str = ""
) -> tuple[list[tuple[str, int, str]], dict[str, str], Path]:
    """Run the workflow's shell steps as a runner would for ``event``, honouring each step's
    ``if:`` on the event and on the build step's ``deploy`` output. Returns (step, status,
    log) for each step run, the build step's outputs and RUNNER_TEMP."""
    text = workflow()
    runner_temp = tmp_path / "runner-temp"
    runner_temp.mkdir()
    out_file, summary = tmp_path / "github-output", tmp_path / "summary.md"
    env = {
        **os.environ,
        "PATH": f"{bin_dir}{os.pathsep}{os.environ['PATH']}",
        "RUNNER_TEMP": str(runner_temp),
        "RELEASE_TAG": "compendium-site",
        "ASSET": "compendium-site.tar.gz",
        "INPUTS": "compendium-inputs.tar.gz",
        "INPUTS_META": "compendium-inputs.json",
        "REPO": "owner/repo",
        "GH_TOKEN": "x",
        "EVENT": event,
        "GITHUB_STEP_SUMMARY": str(summary),
    }
    outputs: dict[str, str] = {}
    ran = []
    for s in steps(text):
        name = s.lstrip()[len("- name: ") :].splitlines()[0]
        if "run: |" not in s:
            continue
        if "if: github.event_name == 'push'" in s and event != "push":
            continue
        if "if: steps.build.outputs.deploy != 'false'" in s and outputs.get("deploy") == "false":
            continue
        out_file.write_text("", encoding="utf-8")
        wanted = want if event == "workflow_dispatch" else outputs.get("sha256", "")
        res = subprocess.run(
            ["bash", "-e", "-c", run_block(s)],
            cwd=cwd,
            env={**env, "GITHUB_OUTPUT": str(out_file), "WANT": wanted},
            capture_output=True,
            text=True,
        )
        ran.append((name, res.returncode, res.stdout + res.stderr))
        if "id: build" in s:
            for line in out_file.read_text(encoding="utf-8").splitlines():
                k, _, v = line.partition("=")
                outputs[k] = v
        if res.returncode:
            break
    return ran, outputs, runner_temp


@needs_tools
@pytest.mark.parametrize(
    "case", ["ok", "no-draft", "published", "wrong-sha", "audio", "bad-sha-input", "no-asset"]
)
def test_workflow_dispatch_steps_with_a_fake_gh(tmp_path, case):
    """The dispatch path's shell steps, run as a runner would, with ``gh`` stubbed."""
    s = make_site(tmp_path / "src", {"audio/bgm.ogg": b"o"} if case == "audio" else None)
    names = [rel for rel, _ in bundle.site_files(s)]
    data = bundle.pack(bundle.site_files(s))
    asset_name = "other.tar.gz" if case == "no-asset" else "compendium-site.tar.gz"
    release = {
        "id": 7,
        "draft": case != "published",
        "assets": [{"id": 42, "name": asset_name, "size": len(data)}],
    }
    bin_dir = fake_gh(tmp_path, None if case == "no-draft" else release, {42: data})
    work = tmp_path / "work"
    work.mkdir()
    want = {"wrong-sha": "0" * 64, "bad-sha-input": "x; rm -rf /"}.get(case, bundle.sha256(data))
    ran, _, runner_temp = run_steps(tmp_path, "workflow_dispatch", bin_dir, work, want)
    site = runner_temp / "site"
    if case == "ok":
        assert [(n.split()[0], rc) for n, rc, _ in ran] == [("Find", 0), ("Check", 0)], ran
        got = sorted(p.relative_to(site).as_posix() for p in site.rglob("*") if p.is_file())
        assert got == sorted(names)
        assert list(work.iterdir()) == []  # nothing written into the workspace
        return
    _, rc, log = ran[-1]
    assert rc != 0
    expect = {
        "no-draft": "expected one release",
        "published": "must sit on a draft release",
        "wrong-sha": "not the one the deploy printed",
        "audio": "file type the site never publishes",
        "bad-sha-input": "is not a SHA-256",
        "no-asset": "has no asset compendium-site.tar.gz",
    }[case]
    assert expect in log, log
    assert not site.exists()
