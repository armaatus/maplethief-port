"""The end-of-line policy (#328, ``.gitattributes``): every tracked text file checks out with LF
on every OS, so a fresh Windows checkout (where ``core.autocrlf=true`` is the usual setting) runs
the shell scripts, which bash refuses with CRLF. This test runs on Windows CI too, through
``scripts/test.ps1``, against that checkout's own working tree."""

from __future__ import annotations

import subprocess

import pytest

from repo_paths import REPO

SHEBANG_SHELLS = (b"sh", b"bash")


def git(*args: str) -> str:
    return subprocess.run(
        ["git", *args], cwd=REPO, capture_output=True, check=True, text=True, encoding="utf-8"
    ).stdout


def tracked() -> list[str]:
    try:
        out = git("ls-files", "-z")
    except (OSError, subprocess.CalledProcessError):
        pytest.skip("not a git checkout")
    return [n for n in out.split("\0") if n]


def is_shell_script(name: str) -> bool:
    if name.endswith(".sh"):
        return True
    if "." in name.rsplit("/", 1)[-1]:
        return False
    path = REPO / name
    if not path.is_file():
        return False
    with path.open("rb") as f:
        first = f.readline()
    if not first.startswith(b"#!"):
        return False
    words = first[2:].replace(b"/", b" ").split()
    return any(w in SHEBANG_SHELLS for w in words)


def attrs(names: list[str]) -> dict[str, dict[str, str]]:
    """``git check-attr text eol`` for each name: {name: {attribute: value}}."""
    out = subprocess.run(
        ["git", "check-attr", "-z", "--stdin", "text", "eol"],
        cwd=REPO,
        input="\0".join(names) + "\0",
        capture_output=True,
        check=True,
        text=True,
        encoding="utf-8",
    ).stdout
    fields = out.split("\0")
    result: dict[str, dict[str, str]] = {}
    for i in range(0, len(fields) - 2, 3):
        result.setdefault(fields[i], {})[fields[i + 1]] = fields[i + 2]
    return result


def test_gitattributes_sets_lf_for_text() -> None:
    lines = (REPO / ".gitattributes").read_text(encoding="utf-8").splitlines()
    assert "* text=auto eol=lf" in lines


def test_shell_scripts_check_out_with_lf() -> None:
    scripts = [n for n in tracked() if is_shell_script(n)]
    assert "scripts/test" in scripts and "scripts/lint" in scripts, scripts
    for name, a in attrs(scripts).items():
        assert a.get("eol") == "lf", f"{name}: eol={a.get('eol')}"
        assert a.get("text") in ("set", "auto"), f"{name}: text={a.get('text')}"
    crlf = [n for n in scripts if b"\r" in (REPO / n).read_bytes()]
    assert not crlf, f"shell scripts with CR in the working tree: {crlf}"


def test_text_files_have_no_crlf_in_the_working_tree() -> None:
    # `git ls-files --eol` reports what git sees in the working tree (w/crlf, w/mixed).
    out = git("ls-files", "--eol")
    bad = []
    for line in out.splitlines():
        info, _, name = line.partition("\t")
        if "w/crlf" in info or "w/mixed" in info:
            bad.append(name)
    assert not bad, f"text files checked out with CRLF: {bad}"
