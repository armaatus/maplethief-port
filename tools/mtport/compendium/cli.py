"""``scripts/compendium``: build, check, preview and deploy the compendium site (issue #226,
ADR 0121).

Run from the repository root (``scripts/compendium`` does that)::

    build  [--app DIR]             knowledge base, images, site (build/compendium/)
    check  [--app DIR]             every step's check: the knowledge base and the images
                                   against a rebuild, the site's links, completeness, prose
                                   lint and fresh-build equality, the bundle's file types,
                                   the size report and the guard
    serve  [--port N]              preview at http://127.0.0.1:N/maplethief-port/, the
                                   sub-path Pages serves from, with the 404 page
    size                           the size report: total, per directory, largest files
    pack   [--out FILE]            write the bundle and print its size and SHA-256
    deploy [--dry-run] [--yes]     check, pack, confirm, upload the bundle to the draft
           [--repo OWNER/NAME]     release and run the deploy workflow
    guard  [--repo-dir DIR]        tracked files that look like compendium output

``deploy`` refuses when ``check`` fails. ``--dry-run`` stops before ``gh`` is called and prints
the commands it would run. ``MTPORT_GH`` names the ``gh`` to run (the tests stub it).
"""

from __future__ import annotations

import argparse
import functools
import http.server
import json
import os
import shlex
import subprocess
import sys
from collections.abc import Callable, Sequence
from pathlib import Path

from mtport.compendium import bundle
from mtport.compendium import site as sitegen

REPO_ROOT = Path(__file__).resolve().parents[3]
COMPENDIUM = Path("build/compendium")
SITE_DIR = COMPENDIUM / "site"
BUNDLE = COMPENDIUM / bundle.BUNDLE_NAME
SERVE_DIR = COMPENDIUM / "serve"
SUBPATH = "maplethief-port"
GITHUB_REPO = "armaatus/maplethief-port"
WORKFLOW = "compendium.yml"
APP = Path("work/ipa/Payload/MapleThief.app")
RELEASE_TITLE = "MapleThief Compendium site bundle"
RELEASE_NOTES = (
    "The packed MapleThief Compendium site (scripts/compendium deploy), deployed to GitHub Pages"
    " by .github/workflows/compendium.yml. Keep this release a draft: publishing it would make"
    " the bundle public outside the site (ADR 0121)."
)


def _need_app(app: Path) -> bool:
    if app.is_dir():
        return True
    print(
        f"error: {app}: no such .app directory. The compendium is built from your own copy:"
        " run scripts/bootstrap first.",
        file=sys.stderr,
    )
    return False


# --- build and check ----------------------------------------------------------------------------


def cmd_build(app: Path) -> int:
    from mtport.compendium import images, kb

    if not _need_app(app):
        return 2
    steps: list[tuple[str, Callable[[], int]]] = [
        ("knowledge base", lambda: kb.main(["build", "--app", str(app)])),
        ("images", lambda: images.main(["build", "--app", str(app)])),
        ("site", lambda: sitegen.main(["build"])),
    ]
    for name, step in steps:
        print(f"== build: {name}", flush=True)
        if step() != 0:
            print(f"error: the {name} step failed", file=sys.stderr)
            return 1
    return 0


def images_rebuild_diff(app: Path, img: Path) -> list[str]:
    """How ``img`` differs from a fresh images build (byte for byte)."""
    from mtport.compendium import images

    ex, _ = images.build(app)
    want = dict(ex.files)
    want[images.MANIFEST] = images.manifest_bytes(ex)
    problems = []
    for rel in sorted(want):
        f = img / rel
        if not f.is_file():
            problems.append(f"img/{rel}: missing")
        elif f.read_bytes() != want[rel]:
            problems.append(f"img/{rel}: differs from a fresh build")
    if img.is_dir():
        on_disk = {p.relative_to(img).as_posix() for p in img.rglob("*") if p.is_file()}
        problems += [f"img/{rel}: not part of a fresh build" for rel in sorted(on_disk - set(want))]
    return problems


