"""Prose lint for the compendium (#212): the mechanical rules of ``docs/compendium/style.md``.

It checks our own words, never the game's: hand-written Markdown under ``docs/compendium/``,
the site generator's templates, and the built HTML site. Rules:

* ``em-dash`` (U+2014), ``en-dash`` (U+2013) and ``other-dash`` (U+2012, U+2015, U+2E3A,
  U+2E3B): ranges are written "to" in prose and with a plain hyphen in table cells.
* ``double-hyphen``: ``--`` used as a dash. Markdown rules and table separator rows are fine.
* ``banned-phrase``: filler and hype from the style guide's list (:data:`BANNED_PHRASES`).
* ``first-person``: "we", "our", "let's", "I'm" and the like; the compendium is third person.
* ``term``: a spelling the style guide replaces ("mesos" is "meso").
* ``emoji``: pictographs and dingbats.
* ``title-case-heading``: headings are sentence case. A capitalised word after the first is
  allowed when it is a known name (:func:`known_names`: the style guide's glossary plus a
  built-in list), an acronym, or starts a new sentence after ``:`` or ``.``.

Exempt from every rule:

* Markdown: fenced code blocks, code spans, ``{{...}}`` references and template tags
  (``{% ... %}``), link targets, HTML comment delimiters, and inline elements with a
  ``data-verbatim`` attribute.
* HTML: any element with a ``data-verbatim`` attribute and everything inside it (the site
  generator marks game text that way), ``<script>``, ``<style>``, ``<code>`` and ``<pre>``.

Usage::

    PYTHONPATH=tools python3 -m mtport.prose_lint [PATH...]

A path may be a file or a directory (searched for ``*.md``, ``*.html`` and ``*.htm``).
Without paths it checks the git-tracked ``docs/compendium/**/*.md`` and every file under
``tools/mtport/compendium/templates/`` if that exists. One line per finding,
``path:line:col: rule: message``; the exit status is 1 when there are findings.

Standard library only; files are parsed as text and never executed.
"""

from __future__ import annotations

import argparse
import functools
import re
import subprocess
import sys
from collections.abc import Iterable, Sequence
from dataclasses import dataclass
from html.parser import HTMLParser
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
STYLE_GUIDE = ROOT / "docs" / "compendium" / "style.md"
TEMPLATES = ROOT / "tools" / "mtport" / "compendium" / "templates"

MASK = "\x00"  # stands in for exempt text, column for column

DASHES = {
    "—": ("em-dash", "em dash (U+2014); use a comma, colon, parentheses or two sentences"),
    "–": ("en-dash", "en dash (U+2013); write ranges as 'to' in prose, '-' in table cells"),
    "‒": ("other-dash", "figure dash (U+2012); use a plain hyphen"),
    "―": ("other-dash", "horizontal bar (U+2015); use a comma, colon or parentheses"),
    "⸺": ("other-dash", "two-em dash (U+2E3A)"),
    "⸻": ("other-dash", "three-em dash (U+2E3B)"),
}

# Filler and hype. The style guide lists the same phrases (a test keeps them in step).
BANNED_PHRASES: tuple[str, ...] = (
    "it's worth noting",
    "it is worth noting",
    "worth mentioning",
    "needless to say",
    "it goes without saying",
    "in conclusion",
    "in summary",
    "to sum up",
    "first and foremost",
    "at the end of the day",
    "delve",
    "delves",
    "delving",
    "dive into",
    "deep dive",
    "embark",
    "tapestry",
    "testament to",
    "plays a crucial role",
    "plays a vital role",
    "game-changer",
    "game changer",
    "must-have",
    "seamless",
    "seamlessly",
    "unleash",
    "look no further",
    "fear not",
    "whether you're",
    "nestled",
    "bustling",
    "vibrant",
    "amazing",
    "awesome",
    "incredible",
    "epic",
)

FIRST_PERSON: tuple[str, ...] = (
    "we",
    "we're",
    "we've",
    "we'll",
    "we'd",
    "us",
    "our",
    "ours",
    "ourselves",
    "let's",
    "i'm",
    "i've",
    "i'll",
    "i'd",
    "me",
    "my",
    "myself",
)

TERMS = {"mesos": "meso (never 'mesos')"}

