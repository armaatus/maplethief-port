"""The compendium's prose format (ADR 0122): a small Markdown subset with entity references.

Hand-written pages live in ``docs/compendium/pages/**/*.md``. They are our own words and
never quote game text: a game name comes from the knowledge base through a reference.

Blocks (separated by blank lines):

- ``# Title`` on the first line: the page title (not repeated in the body); ``##`` to
  ``####``: headings, each with an anchor made from its text;
- paragraphs (consecutive lines are joined);
- lists: ``- item`` (or ``* item``) and ``1. item``, one level; a line indented by two or
  more spaces continues the item;
- ``> quote``: a block quote;
- tables: a ``| a | b |`` header line, a ``|---|---|`` line, then rows;
- ``<!-- comment -->`` lines are dropped.

Inline: ``**bold**``, ``*emphasis*``, ```code```, ``[label](target)`` where the target is a
site path (``mechanics/drops.html#rates``, from the site root), a same-page ``#anchor`` or an
``https://`` URL, and the entity references ``{{kind:id}}`` (a link labelled with the
entity's English name) and ``{{kind:id|label}}`` (our own label). A reference to an entity
the knowledge base does not hold is a build error, as is anything the subset does not know
(raw HTML, a heading level 1 after the first line).
"""

from __future__ import annotations

import re
from typing import TYPE_CHECKING

from mtport.compendium.siteapi import BuildError, path_marker
from mtport.htmlpage import esc

if TYPE_CHECKING:
    from mtport.compendium.siteapi import Context

INLINE = re.compile(
    r"\{\{(?P<ref>[^{}]*)\}\}"
    r"|`(?P<code>[^`]+)`"
    r"|\[(?P<label>[^\]]+)\]\((?P<target>[^)\s]+)\)"
    r"|\*\*(?P<strong>.+?)\*\*"
    r"|\*(?P<em>[^*\s][^*]*?)\*"
)
HEADING = re.compile(r"^(#{1,6})\s+(.*?)\s*#*\s*$")
ULIST = re.compile(r"^[-*]\s+(.*)$")
OLIST = re.compile(r"^\d+\.\s+(.*)$")
TABLE_SEP = re.compile(r"^\|?\s*:?-{3,}:?\s*(\|\s*:?-{3,}:?\s*)*\|?\s*$")
RAW_HTML = re.compile(r"<\s*/?\s*[A-Za-z!]")


def heading_id(value: str) -> str:
    s = re.sub(r"[^a-z0-9]+", "-", value.lower()).strip("-")
    return s or "section"


class _Renderer:
    def __init__(self, ctx: Context, source: str):
        self.ctx = ctx
        self.source = source
        self.line = 0
        self.ids: dict[str, int] = {}

    def fail(self, message: str) -> BuildError:
        return BuildError(f"{self.source}:{self.line}: {message}")

    # inline
    def inline(self, text: str) -> str:
        if RAW_HTML.search(text):
            raise self.fail("raw HTML is not part of the prose format")
        out, pos = [], 0
        for m in INLINE.finditer(text):
            out.append(esc(text[pos : m.start()]))
            pos = m.end()
            if m.group("ref") is not None:
                out.append(self.ref(m.group("ref")))
            elif m.group("code") is not None:
                out.append(f"<code>{esc(m.group('code'))}</code>")
            elif m.group("label") is not None:
                out.append(self.link(m.group("label"), m.group("target")))
            elif m.group("strong") is not None:
                out.append(f"<strong>{self.inline(m.group('strong'))}</strong>")
            else:
                out.append(f"<em>{self.inline(m.group('em'))}</em>")
        out.append(esc(text[pos:]))
        return "".join(out)

    def ref(self, body: str) -> str:
        target, _, label = body.partition("|")
        kind, sep, eid = target.strip().partition(":")
        if not sep or not kind or not eid:
            raise self.fail(f"bad entity reference {{{{{body}}}}}: use {{{{kind:id}}}}")
        if self.ctx.kb.get(kind, eid) is None:
            raise self.fail(f"unknown entity reference {{{{{target.strip()}}}}}")
        return self.ctx.ref(kind, eid, label.strip() or None)

    def link(self, label: str, target: str) -> str:
        if target.startswith(("https://", "http://")):
            href = esc(target)
        elif target.startswith("#"):
            href = esc(target)
        else:
            try:
                href = path_marker(target)
            except BuildError as exc:
                raise self.fail(str(exc)) from None
        return f'<a href="{href}">{self.inline(label)}</a>'

    def heading(self, level: int, text: str) -> str:
        base = heading_id(text)
        n = self.ids.get(base, 0) + 1
        self.ids[base] = n
        hid = base if n == 1 else f"{base}-{n}"
        return f'<h{level} id="{hid}">{self.inline(text)}</h{level}>'

    def table(self, rows: list[str]) -> str:
        def cells(row: str) -> list[str]:
            row = row.strip()
            if row.startswith("|"):
                row = row[1:]
            if row.endswith("|"):
                row = row[:-1]
            return [c.strip() for c in row.split("|")]

        head = cells(rows[0])
        out = ['<div class="wrap"><table class="prose-table">', "<thead><tr>"]
        out += [f"<th>{self.inline(c)}</th>" for c in head]
        out.append("</tr></thead><tbody>")
        for r in rows[2:]:
            cs = cells(r)
            if len(cs) != len(head):
                raise self.fail(f"table row has {len(cs)} cells, the header {len(head)}")
            out.append("<tr>" + "".join(f"<td>{self.inline(c)}</td>" for c in cs) + "</tr>")
        out.append("</tbody></table></div>")
        return "".join(out)