def cmd_check(app: Path) -> int:
    from mtport.compendium import images, kb

    if not _need_app(app):
        return 2
    failed: list[str] = []

    def step(name: str, fn: Callable[[], int | list[str]]) -> None:
        print(f"== check: {name}", flush=True)
        res = fn()
        if isinstance(res, list):
            for line in res:
                print(f"error: {line}")
            print(f"{name}: {len(res)} problems")
            res = 1 if res else 0
        if res != 0:
            failed.append(name)

    step("knowledge base", lambda: kb.main(["check", "--app", str(app)]))
    step("images", lambda: images.main(["check", "--app", str(app)]))
    step("images rebuild", lambda: images_rebuild_diff(app, SITE_DIR / sitegen.IMG_DIR))
    step("site", lambda: sitegen.main(["check"]))
    step("bundle", lambda: bundle.bundle_problems(bundle.site_files(SITE_DIR)))
    step("guard", lambda: bundle.guard(Path(".")))
    print("== check: size")
    report = bundle.size_report(bundle.site_files(SITE_DIR))
    for line in report.lines():
        print(line)
    if report.total > bundle.PAGES_LIMIT:
        failed.append("size")
    if failed:
        print(f"check: FAILED ({', '.join(failed)})")
        return 1
    print("check: OK")
    return 0


# --- preview ------------------------------------------------------------------------------------


class _Preview(http.server.SimpleHTTPRequestHandler):
    """Serves the site as Pages does: under ``/maplethief-port/``, with ``404.html`` for a
    missing page."""

    site: Path = SITE_DIR

    def send_error(self, code, message=None, explain=None):  # noqa: D102 (http.server's API)
        page = self.site / sitegen.NOT_FOUND
        if code != 404 or not page.is_file():
            return super().send_error(code, message, explain)
        body = page.read_bytes()
        self.send_response(404)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)
        return None


def preview_server(port: int) -> http.server.ThreadingHTTPServer:
    """A server for the site at ``/maplethief-port/`` on 127.0.0.1 (port 0: any free one)."""
    SERVE_DIR.mkdir(parents=True, exist_ok=True)
    link = SERVE_DIR / SUBPATH
    if link.is_symlink() or link.exists():
        link.unlink()
    link.symlink_to(Path("..") / "site")
    handler = functools.partial(_Preview, directory=str(SERVE_DIR))
    return http.server.ThreadingHTTPServer(("127.0.0.1", port), handler)


def cmd_serve(port: int) -> int:
    if not (SITE_DIR / "index.html").is_file():
        print(f"error: {SITE_DIR} has no site; run scripts/compendium build", file=sys.stderr)
        return 1
    with preview_server(port) as httpd:
        port = httpd.server_address[1]
        print(f"serving http://127.0.0.1:{port}/{SUBPATH}/ (Ctrl-C stops)", flush=True)
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            pass
    return 0


# --- bundle and deploy --------------------------------------------------------------------------


def cmd_size() -> int:
    for line in bundle.size_report(bundle.site_files(SITE_DIR)).lines():
        print(line)
    return 0


def make_bundle(out: Path) -> tuple[bytes, list[str]] | None:
    files = bundle.site_files(SITE_DIR)
    problems = bundle.bundle_problems(files)
    if problems:
        for line in problems:
            print(f"error: {line}", file=sys.stderr)
        return None
    data = bundle.pack(files)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(data)
    lines = bundle.size_report(files).lines()
    lines.append(f"bundle: {out}: {bundle.human(len(data))} packed, sha256 {bundle.sha256(data)}")
    return data, lines


def cmd_pack(out: Path) -> int:
    made = make_bundle(out)
    if made is None:
        return 1
    for line in made[1]:
        print(line)
    return 0


def _gh() -> list[str]:
    return [os.environ.get("MTPORT_GH", "gh")]


def run_gh(args: Sequence[str]) -> str:
    res = subprocess.run(_gh() + list(args), capture_output=True, text=True)
    if res.returncode != 0:
        raise bundle.ReleaseError(
            f"gh {' '.join(args)} failed ({res.returncode}): {res.stderr.strip()}"
        )
    return res.stdout


def list_releases(repo: str, gh: Callable[[Sequence[str]], str] = run_gh) -> list[dict]:
    out = gh(
        [
            "api",
            "--paginate",
            f"repos/{repo}/releases",
            "--jq",
            ".[] | {id, tag_name, draft, name} | @json",
        ]
    )
    return [json.loads(line) for line in out.splitlines() if line.strip()]


