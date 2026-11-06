"""The "About the game" section (#225): history, credits, localization, unused content, the
original bugs, the online features, a technical overview, the glossary, and the list of the
game's scripts.

Most pages are hand-written prose (``docs/compendium/pages/about/*.md``). This module adds
what comes from the knowledge base at build time:

- the **scripts list** (``about/scripts.html``): every ``script`` entity as an anchored row;
- the **unused scripts** on the unused-content page: why each is unused, the quests it
  prepares and its text (game text, ``data-verbatim``; Korean-only literals are marked);
- the **localization** figures (names and dialogue in English and Korean);
- the **glossary**, read from the terms table of the style guide (``docs/compendium/style.md``).
"""

from __future__ import annotations

import re
from collections.abc import Iterator
from pathlib import Path

from mtport.compendium import templates as t
from mtport.compendium.siteapi import Context, Entity, Page, Section, verbatim

SECTION_ID = "about"
STYLE_GUIDE = Path(__file__).resolve().parents[4] / "docs" / "compendium" / "style.md"

# The sub-pages in reading order: (file stem, title, one line for the index; our words).
PAGES = (
    ("history", "History", "What the game is, who made it, and its versions and releases."),
    ("credits", "Credits", "What the game says about its makers."),
    (
        "localization",
        "Localization",
        "The English, Korean and Japanese parts of the game and how they differ.",
    ),
    ("unused", "Unused content", "Scripts, data and images the game ships but never uses."),
    ("bugs", "Bugs and glitches", "The original game's own bugs, as a player meets them."),
    ("online", "Online features", "Ads, purchases and Game Center, then and now."),
    ("technical", "Technical overview", "How the game's files and engine work."),
    ("scripts", "Scripts", "Every script file of the game, with where it runs."),
    ("glossary", "Glossary", "The game's own names, in English and Korean."),
)

HANGUL = re.compile("[가-힣]")
SCRIPT_FILE = re.compile(r"\b((?:chapter\d|common)/[A-Za-z0-9_]+)")


def page_path(stem: str) -> str:
    return f"{SECTION_ID}/{stem}.html"


def script_anchor(sid: str) -> str:
    return "script-" + t.anchor_id(sid)


# --- scripts ---------------------------------------------------------------------------------


def is_unused(e: Entity) -> bool:
    return not e.get("used")


def unused_group(e: Entity) -> str:
    """Why a script is unused, in three groups: ``unnamed`` (no map object names it),
    ``unreachable`` (only maps no chapter reaches name it) and ``overridden`` (a chapter's
    file of the same name runs instead)."""
    why = str(e.get("unused_reason") or "")
    if why.startswith("no map object"):
        return "unnamed"
    if why.startswith("no map that"):
        return "unreachable"
    return "overridden"


GROUP_TEXT = {
    "unnamed": "No map object names it",
    "unreachable": "Only maps no chapter reaches name it",
    "overridden": "A chapter's file of the same name runs instead",
}


def text_lines(e: Entity) -> list[dict]:
    rows = e.get("text") or []
    return [r for r in rows if isinstance(r, dict) and (r.get("en") or r.get("ko"))]


def korean_only(row: dict) -> bool:
    """A literal string in the bytecode (not a key of ``Scripts.strings``): the game shows it
    as written, so it is Korean in every language."""
    en, ko = row.get("en"), row.get("ko")
    return bool(ko) and en == ko and bool(HANGUL.search(ko)) and row.get("key") == ko


def _script_link(ctx: Context, rel: str) -> str:
    sid = rel.replace("/", "-")
    if ctx.kb.get("script", sid) is None:
        return f"<code>{t.esc(rel)}</code>"
    return f'<a href="{ctx.href("script", sid)}"><code>{t.esc(rel)}</code></a>'


def reason_markup(ctx: Context, e: Entity) -> str:
    """The unused reason with the scripts it names linked."""
    why = str(e.get("unused_reason") or "")
    out, pos = [], 0
    for m in SCRIPT_FILE.finditer(why):
        out.append(t.esc(why[pos : m.start()]))
        out.append(_script_link(ctx, m.group(1)))
        pos = m.end()
    out.append(t.esc(why[pos:]))
    return "".join(out)


def _rel(e: Entity) -> str:
    return f"{e.get('dir')}/{e.get('base_name')}"


def _chapters(e: Entity) -> str:
    cs = [str(c) for c in e.get("chapters") or []]
    if not cs:
        return ""
    return ", ".join(cs)


