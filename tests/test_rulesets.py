"""The branch rulesets of .github/rulesets/ (#329, ADR 0173): every required check is a name CI
reports on a pull request into that branch. On dev that means only jobs that never skip, since a
skipped matrix job reports under its unexpanded name; the `CI result` job stands in for the rest.
scripts/rulesets check compares the files with what GitHub has."""

from __future__ import annotations

import json
import re

import pytest

from repo_paths import REPO

WORKFLOWS = REPO / ".github" / "workflows"


def jobs(workflow: str) -> dict[str, dict]:
    """The jobs of a workflow by id: their name, `if:` and `needs:` lines (no YAML parser in
    the test environment, and these keys sit on single lines)."""
    text = (WORKFLOWS / workflow).read_text(encoding="utf-8")
    body = text.split("\njobs:\n", 1)[1]
    found: dict[str, dict] = {}
    current = None
    for line in body.splitlines():
        if m := re.match(r"^  ([A-Za-z0-9_-]+):\s*$", line):
            current = found.setdefault(m.group(1), {})
        elif current is not None and (m := re.match(r"^    (name|if|needs): (.*)$", line)):
            current[m.group(1)] = m.group(2).strip()
    return found


def ruleset(name: str) -> dict:
    return json.loads((REPO / ".github" / "rulesets" / f"{name}.json").read_text(encoding="utf-8"))


def required(name: str) -> set[str]:
    rules = {r["type"]: r for r in ruleset(name)["rules"]}
    return {
        c["context"]
        for c in rules["required_status_checks"]["parameters"]["required_status_checks"]
    }


CI = jobs("ci.yml")
# Jobs that never run on a pull request (the weekly run's report, #339): no ruleset can require
# them, and `CI result` doesn't wait for them.
NOT_ON_PULL_REQUESTS = {
    job_id
    for job_id, job in CI.items()
    if job.get("if") == "${{ !cancelled() && (github.event_name == 'schedule' || inputs.report) }}"
}


def test_ci_result_needs_every_other_job_and_always_runs():
    gate = CI["ci-result"]
    assert gate["name"] == "CI result"
    assert gate["if"] == "always()"
    needs = set(gate["needs"].strip("[]").replace(" ", "").split(","))
    assert needs == set(CI) - {"ci-result"} - NOT_ON_PULL_REQUESTS


def test_the_weekly_report_never_runs_on_a_pull_request():
    assert NOT_ON_PULL_REQUESTS == {"weekly-report"}


def test_dev_requires_only_checks_that_never_skip():
    always = {
        j["name"]
        for i, j in CI.items()
        if j.get("if") in (None, "always()") and i not in NOT_ON_PULL_REQUESTS
    }
    assert required("dev") == always
    assert "CI result" in required("dev")


def test_main_requires_the_full_profile_and_the_release_title():
    names = {
        j["name"] for i, j in CI.items() if "${{" not in j["name"] and i not in NOT_ON_PULL_REQUESTS
    }
    matrix = {
        f"{os} {cfg}"
        for os in (
            "ubuntu-latest",
            "macos-latest",
            "windows-latest",
            "ubuntu-24.04-arm",
            "windows-11-arm",
        )
        for cfg in ("debug", "release")
    }
    title = jobs("release.yml")["title"]["name"]
    assert required("main") == names | matrix | {title}


def test_the_scope_job_never_skips_on_main():
    text = (WORKFLOWS / "ci.yml").read_text(encoding="utf-8")
    assert '[ "$BASE" = main ]' in text


@pytest.mark.parametrize("name", ["dev", "main"])
def test_rulesets_allow_merge_commits_only_and_block_force_push_and_deletion(name):
    rs = ruleset(name)
    assert rs["name"] == name
    assert rs["enforcement"] == "active"
    assert rs["bypass_actors"] == []
    assert rs["conditions"]["ref_name"]["include"] == [f"refs/heads/{name}"]
    rules = {r["type"]: r for r in rs["rules"]}
    assert {"deletion", "non_fast_forward", "pull_request", "required_status_checks"} <= set(rules)
    assert rules["pull_request"]["parameters"]["allowed_merge_methods"] == ["merge"]