# Proper nouns and fixed spellings that may be capitalised anywhere in a heading.
BUILTIN_NAMES = frozenset(
    {
        "MapleStory",
        "MapleThief",
        "Maple",
        "World",
        "Thief",
        "Edition",
        "Nexon",
        "Korean",
        "English",
        "Japanese",
        "Korea",
        "Japan",
        "iOS",
        "iPhone",
        "iPod",
        "iPad",
        "macOS",
        "Windows",
        "Linux",
        "Steam",
        "Deck",
        "Game",
        "Center",
        "App",
        "Store",
        "GitHub",
        "Bulbapedia",
        "StrategyWiki",
        "Serebii",
        "I",
    }
)

_EMOJI_RANGES = (
    (0x1F000, 0x1FAFF),
    (0x2600, 0x27BF),
    (0x2B00, 0x2BFF),
    (0xFE0F, 0xFE0F),
)

_WORD = re.compile(r"[A-Za-z][A-Za-z']*")
_TOKEN = re.compile(r"\x00+|[A-Za-z0-9][A-Za-z0-9'.]*|[:.!?]")
_DOUBLE_HYPHEN = re.compile(r"(?<!-)--(?!-)|(?<=\w)-{3,}(?=\w)|(?<= )-{3,}(?= )")
_RULE_LINE = re.compile(r"^\s*[-|:= ]+\s*$")


def _phrase_re(phrases: Iterable[str]) -> re.Pattern[str]:
    alts = sorted((re.escape(p) for p in phrases), key=len, reverse=True)
    return re.compile(r"(?<![\w'])(?:" + "|".join(alts) + r")(?![\w'])", re.IGNORECASE)


_BANNED = _phrase_re(BANNED_PHRASES)
_FIRST_PERSON = _phrase_re(FIRST_PERSON)
_TERMS = _phrase_re(TERMS)


@dataclass(frozen=True)
class Finding:
    """One rule broken at one place (1-based line and column)."""

    path: str
    line: int
    col: int
    rule: str
    message: str

    def __str__(self) -> str:
        return f"{self.path}:{self.line}:{self.col}: {self.rule}: {self.message}"


# -- names ----------------------------------------------------------------------------------


def glossary_names(style_md: str) -> set[str]:
    """Words of the first column of the table under the style guide's "Terms" heading."""
    names: set[str] = set()
    in_terms = False
    for line in style_md.splitlines():
        if line.startswith("#"):
            in_terms = line.lstrip("#").strip().lower().startswith("terms")
            continue
        if in_terms and line.startswith("|") and not _RULE_LINE.match(line):
            cells = line.strip().strip("|").split("|")
            names.update(_WORD.findall(cells[0]))
    return names


@functools.lru_cache(maxsize=1)
def known_names() -> frozenset[str]:
    """Built-in names plus the style guide's glossary (when the guide is there)."""
    names = set(BUILTIN_NAMES)
    try:
        names |= glossary_names(STYLE_GUIDE.read_text(encoding="utf-8"))
    except OSError:
        pass
    return frozenset(names)


# -- rules over masked text -----------------------------------------------------------------


def _is_emoji(ch: str) -> bool:
    cp = ord(ch)
    return any(lo <= cp <= hi for lo, hi in _EMOJI_RANGES)


def _check_line(text: str, line: int, col0: int, out: list[tuple[int, int, str, str]]) -> None:
    """Character-level rules over one line of masked prose; col0 is the 1-based column of
    text[0]."""
    for i, ch in enumerate(text):
        if ch in DASHES:
            rule, msg = DASHES[ch]
            out.append((line, col0 + i, rule, msg))
        elif _is_emoji(ch):
            out.append((line, col0 + i, "emoji", f"emoji or pictograph U+{ord(ch):04X}"))
    if not _RULE_LINE.match(text.replace(MASK, " ")):
        for m in _DOUBLE_HYPHEN.finditer(text):
            out.append((line, col0 + m.start(), "double-hyphen", "'--' used as a dash"))
    norm = text.replace("’", "'")
    for m in _BANNED.finditer(norm):
        out.append((line, col0 + m.start(), "banned-phrase", f"'{m.group(0)}' (style guide)"))
    for m in _FIRST_PERSON.finditer(norm):
        out.append(
            (line, col0 + m.start(), "first-person", f"'{m.group(0)}'; write in third person")
        )
    for m in _TERMS.finditer(norm):
        out.append(
            (line, col0 + m.start(), "term", f"'{m.group(0)}': write {TERMS[m.group(0).lower()]}")
        )