def scripts_page(ctx: Context) -> Page:
    rows, ids = [], []
    page = Page(page_path("scripts"), "Scripts", "", description="Every script of the game.")
    scripts = ctx.kb.entities("script")
    for e in scripts:
        sid = str(e["id"])
        anchor = script_anchor(sid)
        page.documents("script", sid, anchor)
        ids.append(anchor)
        if is_unused(e):
            status = t.markup_cell(
                f'<a href="{ctx.url(page_path("unused") + "#" + anchor)}">unused</a>', "unused"
            )
        else:
            cs = e.get("chapters") or []
            word = "chapter " if len(cs) == 1 else "chapters "
            status = t.text_cell(word + _chapters(e) if cs else "used")
        maps = [m for m in e.get("maps") or [] if isinstance(m, dict)]
        seen: list[str] = []
        for m in maps:
            if m.get("id") not in seen and ctx.kb.get("map", str(m.get("id"))) is not None:
                seen.append(str(m["id"]))
        where = ", ".join(ctx.ref("map", m) for m in seen[:3])
        if len(seen) > 3:
            where += f" and {len(seen) - 3} more"
        dialogue = [d for d in e.get("dialogue") or [] if isinstance(d, dict)]
        talk = ""
        if dialogue and ctx.kb.get("dialogue", str(dialogue[0].get("id"))) is not None:
            talk = ctx.ref("dialogue", str(dialogue[0]["id"]), "Dialogue")
        rows.append(
            [
                t.markup_cell(f"<code>{t.esc(_rel(e))}</code>", _rel(e)),
                status,
                t.text_cell(", ".join(e.get("roles") or [])),
                t.markup_cell(where, len(seen)),
                t.markup_cell(talk),
            ]
        )
    unused = sum(1 for e in scripts if is_unused(e))
    lead = ctx.prose("about/scripts.md") or ""
    counts = ""
    if scripts:
        more = (
            f' (<a href="{ctx.url(page_path("unused") + "#unused-scripts")}">unused scripts</a>)'
            if unused
            else ""
        )
        counts = (
            f"<p>The game ships {t.number(len(scripts))} script files. "
            f"{t.number(len(scripts) - unused)} run in some chapter and {t.number(unused)}"
            f" never run{more}.</p>\n"
        )
    cols = [
        t.Column("Script", "The file under script/, without .out"),
        t.Column("Runs in", "The chapters where the game runs this file"),
        t.Column("Role", "What names it: an NPC, a portal or a map"),
        t.Column("Maps", "The maps whose objects run it"),
        t.Column("Dialogue"),
    ]
    page.body = lead + counts + t.table(cols, rows, ids)
    page.crumbs = []
    return page


def _quests_markup(e: Entity) -> str:
    rows = [q for q in e.get("unused_quests") or [] if isinstance(q, dict)]
    if not rows:
        return ""
    texts = {r.get("key"): r for r in text_lines(e)}
    items = []
    for q in rows:
        kind = "main" if q.get("kind") == 0 else "sub"
        title = q.get("title_key")
        shown = ""
        if isinstance(title, str) and title:
            row = texts.get(title)
            if row is not None and row.get("en"):
                shown = " " + verbatim(row["en"], "en")
                if row.get("ko") and row["ko"] != row["en"]:
                    shown += " (" + verbatim(row["ko"], "ko") + ")"
            elif HANGUL.search(title):
                shown = " " + verbatim(title, "ko")
        items.append(f"<li>{kind} quest {t.esc(str(q.get('number')))}{shown}</li>")
    return "<p>Quests it prepares:</p>\n<ul>" + "".join(items) + "</ul>\n"


def _text_markup(e: Entity) -> str:
    rows = text_lines(e)
    if not rows:
        return ""
    ko_only = [r for r in rows if korean_only(r)]
    out = []
    if len(ko_only) == len(rows):
        out.append(
            '<p class="note">This text is Korean only: it is written into the script itself'
            " instead of being a key of the strings files, so the game would show it as"
            " written in every language.</p>\n"
        )
        out.append(
            '<ol class="script-text" lang="ko">'
            + "".join(f"<li>{verbatim(r['ko'], 'ko')}</li>" for r in rows)
            + "</ol>\n"
        )
        return "".join(out)
    if ko_only:
        out.append(
            f'<p class="note">{t.number(len(ko_only))} of these lines are Korean only (written'
            " into the script itself).</p>\n"
        )
    out.append(
        t.dialogue(
            [t.Line("", r.get("en") or "", r.get("ko") or "") for r in rows], label="Script text"
        )
    )
    return "".join(out)


