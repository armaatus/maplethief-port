"""Static portability checks over the Python in tools/ and tests/ (#111).

Windows breaks two habits that work on macOS and Linux: text I/O without an explicit
encoding (Windows defaults to the ANSI code page, so UTF-8 text fails to decode) and
``str(path.relative_to(root))`` as a bundle path (Windows gives backslashes, while keys,
manifests and comparisons are POSIX). These checks run on every OS, so a regression fails on
macOS too. ruff's PLW1514 (scripts/lint) covers builtin ``open()`` only, so this test also
covers the ``pathlib`` methods.
"""

from __future__ import annotations

import ast
from pathlib import Path

import pytest

from repo_paths import REPO

SOURCES = sorted(p for d in ("tools", "tests") for p in (REPO / d).rglob("*.py"))
TEXT_METHODS = {"read_text", "write_text"}


def _imported_names(tree: ast.Module) -> set[str]:
    """Names bound by import statements: ``gc.write_text(...)`` is a module's own helper."""
    names = set()
    for node in ast.walk(tree):
        if isinstance(node, (ast.Import, ast.ImportFrom)):
            names |= {(a.asname or a.name).split(".")[0] for a in node.names}
    return names


def _keywords(call: ast.Call) -> set[str | None]:
    return {k.arg for k in call.keywords}


def _text_mode(mode: ast.expr | None) -> bool:
    """True for a missing mode or a literal one without ``b``; False when it can't be known."""
    if mode is None:
        return True
    return isinstance(mode, ast.Constant) and isinstance(mode.value, str) and "b" not in mode.value


def _mode_arg(call: ast.Call, index: int) -> ast.expr | None:
    for k in call.keywords:
        if k.arg == "mode":
            return k.value
    return call.args[index] if len(call.args) > index else None


def problems(source: str, name: str = "<src>") -> list[str]:
    """Each portability problem in ``source`` as ``name:line: message``."""
    tree = ast.parse(source, name)
    imported = _imported_names(tree)
    found = []
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call):
            continue
        func, where = node.func, f"{name}:{node.lineno}"
        has_encoding = "encoding" in _keywords(node) or None in _keywords(node)  # **kwargs
        if isinstance(func, ast.Name) and func.id == "open":
            if not has_encoding and _text_mode(_mode_arg(node, 1)):
                found.append(f"{where}: open() in text mode without encoding=")
        elif isinstance(func, ast.Attribute):
            receiver = func.value
            if isinstance(receiver, ast.Name) and receiver.id in imported:
                continue  # a module function such as tarfile.open or ghidra_common.write_text
            if func.attr in TEXT_METHODS and not has_encoding:
                found.append(f"{where}: .{func.attr}() without encoding=")
            elif func.attr == "open" and not has_encoding:
                # Path.open(mode): only a literal text mode, or no argument at all, is certain.
                first = _mode_arg(node, 0)
                if (first is None and not node.args) or (
                    isinstance(first, ast.Constant) and _text_mode(first)
                ):
                    found.append(f"{where}: .open() in text mode without encoding=")
        if (
            isinstance(func, ast.Name)
            and func.id == "str"
            and len(node.args) == 1
            and isinstance(call := node.args[0], ast.Call)
            and isinstance(call.func, ast.Attribute)
            and call.func.attr == "relative_to"
        ):
            found.append(f"{where}: str(....relative_to(...)): use .as_posix()")
    return found


def test_sources_are_found():
    names = {p.relative_to(REPO).as_posix() for p in SOURCES}
    assert {"tools/mtport/pipeline.py", "tests/test_portability.py"} <= names


@pytest.mark.parametrize("path", SOURCES, ids=lambda p: p.relative_to(REPO).as_posix())
def test_text_io_has_an_encoding_and_relative_paths_are_posix(path: Path):
    rel = path.relative_to(REPO).as_posix()
    assert problems(path.read_text(encoding="utf-8"), rel) == []


@pytest.mark.parametrize(
    "code",
    [
        "p.read_text()",
        "Path('x').write_text('y')",
        "open(p)",
        "open(p, 'w')",
        "open(p, mode='a')",
        "p.open()",
        "p.open('w')",
        "str(p.relative_to(root))",
    ],
)
def test_flags(code):
    assert len(problems(code)) == 1, code


@pytest.mark.parametrize(
    "code",
    [
        "p.read_text(encoding='utf-8')",
        "p.write_text('y', encoding='utf-8')",
        "p.write_text('y', **kw)",
        "open(p, 'rb')",
        "open(p, mode='wb')",
        "open(p, 'w', encoding='utf-8')",
        "open(p, mode)",  # unknown mode: not flagged
        "p.open('rb')",
        "archive.open(info)",  # zipfile.ZipFile.open(member)
        "import tarfile\ntarfile.open(p, 'w:gz')",
        "from mtport import ghidra_common as gc\ngc.write_text(p, 'x')",
        "p.relative_to(root).as_posix()",
        "str(p)",
    ],
)
def test_allows(code):
    assert problems(code) == [], code
