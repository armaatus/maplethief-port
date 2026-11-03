"""Registers each doctest case as one CTest, whatever its name (#171, ADR 0005).

doctest's own ``doctest_discover_tests`` parses ``--list-test-cases`` with CMake lists: a case
name with ``;`` is cut in two, a ``[`` swallows the rest of the list, and the cut name's
``--test-case=`` filter matches nothing, so doctest exits 0 and the CTest "passes" without
running anything. 48 cases hid that way until #171.

``write`` (a POST_BUILD step of every doctest executable, ``tests/CMakeLists.txt``) lists the
cases and writes the CTest script that registers them: every name and argument is a CMake
bracket argument, so no character of a name is special, and each test's filter is the name with
doctest's escapes (``\\`` and ``,``; non-ASCII runs become ``*``, which survives any command-line
code page). doctest has no escape for its wildcards ``*`` and ``?``, so ``write`` fails the build
when a filter would match more than one case (doctest compares case-insensitively). Each test
also sets ``MT_DOCTEST_EXPECT_CASES=1``: the shared ``main()`` (``tests/cpp/doctest_main.cpp``)
then fails a run whose filter matched no case, or more than one.

``check`` (CTest ``doctest.discovery``) reads ``ctest --show-only=json-v1`` and checks, for every
doctest executable, that CTest has exactly one test per listed case, with that case's filter.

This module is stdlib-only and runs as a plain script (``python -I -B``) from the build.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from collections.abc import Sequence
from pathlib import Path

EXPECT_ENV = "MT_DOCTEST_EXPECT_CASES"
FILTER_OPTION = "--test-case="
_RULE = "=" * 79


class DiscoveryError(Exception):
    pass


def parse_list(output: str) -> list[str]:
    """The case names in ``--list-test-cases --no-colors`` output.

    The names sit between two rules of '='; the last line states how many there are.
    """
    lines = output.splitlines()
    try:
        start = lines.index(_RULE)
        end = lines.index(_RULE, start + 1)
    except ValueError:
        raise DiscoveryError("no list of test cases in the output:\n" + output) from None
    names = lines[start + 1 : end]
    m = re.search(r"test cases passing the current filters: (\d+)", output)
    if m is None or int(m.group(1)) != len(names):
        raise DiscoveryError(f"listed {len(names)} names, but the count line says otherwise")
    return names


def list_cases(exe: str, cwd: str | None = None) -> list[str]:
    r = subprocess.run(
        [exe, "--list-test-cases", "--no-colors"],
        capture_output=True,
        cwd=cwd,
        check=False,
    )
    out = r.stdout.decode("utf-8", "surrogateescape")
    if r.returncode != 0:
        raise DiscoveryError(f"{exe} --list-test-cases exited {r.returncode}:\n{out}")
    return parse_list(out)


def case_filter(name: str) -> str:
    """The ``--test-case`` value that selects ``name``: ``\\`` and ``,`` escaped as doctest's
    parser wants, each run of non-ASCII characters a ``*`` (Windows passes arguments through the
    ANSI code page, which can't carry them)."""
    escaped = name.replace("\\", "\\\\").replace(",", "\\,")
    return re.sub(r"[^\x00-\x7f]+", "*", escaped)


def unescape_filter(value: str) -> list[str]:
    """doctest's own split of a ``--test-case`` value into patterns (parseCommaSepArgs)."""
    out: list[str] = []
    cur: list[str] = []
    backslash = False
    for ch in value:
        if backslash:
            backslash = False
            if ch in ",\\":
                cur.append(ch)
                continue
            cur.append("\\")
        if ch == "\\":
            backslash = True
        elif ch == ",":
            if cur:
                out.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    if backslash:
        cur.append("\\")
    if cur:
        out.append("".join(cur))
    return out


def wildcmp(name: str, wild: str) -> bool:
    """doctest's case-insensitive wildcard match (``*`` and ``?``) of a whole name."""
    pattern = "".join(".*" if c == "*" else "." if c == "?" else re.escape(c) for c in wild.lower())
    return re.fullmatch(pattern, name.lower(), re.DOTALL) is not None


def matches(names: Sequence[str], value: str) -> list[str]:
    patterns = unescape_filter(value)
    return [n for n in names if any(wildcmp(n, p) for p in patterns)]


def check_unique(names: Sequence[str]) -> None:
    """Each case's filter selects that case only."""
    problems = []
    if len(set(names)) != len(names):
        problems.append(
            "duplicate names: " + ", ".join(sorted({n for n in names if names.count(n) > 1}))
        )
    for n in names:
        hit = matches(names, case_filter(n))
        if hit != [n]:
            problems.append(f"the filter for {n!r} selects {hit!r}")
    if problems:
        raise DiscoveryError(
            "doctest can't select these cases one by one; rename them "
            "(doctest's * and ? can't be escaped, and names compare case-insensitively):\n  "
            + "\n  ".join(problems)
        )


def bracket(value: str) -> str:
    """``value`` as a CMake bracket argument, which is taken literally."""
    eq = ""
    while f"]{eq}]" in value:
        eq += "="
    return f"[{eq}[{value}]{eq}]"


def render(names: Sequence[str], exe: str, prefix: str, workdir: str, props_fn: str) -> str:
    """The CTest script that registers ``names``. ``props_fn`` is a CMake function (defined by
    the include script) that sets the shared properties of one test."""
    lines = [f"# Written by tools/mtport/doctestctest.py for {exe}; do not edit."]
    for n in names:
        test = bracket(prefix + n)
        lines.append(f"add_test({test} {bracket(exe)} {bracket(FILTER_OPTION + case_filter(n))})")
        lines.append(
            f"set_tests_properties({test} PROPERTIES WORKING_DIRECTORY {bracket(workdir)}"
            f" ENVIRONMENT_MODIFICATION {bracket(EXPECT_ENV + '=set:1')})"
        )
        lines.append(f"{props_fn}({test})")
    return "\n".join(lines) + "\n"


def cmd_write(args: argparse.Namespace) -> int:
    names = list_cases(args.exe, args.workdir)
    check_unique(names)
    text = render(names, args.exe, args.prefix, args.workdir, args.props_fn)
    out = Path(args.out)
    tmp = out.with_name(out.name + ".tmp")
    tmp.write_text(text, encoding="utf-8", errors="surrogateescape")
    os.replace(tmp, out)
    return 0


def doctest_tests(show_only: dict) -> dict[str, list[tuple[str, str]]]:
    """``{executable: [(test name, --test-case value)]}`` for the tests that set
    ``MT_DOCTEST_EXPECT_CASES``, from ``ctest --show-only=json-v1``."""
    out: dict[str, list[tuple[str, str]]] = {}
    for t in show_only.get("tests", []):
        props = {p["name"]: p["value"] for p in t.get("properties", [])}
        env = props.get("ENVIRONMENT_MODIFICATION", [])
        if not any(e.startswith(EXPECT_ENV + "=") for e in env):
            continue
        cmd = t.get("command") or []
        filters = [a[len(FILTER_OPTION) :] for a in cmd[1:] if a.startswith(FILTER_OPTION)]
        if len(filters) != 1:
            raise DiscoveryError(f"{t['name']!r} has {len(filters)} {FILTER_OPTION} arguments")
        out.setdefault(cmd[0], []).append((t["name"], filters[0]))
    return out


def compare(exe: str, names: Sequence[str], tests: Sequence[tuple[str, str]]) -> list[str]:
    """What is wrong with ``tests`` (CTest's) for the cases ``names`` of ``exe``."""
    problems = []
    by_filter: dict[str, list[str]] = {}
    for test, value in tests:
        by_filter.setdefault(value, []).append(test)
        hit = matches(names, value)
        if len(hit) != 1:
            problems.append(f"{exe}: CTest {test!r} selects {len(hit)} cases: {hit!r}")
        elif not test.endswith(hit[0]):
            problems.append(f"{exe}: CTest {test!r} runs the case {hit[0]!r}")
    for n in names:
        got = by_filter.get(case_filter(n), [])
        if len(got) != 1:
            problems.append(f"{exe}: the case {n!r} has {len(got)} CTests: {got!r}")
    return problems


def cmd_check(args: argparse.Namespace) -> int:
    r = subprocess.run(
        [args.ctest, "--show-only=json-v1"],
        capture_output=True,
        cwd=args.build_dir,
        check=True,
    )
    groups = doctest_tests(json.loads(r.stdout))
    if not groups:
        print("doctest.discovery: no doctest CTests found", file=sys.stderr)
        return 1
    problems: list[str] = []
    total = 0
    for exe, tests in sorted(groups.items()):
        names = list_cases(exe)
        total += len(names)
        problems += compare(exe, names, tests)
        print(f"{Path(exe).name}: {len(names)} cases, {len(tests)} CTests")
    for p in problems:
        print(p, file=sys.stderr)
    if problems:
        return 1
    print(f"doctest.discovery: {total} cases in {len(groups)} executables, one CTest each")
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    w = sub.add_parser("write", help="list a doctest executable's cases into a CTest script")
    w.add_argument("--exe", required=True)
    w.add_argument("--prefix", default="")
    w.add_argument("--workdir", required=True)
    w.add_argument("--props-fn", required=True)
    w.add_argument("--out", required=True)
    c = sub.add_parser("check", help="every doctest case has exactly one CTest")
    c.add_argument("--ctest", required=True)
    c.add_argument("--build-dir", required=True)
    args = ap.parse_args(argv)
    try:
        return cmd_write(args) if args.cmd == "write" else cmd_check(args)
    except DiscoveryError as e:
        print(f"doctestctest: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