def heading_problem(text: str, names: frozenset[str] | set[str]) -> tuple[int, str] | None:
    """The offset and word of the first title-case word in a masked heading, or None.

    The first word may be capitalised, and so may the first word after ':', '.', '!' or '?'.
    A run of masked text counts as one word (an entity name or code).
    """
    sentence_start = True
    for m in _TOKEN.finditer(text):
        tok = m.group(0)
        if tok in ":.!?":
            sentence_start = True
            continue
        if tok.startswith(MASK):
            sentence_start = False
            continue
        word = tok.rstrip(".")
        start = sentence_start
        sentence_start = tok.endswith(".") and not word.endswith(("Mr", "Mrs", "Jr", "Dr", "St"))
        if start or not word[0].isupper():
            continue
        word = re.sub(r"'s?$", "", word)
        if word in names or all(_name_like(p, names) for p in re.split(r"[-']", word) if p):
            continue
        return m.start(), word
    return None


def _name_like(word: str, names: frozenset[str] | set[str]) -> bool:
    """A known name, an acronym (EXP, NPCs), a single letter (a button or key), a word with a
    digit, or one with capitals inside (MacArthur, UIKit): none of these is title case."""
    if word in names or len(word) == 1 or not word[0].isalpha() or any(c.isdigit() for c in word):
        return True
    stem = word[:-1] if word.endswith("s") and len(word) > 2 else word
    if stem.isupper():
        return True
    return any(c.isupper() for c in word[1:])


# -- Markdown --------------------------------------------------------------------------------

_MD_SPANS = (
    re.compile(r"(`+)(?:(?!\1).)+?\1"),  # code spans
    re.compile(r"\{\{.*?\}\}|\{%.*?%\}|\{#.*?#\}"),  # references and template tags
    re.compile(r"\]\([^)]*\)"),  # link targets
    re.compile(r"<(https?://|mailto:)[^>]*>"),  # autolinks
    re.compile(r"<(\w+)\b[^>]*\bdata-verbatim\b[^>]*>.*?</\1\s*>"),  # inline game text
    re.compile(r"<[A-Za-z/][^>]*>"),  # other inline tags (attributes are not prose)
    re.compile(r"<!--|-->"),  # comment delimiters
    re.compile(r"https?://\S+"),  # bare URLs
)
_FENCE = re.compile(r"^\s*(`{3,}|~{3,})")


def _mask(text: str, spans: Iterable[re.Pattern[str]]) -> str:
    chars = list(text)
    for pat in spans:
        for m in pat.finditer("".join(chars)):
            chars[m.start() : m.end()] = MASK * (m.end() - m.start())
    return "".join(chars)


def _lint_md(text: str, names: frozenset[str] | set[str]) -> list[tuple[int, int, str, str]]:
    out: list[tuple[int, int, str, str]] = []
    fence: str | None = None
    for n, raw in enumerate(text.splitlines(), start=1):
        fm = _FENCE.match(raw)
        if fence is not None:
            if fm and fm.group(1)[0] == fence[0] and len(fm.group(1)) >= len(fence):
                fence = None
            continue
        if fm:
            fence = fm.group(1)
            continue
        masked = _mask(raw, _MD_SPANS)
        _check_line(masked, n, 1, out)
        hm = re.match(r"^\s{0,3}(#{1,6})\s+(.*?)\s*#*\s*$", masked)
        if hm:
            heading = re.sub(r"\{#[^}]*\}\s*$", "", hm.group(2))
            problem = heading_problem(heading, names)
            if problem:
                off, word = problem
                out.append((n, hm.start(2) + off + 1, "title-case-heading", _heading_msg(word)))
    return out


def _heading_msg(word: str) -> str:
    return f"'{word}' capitalised; headings are sentence case (add real names to the glossary)"


# -- HTML ------------------------------------------------------------------------------------

_EXEMPT_TAGS = frozenset({"script", "style", "code", "pre", "textarea", "svg", "math"})
_VOID_TAGS = frozenset(
    {
        "area",
        "base",
        "br",
        "col",
        "embed",
        "hr",
        "img",
        "input",
        "link",
        "meta",
        "source",
        "track",
        "wbr",
    }
)
_HEADINGS = frozenset({"h1", "h2", "h3", "h4", "h5", "h6"})


