"""The weekly CI report (#339, tools/mtport/cireport.py): every job of the full profile must
run and pass; the first failing run opens the ci-weekly issue, the next one comments on it, and
a green run closes it. ``gh`` is replaced by a fake that keeps the issues in memory."""

from __future__ import annotations

import re
import subprocess

import pytest

from mtport import cireport
from mtport.cireport import EXPECTED, SELF, Job, assess, file_report, render
from repo_paths import REPO

CI = (REPO / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
NAMES = [name for names in EXPECTED.values() for name in names]


def jobs(conclusion: dict[str, str | None] | None = None, drop: tuple[str, ...] = ()) -> list[Job]:
    """The full run's jobs, all green unless ``conclusion`` says otherwise, without ``drop``."""
    conclusion = conclusion or {}
    out = [
        Job(name, conclusion.get(name, "success"), f"https://example.invalid/job/{i}")
        for i, name in enumerate(NAMES)
        if name not in drop
    ]
    return out + [Job(SELF[1], None, "https://example.invalid/job/self")]


class FakeGh(cireport.Gh):
    def __init__(self) -> None:
        super().__init__("owner/repo")
        self.issues: dict[int, dict] = {}
        self.labels: set[str] = set()

    def open_issue(self) -> int | None:
        open_ = [n for n, i in self.issues.items() if i["open"] and cireport.LABEL in i["labels"]]
        return min(open_) if open_ else None

    def ensure_label(self) -> None:
        self.labels.add(cireport.LABEL)

    def create_issue(self, body: str) -> int:
        assert cireport.LABEL in self.labels
        number = len(self.issues) + 1
        self.issues[number] = {
            "title": cireport.TITLE,
            "body": body,
            "labels": {cireport.LABEL},
            "comments": [],
            "open": True,
        }
        return number

    def comment(self, number: int, body: str) -> None:
        self.issues[number]["comments"].append(body)

    def close(self, number: int) -> None:
        self.issues[number]["open"] = False


def run(gh: FakeGh, report: cireport.Report) -> str:
    return file_report(gh, report, render(report, "https://example.invalid/run/1", "a" * 40, "dev"))


def test_a_green_run_is_green():
    report = assess(jobs())
    assert report.green
    assert report.ran == len(NAMES)
    assert "Every expected job ran and passed" in render(report, "u", "sha", "dev")


def test_a_failed_job_is_reported_with_its_link():
    report = assess(
        jobs(
            {
                "windows-latest release": "failure",
                "ios arm64 (compile, no IPA, no signing)": "timed_out",
            }
        )
    )
    assert not report.green
    assert [j.name for j in report.failed] == [
        "windows-latest release",
        "ios arm64 (compile, no IPA, no signing)",
    ]
    body = render(report, "u", "sha", "dev")
    assert "[windows-latest release](https://example.invalid/job/" in body
    assert ": timed_out" in body


@pytest.mark.parametrize("conclusion", ["skipped", "cancelled", None])
def test_a_job_that_did_not_pass_fails(conclusion):
    assert not assess(jobs({"lint (clang-tidy)": conclusion})).green


def test_a_missing_job_fails_even_when_the_rest_passed():
    report = assess(
        jobs(drop=("macos-latest debug", "android (APK, pinned SDK and NDK container, no IPA)"))
    )
    assert not report.green
    assert not report.failed
    assert report.missing == [
        "macos-latest debug",
        "android (APK, pinned SDK and NDK container, no IPA)",
    ]
    body = render(report, "u", "sha", "dev")
    assert f"{len(NAMES) - 2} of {len(NAMES)} expected jobs ran" in body
    assert "- macos-latest debug" in body


def test_an_unexpected_job_fails_only_when_it_failed():
    assert assess(jobs() + [Job("new gate", "success", "u")]).green
    assert assess(jobs() + [Job("new job", "skipped", "u")]).green
    assert [j.name for j in assess(jobs() + [Job("new gate", "failure", "u")]).failed] == [
        "new gate"
    ]


def test_the_first_failure_opens_the_issue_the_second_updates_it_green_closes_it():
    gh = FakeGh()
    assert run(gh, assess(jobs())) == "green, no open issue"
    assert gh.issues == {}

    assert run(gh, assess(jobs({"scope": "failure"}))) == "failed, opened #1"
    assert gh.issues[1]["open"] and "[scope]" in gh.issues[1]["body"]

    assert run(gh, assess(jobs(drop=("scope",)))) == "failed, commented on #1"
    assert len(gh.issues) == 1
    assert "- scope" in gh.issues[1]["comments"][0]

    assert run(gh, assess(jobs())) == "green, closed #1"
    assert not gh.issues[1]["open"]

    # The next break opens a new issue.
    assert run(gh, assess(jobs({"scope": "failure"}))) == "failed, opened #2"


def test_a_dry_run_writes_nothing():
    gh = FakeGh()
    report = assess(jobs({"scope": "failure"}))
    assert file_report(gh, report, "body", dry_run=True) == "failed, would open an issue"
    assert gh.issues == {}


def test_gh_is_called_with_the_runs_jobs_and_the_issue(monkeypatch):
    calls: list[list[str]] = []
    outputs = iter(
        [
            '{"name":"scope","conclusion":"success","html_url":"u1"}\n'
            '{"name":"weekly report","conclusion":null,"html_url":"u2"}\n',
            "null\n",
            "",
            "7\n",
        ]
    )

    def fake_run(args, **kwargs):
        calls.append(args)
        return subprocess.CompletedProcess(args, 0, next(outputs), "")

    monkeypatch.setattr(cireport.subprocess, "run", fake_run)
    gh = cireport.Gh("owner/repo")
    assert gh.jobs("42") == [Job("scope", "success", "u1"), Job("weekly report", None, "u2")]
    assert calls[0][:3] == ["gh", "api", "--paginate"]
    assert "repos/owner/repo/actions/runs/42/jobs?filter=latest&per_page=100" in calls[0]
    assert gh.open_issue() is None
    assert "labels=ci-weekly&state=open" in calls[1][2]
    gh.ensure_label()
    assert gh.create_issue("body") == 7
    assert "labels[]=ci-weekly" in calls[3] and f"title={cireport.TITLE}" in calls[3]


def _job_blocks() -> dict[str, str]:
    body = CI[CI.index("\njobs:\n") :]
    ids = re.findall(r"^  ([\w-]+):\n", body, re.M)
    blocks = {}
    for i, job_id in enumerate(ids):
        start = body.index(f"\n  {job_id}:\n")
        end = body.index(f"\n  {ids[i + 1]}:\n") if i + 1 < len(ids) else len(body)
        blocks[job_id] = body[start:end]
    return blocks


def test_the_expected_jobs_are_cis_jobs():
    # A job added to ci.yml is added to EXPECTED too, or the weekly run can't see it go missing.
    blocks = _job_blocks()
    assert set(blocks) == set(EXPECTED) | {SELF[0]}
    for job_id, names in EXPECTED.items():
        if job_id != "build":
            assert names == (re.search(r"\n    name: (.+)\n", blocks[job_id]).group(1),)
    assert f"\n    name: {SELF[1]}\n" in blocks[SELF[0]]


def test_the_report_job_needs_every_job_and_runs_on_the_schedule():
    job = _job_blocks()[SELF[0]]
    needs = re.search(r"\n    needs: \[(.+)\]\n", job).group(1).split(", ")
    assert set(needs) == set(EXPECTED)
    assert "!cancelled() && (github.event_name == 'schedule' || inputs.report)" in job
    assert "issues: write" in job
    assert "python3 -m mtport.cireport" in job
    assert re.search(r"\n  schedule:\n    #.*\n.*\n    - cron: \"[^\"]+\"\n", CI)


def test_the_weekly_run_builds_the_full_profile():
    build = _job_blocks()["build"]
    assert "github.event_name == 'schedule'" in build  # Release on the weekly run
    assert "github.event_name == 'pull_request' && github.base_ref != 'main'" in build  # every OS
    assert "github.event_name == 'schedule'" in _job_blocks()["ios"]
