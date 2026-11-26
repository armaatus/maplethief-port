"""The weekly CI report (#339): the last job of CI's scheduled run on ``dev``.

The schedule runs the manual-run profile (every OS, Debug and Release, iOS, the Switch,
Android, ASan). This module lists that run's jobs through GitHub's API and checks that every
job of the profile, :data:`EXPECTED`, ran and passed: a run has ended as a failure with jobs
missing and no log, so the count of jobs is checked, not only the conclusion. A missing,
skipped, cancelled or failed job fails the report.

On a failure it opens one issue labelled :data:`LABEL` with the failed jobs' links and the
missing jobs, or comments on the open one, so a break stays in one place until it is fixed. A
green run comments on the open issue and closes it. Everything goes through ``gh``.

CLI (CI's ``weekly report`` job sets the arguments from its environment)::

    PYTHONPATH=tools python3 -m mtport.cireport [--repo OWNER/NAME] [--run-id ID]
        [--sha SHA] [--branch NAME] [--dry-run]

``--dry-run`` reads the run and prints the report and what it would do to the issue, without
writing anything. Exit status: 0 green, 1 a job failed or is missing, 2 ``gh`` failed.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from dataclasses import dataclass, field

# Every job of ci.yml's full profile (a scheduled or manual run on hosted runners), by job id,
# with the names its runs show. tests/test_ci_report.py checks the ids against ci.yml.
EXPECTED: dict[str, tuple[str, ...]] = {
    "scope": ("scope",),
    "build": tuple(
        f"{os_name} {config}"
        for os_name in ("ubuntu-latest", "macos-latest", "windows-latest")
        for config in ("debug", "release")
    ),
    "asan": ("linux asan+ubsan",),
    "ios": ("ios arm64 (compile, no IPA, no signing)",),
    "switch": ("switch (NRO, devkitA64 container, no IPA)",),
    "android": ("android (APK, pinned SDK and NDK container, no IPA)",),
    "lint-python": ("lint + Python min/latest",),
    "clang-tidy": ("lint (clang-tidy)",),
}
# The job that runs this module; it is still running when it reads the list.
SELF = ("weekly-report", "weekly report")

LABEL = "ci-weekly"
LABEL_COLOR = "d93f0b"
TITLE = "Weekly CI failed on dev"


@dataclass(frozen=True)
class Job:
    name: str
    conclusion: str | None  # None while it is still running
    url: str


@dataclass
class Report:
    failed: list[Job] = field(default_factory=list)
    missing: list[str] = field(default_factory=list)
    ran: int = 0

    @property
    def green(self) -> bool:
        return not self.failed and not self.missing


def assess(jobs: list[Job]) -> Report:
    """Compare the run's jobs with :data:`EXPECTED`. A job that ran but isn't expected (one
    added to ci.yml since) fails the report only when it failed."""
    expected = [name for names in EXPECTED.values() for name in names]
    by_name = {job.name: job for job in jobs if job.name != SELF[1]}
    report = Report(ran=len(by_name))
    for name in expected:
        job = by_name.get(name)
        if job is None:
            report.missing.append(name)
        elif job.conclusion != "success":
            report.failed.append(job)
    report.failed += [
        job
        for name, job in by_name.items()
        if name not in expected and job.conclusion not in ("success", "skipped")
    ]
    return report


def render(report: Report, run_url: str, sha: str, branch: str) -> str:
    """The Markdown of the issue body or comment."""
    expected = sum(len(names) for names in EXPECTED.values())
    lines = [
        f"CI's weekly run on `{branch}` at {sha[:12]}: [run]({run_url}), "
        f"{report.ran} of {expected} expected jobs ran.",
        "",
    ]
    if report.green:
        lines.append("Every expected job ran and passed.")
        return "\n".join(lines) + "\n"
    if report.failed:
        lines.append("Failed:")
        lines += [f"- [{j.name}]({j.url}): {j.conclusion or 'not finished'}" for j in report.failed]
        lines.append("")
    if report.missing:
        lines.append("Missing (no job of this name in the run):")
        lines += [f"- {name}" for name in report.missing]
        lines.append("")
    lines.append(
        "This issue stays open while the weekly run fails: each failing run comments here and "
        "the next green one closes it (`tools/mtport/cireport.py`, docs/build/testing.md)."
    )
    return "\n".join(lines) + "\n"


class Gh:
    """GitHub through the ``gh`` CLI (raises OSError or CalledProcessError when it fails)."""

    def __init__(self, repo: str) -> None:
        self.repo = repo

    def _run(self, args: list[str]) -> str:
        return subprocess.run(
            ["gh", *args], capture_output=True, text=True, check=True, timeout=120
        ).stdout

    def jobs(self, run_id: str) -> list[Job]:
        out = self._run(
            [
                "api",
                "--paginate",
                f"repos/{self.repo}/actions/runs/{run_id}/jobs?filter=latest&per_page=100",
                "--jq",
                ".jobs[] | {name, conclusion, html_url}",
            ]
        )
        jobs = []
        for line in out.splitlines():
            if line.strip():
                item = json.loads(line)
                jobs.append(Job(item["name"], item["conclusion"], item["html_url"]))
        return jobs

    def open_issue(self) -> int | None:
        out = self._run(
            [
                "api",
                f"repos/{self.repo}/issues?labels={LABEL}&state=open&per_page=100",
                "--jq",
                "[.[] | select(.pull_request == null) | .number] | min",
            ]
        ).strip()
        return int(out) if out and out != "null" else None

    def ensure_label(self) -> None:
        try:
            self._run(
                [
                    "api",
                    "-X",
                    "POST",
                    f"repos/{self.repo}/labels",
                    "-f",
                    f"name={LABEL}",
                    "-f",
                    f"color={LABEL_COLOR}",
                    "-f",
                    "description=CI's weekly full run on dev failed (tools/mtport/cireport.py)",
                ]
            )
        except subprocess.CalledProcessError:
            pass  # it exists already (422)

    def create_issue(self, body: str) -> int:
        out = self._run(
            [
                "api",
                "-X",
                "POST",
                f"repos/{self.repo}/issues",
                "-f",
                f"title={TITLE}",
                "-f",
                f"body={body}",
                "-f",
                f"labels[]={LABEL}",
                "--jq",
                ".number",
            ]
        )
        return int(out.strip())

    def comment(self, number: int, body: str) -> None:
        self._run(
            [
                "api",
                "-X",
                "POST",
                f"repos/{self.repo}/issues/{number}/comments",
                "-f",
                f"body={body}",
            ]
        )

    def close(self, number: int) -> None:
        self._run(
            [
                "api",
                "-X",
                "PATCH",
                f"repos/{self.repo}/issues/{number}",
                "-f",
                "state=closed",
                "-f",
                "state_reason=completed",
            ]
        )


def file_report(gh: Gh, report: Report, body: str, dry_run: bool = False) -> str:
    """Open, update or close the issue for ``report``; returns what it did, in words."""
    number = gh.open_issue()
    if report.green:
        if number is None:
            return "green, no open issue"
        if not dry_run:
            gh.comment(number, body)
            gh.close(number)
        return f"green, closed #{number}"
    if number is not None:
        if not dry_run:
            gh.comment(number, body)
        return f"failed, commented on #{number}"
    if dry_run:
        return "failed, would open an issue"
    gh.ensure_label()
    return f"failed, opened #{gh.create_issue(body)}"


def main(argv: list[str] | None = None) -> int:
    env = os.environ
    parser = argparse.ArgumentParser(prog="mtport.cireport", description=__doc__.split("\n")[0])
    parser.add_argument("--repo", default=env.get("GITHUB_REPOSITORY", "armaatus/maplethief-port"))
    parser.add_argument("--run-id", default=env.get("GITHUB_RUN_ID"))
    parser.add_argument("--sha", default=env.get("GITHUB_SHA", ""))
    parser.add_argument("--branch", default=env.get("GITHUB_REF_NAME", "dev"))
    parser.add_argument("--dry-run", action="store_true", help="read only; print the report")
    args = parser.parse_args(argv)
    if not args.run_id:
        parser.error("--run-id is required outside GitHub Actions")
    server = env.get("GITHUB_SERVER_URL", "https://github.com")
    run_url = f"{server}/{args.repo}/actions/runs/{args.run_id}"

    gh = Gh(args.repo)
    try:
        report = assess(gh.jobs(args.run_id))
        body = render(report, run_url, args.sha, args.branch)
        print(body)
        print(f"cireport: {file_report(gh, report, body, args.dry_run)}")
    except (OSError, subprocess.SubprocessError, ValueError) as e:
        stderr = getattr(e, "stderr", "") or ""
        print(f"cireport: gh failed: {e} {stderr}".rstrip(), file=sys.stderr)
        return 2
    summary = env.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as f:
            f.write(body)
    return 0 if report.green else 1


if __name__ == "__main__":
    sys.exit(main())