def deploy_commands(repo: str, digest: str, exists: bool, ref: str = "main") -> list[list[str]]:
    """The ``gh`` calls that publish ``BUNDLE`` (without the leading ``gh``)."""
    cmds = []
    if not exists:
        cmds.append(
            [
                "release",
                "create",
                bundle.RELEASE_TAG,
                "--repo",
                repo,
                "--draft",
                "--title",
                RELEASE_TITLE,
                "--notes",
                RELEASE_NOTES,
            ]
        )
    cmds.append(["release", "upload", bundle.RELEASE_TAG, str(BUNDLE), "--repo", repo, "--clobber"])
    cmds.append(
        ["workflow", "run", WORKFLOW, "--repo", repo, "--ref", ref, "-f", f"sha256={digest}"]
    )
    return cmds


def _ask(prompt: str) -> bool:
    if not sys.stdin.isatty():
        print("error: no terminal to confirm on; pass --yes to publish", file=sys.stderr)
        return False
    return input(prompt).strip().lower() in ("y", "yes")


def cmd_deploy(
    repo: str,
    dry_run: bool,
    yes: bool,
    app: Path = APP,
    check: Callable[[Path], int] = cmd_check,
    gh: Callable[[Sequence[str]], str] = run_gh,
    ask: Callable[[str], bool] = _ask,
) -> int:
    print("== deploy: scripts/compendium check", flush=True)
    if check(app) != 0:
        print("error: scripts/compendium check failed; nothing is published", file=sys.stderr)
        return 1
    made = make_bundle(BUNDLE)
    if made is None:
        return 1
    data, lines = made
    digest = bundle.sha256(data)
    print(f"== deploy: this publishes {SITE_DIR} to {sitegen.BASE_URL}")
    for line in lines:
        print(line)
    print(
        f"deploy: the bundle goes to the draft release {bundle.RELEASE_TAG!r} of {repo}, then"
        f" {WORKFLOW} deploys it to GitHub Pages"
    )
    if dry_run:
        print("deploy: dry run; gh is not called. It would run:")
        for cmd in [["api", "--paginate", f"repos/{repo}/releases"]] + deploy_commands(
            repo, digest, exists=False
        ):
            print("  " + shlex.join(["gh", *cmd]))
        print("  (release create only when the draft does not exist yet)")
        return 0
    if not yes and not ask(f"Publish {bundle.human(len(data))} to {sitegen.BASE_URL}? [y/N] "):
        print("deploy: not confirmed; nothing is published")
        return 1
    try:
        release = bundle.find_release(list_releases(repo, gh))
        for cmd in deploy_commands(repo, digest, exists=release is not None):
            print("+ " + shlex.join(["gh", *cmd]), flush=True)
            gh(cmd)
    except bundle.ReleaseError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(
        f"deploy: started. Follow it with `gh run watch --repo {repo}` or at"
        f" https://github.com/{repo}/actions/workflows/{WORKFLOW}"
    )
    return 0


def cmd_guard(repo_dir: Path) -> int:
    problems = bundle.guard(repo_dir)
    for line in problems:
        print(f"compendium guard: {line}", file=sys.stderr)
    if problems:
        print(
            f"compendium guard: {len(problems)} tracked file(s) look like compendium output;"
            " the site and its images never land in git (ADR 0121)",
            file=sys.stderr,
        )
        return 1
    print("compendium guard: OK, no compendium output among the tracked files")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="scripts/compendium", description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("build", "check"):
        p = sub.add_parser(name)
        p.add_argument("--app", type=Path, default=APP, help=f"MapleThief.app (default {APP})")
    p = sub.add_parser("serve")
    p.add_argument("--port", type=int, default=8000)
    sub.add_parser("size")
    p = sub.add_parser("pack")
    p.add_argument("--out", type=Path, default=BUNDLE)
    p = sub.add_parser("deploy")
    p.add_argument("--dry-run", action="store_true", help="stop before gh is called")
    p.add_argument("--yes", action="store_true", help="do not ask for confirmation")
    p.add_argument("--repo", default=GITHUB_REPO)
    p.add_argument("--app", type=Path, default=APP)
    p = sub.add_parser("guard")
    p.add_argument("--repo-dir", type=Path, default=Path("."), help="the checkout to look at")
    args = ap.parse_args(argv)
    if args.cmd == "build":
        return cmd_build(args.app)
    if args.cmd == "check":
        return cmd_check(args.app)
    if args.cmd == "serve":
        return cmd_serve(args.port)
    if args.cmd == "size":
        return cmd_size()
    if args.cmd == "pack":
        return cmd_pack(args.out)
    if args.cmd == "deploy":
        return cmd_deploy(args.repo, args.dry_run, args.yes, args.app)
    return cmd_guard(args.repo_dir)


if __name__ == "__main__":
    sys.exit(main())