def unused_scripts(ctx: Context) -> str:
    """The unused scripts part of the unused-content page."""
    unused = [e for e in ctx.kb.entities("script") if is_unused(e)]
    if not unused:
        return ""
    groups = {g: [e for e in unused if unused_group(e) == g] for g in GROUP_TEXT}
    total = len(ctx.kb.entities("script"))
    literal = sum(1 for e in unused for r in text_lines(e) if korean_only(r))
    parts = [
        f"<p>{t.number(len(unused))} of the {t.number(total)} script files never run"
        f' (<a href="{ctx.url(page_path("scripts"))}">every script</a>).'
        f" No map object names {t.number(len(groups['unnamed']))} of them;"
        f" {t.number(len(groups['unreachable']))} are named only on maps that no chapter"
        f" reaches; and for {t.number(len(groups['overridden']))}, a file of the same name in"
        " a chapter's directory runs in every chapter that reaches a map naming them."
        f" The unused scripts hold {t.number(literal)} distinct Korean sentences written into"
        " the bytecode instead of keys (127 with repeats,"
        ' <a href="https://github.com/armaatus/maplethief-port/blob/main/docs/formats/'
        'script-out.md">formats/script-out.md</a>).</p>\n',
        "<p><strong>Evidence:</strong> the knowledge base walks every path of every script and"
        " follows every warp from the start of each chapter (an upper bound on what the game"
        ' can run), <a href="https://github.com/armaatus/maplethief-port/blob/main/docs/'
        'compendium/kb.md#the-quests-stage">the quests stage</a>. A script that no map object'
        " names is certainly unused; the other two groups are probable.</p>\n",
    ]
    cols = [
        t.Column("Script"),
        t.Column("Why it never runs"),
        t.Column("Lines", "Distinct lines of text it uses", numeric=True),
    ]
    rows = []
    for e in unused:
        sid = str(e["id"])
        rows.append(
            [
                t.markup_cell(
                    f'<a href="#{script_anchor(sid)}"><code>{t.esc(_rel(e))}</code></a>', _rel(e)
                ),
                t.markup_cell(reason_markup(ctx, e), unused_group(e)),
                t.num_cell(len(text_lines(e))),
            ]
        )
    parts.append(t.table(cols, rows))
    for e in unused:
        sid = str(e["id"])
        anchor = script_anchor(sid)
        body = [
            f"<p>{t.esc(GROUP_TEXT[unused_group(e)])}: {reason_markup(ctx, e)}."
            f' Its row in the <a href="{ctx.href("script", sid)}">scripts list</a>.</p>\n'
        ]
        fns = [str(f) for f in e.get("functions") or []]
        if fns:
            body.append(
                f"<p>Functions ({t.number(len(fns))}): "
                + ", ".join(f"<code>{t.esc(f)}</code>" for f in fns)
                + ".</p>\n"
            )
        body.append(_quests_markup(e))
        notes = [str(n) for n in e.get("notes") or []]
        if notes:
            body.append(
                "<p>Slips in the script itself:</p><ul>"
                + "".join(f"<li><code>{t.esc(n)}</code></li>" for n in notes)
                + "</ul>\n"
            )
        body.append(_text_markup(e))
        parts.append(
            f'<section class="unused-script" aria-labelledby="{anchor}">\n'
            f'<h3 id="{anchor}"><code>{t.esc(_rel(e))}</code></h3>\n'
            + "".join(body)
            + "</section>\n"
        )
    return t.section("Unused scripts", "".join(parts), "unused-scripts")


# --- localization -----------------------------------------------------------------------------


def localization_figures(ctx: Context) -> str:
    """Counts from the knowledge base: names in both languages, and the dialogue's length."""
    rows = []
    for kind, label in (
        ("monster", "Monsters"),
        ("item", "Items"),
        ("npc", "NPCs"),
        ("map", "Maps"),
        ("skill", "Skills"),
        ("quest", "Quests"),
    ):
        es = ctx.kb.entities(kind)
        if not es:
            continue
        both = same = 0
        for e in es:
            n = e.get("name")
            if isinstance(n, dict) and n.get("en") and n.get("ko"):
                both += 1
                same += n["en"] == n["ko"]
        rows.append([t.text_cell(label), t.num_cell(len(es)), t.num_cell(both), t.num_cell(same)])
    out = []
    if rows:
        out.append(
            t.table(
                [
                    t.Column("Kind"),
                    t.Column("Entries", numeric=True),
                    t.Column("Named in both", numeric=True),
                    t.Column("Same in both", "The English and Korean name are equal", True),
                ],
                rows,
                compact=True,
            )
        )
    en = ko = pairs = 0
    missing: list[tuple[str, str]] = []
    for d in ctx.kb.entities("dialogue"):
        for row in (d.get("text") or {}).values():
            if not isinstance(row, dict):
                continue
            if row.get("en") and row.get("ko"):
                pairs += 1
                en += len(row["en"])
                ko += len(row["ko"])
        for key in d.get("missing_text") or []:
            missing.append((str(d["id"]), str(key)))
    if pairs:
        ratio = en / ko if ko else 0
        out.append(
            f"<p>The dialogue of the scripts that run has {t.number(pairs)} lines in both"
            f" languages: {t.number(en)} characters in English and {t.number(ko)} in Korean,"
            f" so an English line is {t.number(round(ratio, 2))} times as long as its Korean"
            " line on average (Hangul packs a syllable into one character).</p>\n"
        )
    if missing:
        items = "".join(
            f"<li>{ctx.ref('dialogue', d)}: <code>{t.esc(k)}</code></li>" for d, k in missing
        )
        out.append(
            "<p>Keys of the dialogue that lack a language (the game would show the key"
            f" itself):</p><ul>{items}</ul>\n"
        )
    return t.section("Figures", "".join(out), "figures") if out else ""