class _HtmlLinter(HTMLParser):
    def __init__(self, names: frozenset[str] | set[str]) -> None:
        super().__init__(convert_charrefs=True)
        self.names = names
        self.out: list[tuple[int, int, str, str]] = []
        self.stack: list[tuple[str, bool]] = []  # (tag, exempt)
        self.heading: list[str] | None = None
        self.heading_pos = (0, 0)

    @property
    def exempt(self) -> bool:
        return any(ex for _, ex in self.stack)

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        if tag in _VOID_TAGS:
            return
        exempt = tag in _EXEMPT_TAGS or any(name == "data-verbatim" for name, _ in attrs)
        self.stack.append((tag, exempt))
        if tag in _HEADINGS and self.heading is None:
            self.heading = []
            self.heading_pos = self.getpos()

    def handle_startendtag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        pass

    def handle_endtag(self, tag: str) -> None:
        if not any(t == tag for t, _ in self.stack):
            return
        while self.stack:
            t, _ = self.stack.pop()
            if t == tag:
                break
        if tag in _HEADINGS and self.heading is not None:
            text = " ".join("".join(self.heading).split())
            problem = heading_problem(text, self.names)
            if problem:
                line, col = self.heading_pos
                self.out.append((line, col + 1, "title-case-heading", _heading_msg(problem[1])))
            self.heading = None

    def handle_data(self, data: str) -> None:
        exempt = self.exempt
        if self.heading is not None:
            self.heading.append(MASK if exempt else data)
        if exempt:
            return
        line, col = self.getpos()
        for i, part in enumerate(data.split("\n")):
            _check_line(part, line + i, (col if i == 0 else 0) + 1, self.out)


def _lint_html(text: str, names: frozenset[str] | set[str]) -> list[tuple[int, int, str, str]]:
    p = _HtmlLinter(names)
    p.feed(text)
    p.close()
    return p.out


# -- API -------------------------------------------------------------------------------------


def lint_text(
    text: str,
    kind: str = "md",
    *,
    path: str = "<text>",
    names: Iterable[str] | None = None,
) -> list[Finding]:
    """Lint one document. ``kind`` is ``"md"`` or ``"html"``; ``names`` adds proper nouns the
    heading rule accepts (on top of :func:`known_names`)."""
    allowed = known_names() if names is None else known_names() | frozenset(names)
    if kind == "md":
        raw = _lint_md(text, allowed)
    elif kind == "html":
        raw = _lint_html(text, allowed)
    else:
        raise ValueError(f"kind must be 'md' or 'html', not {kind!r}")
    return [Finding(path, line, col, rule, msg) for line, col, rule, msg in sorted(raw)]


def kind_of(path: Path) -> str:
    return "html" if path.suffix.lower() in (".html", ".htm") else "md"


def lint_file(path: Path, names: Iterable[str] | None = None) -> list[Finding]:
    text = path.read_text(encoding="utf-8")
    return lint_text(text, kind_of(path), path=str(path), names=names)


def default_paths(root: Path = ROOT) -> list[Path]:
    """Tracked compendium Markdown plus the site generator's templates, if any."""
    try:
        res = subprocess.run(
            ["git", "ls-files", "-z", "--", "docs/compendium/*.md", "docs/compendium/**/*.md"],
            cwd=root,
            capture_output=True,
            check=True,
        )
        paths = sorted({root / p for p in res.stdout.decode().split("\0") if p})
    except (OSError, subprocess.CalledProcessError):
        paths = sorted((root / "docs" / "compendium").rglob("*.md"))
    templates = root / "tools" / "mtport" / "compendium" / "templates"
    if templates.is_dir():
        paths += sorted(p for p in templates.rglob("*") if p.is_file())
    return paths


def expand(paths: Sequence[Path]) -> list[Path]:
    files: list[Path] = []
    for p in paths:
        if p.is_dir():
            files += sorted(
                f
                for f in p.rglob("*")
                if f.is_file() and f.suffix.lower() in (".md", ".html", ".htm")
            )
        else:
            files.append(p)
    return files


def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="mtport.prose_lint", description=__doc__.split("\n\n")[0])
    ap.add_argument(
        "paths", nargs="*", type=Path, help="files or directories (default: the compendium)"
    )
    args = ap.parse_args(argv)
    files = expand(args.paths) if args.paths else default_paths()
    findings: list[Finding] = []
    for f in files:
        try:
            findings += lint_file(f)
        except (OSError, UnicodeDecodeError) as e:
            print(f"{f}: cannot read: {e}", file=sys.stderr)
            return 2
    for finding in findings:
        print(finding)
    print(f"prose_lint: {len(files)} file(s), {len(findings)} finding(s)", file=sys.stderr)
    return 1 if findings else 0


if __name__ == "__main__":
    raise SystemExit(main())