def render(text: str, ctx: Context, source: str = "<prose>") -> tuple[str, str]:
    """Render prose: ``(title, body markup)``. ``title`` is the first line's ``# `` heading,
    or ``""``. Raises :class:`BuildError` (with ``source:line``) on an unknown reference or
    anything outside the subset."""
    r = _Renderer(ctx, source)
    lines = text.replace("\r\n", "\n").split("\n")
    title = ""
    blocks: list[str] = []
    i = 0
    in_comment = False
    # first non-blank line may be the title
    while i < len(lines) and not lines[i].strip():
        i += 1
    if i < len(lines):
        m = HEADING.match(lines[i])
        if m and len(m.group(1)) == 1:
            r.line = i + 1
            if RAW_HTML.search(m.group(2)) or INLINE.search(m.group(2)):
                raise r.fail("the title is plain text")
            title = m.group(2)
            i += 1
    while i < len(lines):
        line = lines[i]
        r.line = i + 1
        stripped = line.strip()
        if in_comment or stripped.startswith("<!--"):
            in_comment = "-->" not in stripped
            i += 1
            continue
        if not stripped:
            i += 1
            continue
        m = HEADING.match(stripped)
        if m:
            level = len(m.group(1))
            if level == 1:
                raise r.fail("only the first line may be a # title; use ## for sections")
            blocks.append(r.heading(min(level, 6), m.group(2)))
            i += 1
            continue
        if stripped.startswith("|") and i + 1 < len(lines) and TABLE_SEP.match(lines[i + 1]):
            rows = []
            while i < len(lines) and lines[i].strip().startswith("|"):
                rows.append(lines[i])
                i += 1
            blocks.append(r.table(rows))
            continue
        if ULIST.match(stripped) or OLIST.match(stripped):
            ordered = OLIST.match(stripped) is not None
            pat = OLIST if ordered else ULIST
            items: list[str] = []
            while i < len(lines):
                cur = lines[i]
                r.line = i + 1
                lm = pat.match(cur.strip()) if not cur.startswith(" ") else None
                if lm:
                    items.append(lm.group(1))
                elif cur.startswith("  ") and cur.strip() and items:
                    items[-1] += " " + cur.strip()
                else:
                    break
                i += 1
            tag = "ol" if ordered else "ul"
            blocks.append(
                f"<{tag}>" + "".join(f"<li>{r.inline(t)}</li>" for t in items) + f"</{tag}>"
            )
            continue
        if stripped.startswith(">"):
            quote = []
            while i < len(lines) and lines[i].strip().startswith(">"):
                quote.append(lines[i].strip()[1:].strip())
                i += 1
            blocks.append(f"<blockquote><p>{r.inline(' '.join(quote))}</p></blockquote>")
            continue
        para = []
        while i < len(lines):
            cur = lines[i].strip()
            if not cur or HEADING.match(cur) or ULIST.match(cur) or OLIST.match(cur):
                break
            if cur.startswith((">", "|", "<!--")):
                break
            para.append(cur)
            i += 1
        r.line = i
        blocks.append(f"<p>{r.inline(' '.join(para))}</p>")
    return title, "\n".join(blocks) + ("\n" if blocks else "")