# --- glossary ---------------------------------------------------------------------------------


def glossary_rows(style_text: str) -> list[list[str]]:
    """The rows of the style guide's terms table (under ``## Terms``): English, Korean, kind
    and source cells, as written."""
    lines = style_text.splitlines()
    try:
        start = lines.index("## Terms")
    except ValueError:
        return []
    rows: list[list[str]] = []
    seen_head = False
    for line in lines[start + 1 :]:
        s = line.strip()
        if s.startswith("## "):
            break
        if not s.startswith("|"):
            if rows:
                break
            continue
        cells = [c.strip() for c in s.strip("|").split("|")]
        if not seen_head:
            seen_head = True
            continue
        if set("".join(cells)) <= set("-: "):
            continue
        rows.append(cells)
    return rows


def _source_markup(src: str) -> str:
    out, pos = [], 0
    for m in re.finditer(r"`([^`]+)`", src):
        out.append(t.esc(src[pos : m.start()]))
        out.append(f"<code>{t.esc(m.group(1))}</code>")
        pos = m.end()
    out.append(t.esc(src[pos:]))
    return "".join(out)


def glossary_page(ctx: Context) -> Page:
    try:
        text = STYLE_GUIDE.read_text(encoding="utf-8")
    except OSError:
        text = ""
    rows = []
    for cells in glossary_rows(text):
        cells = (cells + ["", "", "", ""])[:4]
        en, ko, kind, src = cells
        rows.append(
            [
                t.markup_cell(verbatim(en, "en") if en else "", en),
                t.Cell(verbatim(ko, "ko") if ko else "", ko, "ko"),
                t.text_cell(kind),
                t.markup_cell(_source_markup(src), src),
            ]
        )
    body = ctx.prose("about/glossary.md") or ""
    if rows:
        body += t.table(
            [t.Column("English"), t.Column("Korean"), t.Column("What it is"), t.Column("Source")],
            rows,
            [f"term-{t.anchor_id(r[0].key or '')}" for r in rows],
        )
    return Page(page_path("glossary"), "Glossary", body, description="The game's own names.")


# --- pages ------------------------------------------------------------------------------------


def _prose_page(ctx: Context, stem: str, title: str, blurb: str, extra: str = "") -> Page:
    body = ctx.prose(f"about/{stem}.md")
    if body is None:
        body = '<p class="muted">This page is being written.</p>\n'
    return Page(page_path(stem), title, body + extra, description=blurb)


def pages(ctx: Context) -> Iterator[Page]:
    links = "".join(
        f'<li><a href="{ctx.url(page_path(stem))}">{t.esc(title)}</a>: {t.esc(blurb)}</li>'
        for stem, title, blurb in PAGES
    )
    intro = ctx.prose("about/index.md") or ""
    yield Page(
        f"{SECTION_ID}/index.html",
        "About the game",
        intro + t.section("Pages", f'<ul class="pages">{links}</ul>\n', "pages"),
        description="History, versions, unused content and the original bugs.",
    )
    for stem, title, blurb in PAGES:
        if stem == "scripts":
            yield scripts_page(ctx)
        elif stem == "glossary":
            yield glossary_page(ctx)
        elif stem == "unused":
            yield _prose_page(ctx, stem, title, blurb, unused_scripts(ctx))
        elif stem == "localization":
            yield _prose_page(ctx, stem, title, blurb, localization_figures(ctx))
        else:
            yield _prose_page(ctx, stem, title, blurb)


SECTION = Section(
    SECTION_ID,
    "About the game",
    90,
    ("script",),
    pages,
    blurb="History, versions, unused content and the original bugs.",
    image="item:etc:25",
)
