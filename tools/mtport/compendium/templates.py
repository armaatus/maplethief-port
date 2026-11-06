"""Shared page templates of the compendium site (ADR 0122).

Every function returns finished markup (it may hold link markers from
:class:`~mtport.compendium.siteapi.Context`). Arguments named ``*_markup`` must already be
safe HTML; plain ``str`` arguments are escaped here.

- :func:`infobox`: the box of key facts beside an entity page's text;
- :func:`table`: a sortable, filterable table (``htmlpage.table``, re-exported with its
  :class:`Column` and :class:`Cell` helpers);
- :func:`drop_table`: drops with the percentage and the game's 1/1000 value;
- :func:`dialogue`: a conversation in English and Korean with a language toggle;
- :func:`gallery`: images with captions;
- :func:`see_also`: a list of related links;
- :func:`section`: a titled part of a page with an anchor;
- :func:`svg_line_chart`: a small inline SVG chart (a curve such as EXP per level).
"""

from __future__ import annotations

from collections.abc import Iterable, Sequence
from dataclasses import dataclass

from mtport import htmlpage as hp
from mtport.compendium.siteapi import verbatim

Column = hp.Column
Cell = hp.Cell
text_cell = hp.text
num_cell = hp.num
esc = hp.esc


def markup_cell(markup: str, key: str | int | float | None = None) -> Cell:
    """A cell of safe markup (a link, a verbatim name), sorted by ``key`` or its text."""
    return Cell(markup, key)


def table(columns: Sequence[Column], rows: Sequence[Sequence[Cell]], row_ids=None) -> str:
    """A sortable, filterable table; ``row_ids`` give each row an anchor."""
    return hp.table(columns, rows, row_ids)


def anchor_id(value: str) -> str:
    """An element id made from any string (ids, names): letters, digits, ``-`` and ``_``."""
    out = "".join(c if c.isalnum() and c.isascii() or c in "-_" else "-" for c in str(value))
    return out.strip("-") or "x"


def section(title: str, body_markup: str, anchor: str = "", level: int = 2) -> str:
    """``<section>`` with a heading (our words, escaped) and an anchor from the title."""
    a = anchor or anchor_id(title.lower())
    return (
        f'<section aria-labelledby="{esc(a)}">\n<h{level} id="{esc(a)}">{esc(title)}</h{level}>\n'
        f"{body_markup}</section>\n"
    )


def infobox(
    heading_markup: str,
    rows: Iterable[tuple[str, str]],
    image_markup: str = "",
    caption_markup: str = "",
) -> str:
    """The infobox: a heading (often :meth:`Context.name`), an optional picture and rows of
    ``(label, value markup)``. Labels are our words, escaped."""
    body = [
        f'<aside class="infobox" aria-label="Key facts">\n<p class="ib-head">{heading_markup}</p>'
    ]
    if image_markup:
        cap = f"<figcaption>{caption_markup}</figcaption>" if caption_markup else ""
        body.append(f'<figure class="ib-image">{image_markup}{cap}</figure>')
    items = [f"<dt>{esc(label)}</dt><dd>{value}</dd>" for label, value in rows]
    if items:
        body.append("<dl>" + "".join(items) + "</dl>")
    body.append("</aside>\n")
    return "\n".join(body)


@dataclass(frozen=True)
class Drop:
    """One drop: the item (link markup), its chance in the game's 1/1000 units, a note."""

    item_markup: str
    per_1000: int
    note: str = ""


def percent(per_1000: int | float) -> str:
    """A 1/1000 chance as a percentage: ``25`` gives ``2.5%``."""
    value = per_1000 / 10
    return (f"{value:.1f}".rstrip("0").rstrip(".") if value % 1 else f"{value:.0f}") + "%"


def drop_table(drops: Iterable[Drop], item_title: str = "Item") -> str:
    """A table of drops: item, chance as a percentage and the 1/1000 value, note."""
    drops = list(drops)
    columns = [
        Column(item_title),
        Column("Chance", "Chance per kill", numeric=True),
        Column("Per 1000", "The game's own value, in 1/1000", numeric=True),
        Column("Note"),
    ]
    rows = [
        [
            markup_cell(d.item_markup),
            Cell(esc(percent(d.per_1000)), d.per_1000),
            num_cell(d.per_1000),
            text_cell(d.note),
        ]
        for d in drops
    ]
    return '<div class="drops">' + table(columns, rows) + "</div>\n"


@dataclass(frozen=True)
class Line:
    """One line of dialogue: speaker markup (a link or a name), English and Korean text."""

    speaker_markup: str
    en: str
    ko: str = ""


def dialogue(lines: Iterable[Line], label: str = "Dialogue") -> str:
    """A conversation shown in English, Korean or both; the toggle needs the site script and
    without it both languages show. The text is game text (``data-verbatim``)."""
    out = [
        f'<div class="dialogue" data-show="both" role="group" aria-label="{esc(label)}">',
        '<p class="lang-toggle" hidden>Show: '
        '<button type="button" data-lang="en">English</button> '
        '<button type="button" data-lang="ko">Korean</button> '
        '<button type="button" data-lang="both" aria-pressed="true">Both</button></p>',
        "<ol>",
    ]
    for ln in lines:
        who = f'<span class="who">{ln.speaker_markup}</span>' if ln.speaker_markup else ""
        ko = verbatim(ln.ko, "ko", "span", "ko") if ln.ko else ""
        out.append(f"<li>{who}{verbatim(ln.en, 'en', 'span', 'en')}{ko}</li>")
    out.append("</ol>\n</div>\n")
    return "\n".join(out)


def gallery(figures: Iterable[tuple[str, str]], label: str = "Images") -> str:
    """Pictures with captions: ``(img markup, caption markup)``. Empty without figures."""
    items = [
        f"<figure>{img}" + (f"<figcaption>{cap}</figcaption>" if cap else "") + "</figure>"
        for img, cap in figures
    ]
    if not items:
        return ""
    return (
        f'<div class="gallery" role="group" aria-label="{esc(label)}">'
        + "".join(items)
        + "</div>\n"
    )


def see_also(links_markup: Iterable[str], title: str = "See also") -> str:
    """A "See also" section from link markup; empty without links."""
    items = [f"<li>{m}</li>" for m in links_markup]
    if not items:
        return ""
    return section(title, '<ul class="see-also">' + "".join(items) + "</ul>\n")


def svg_line_chart(
    points: Sequence[tuple[float, float]],
    title: str,
    x_label: str = "",
    y_label: str = "",
    width: int = 480,
    height: int = 200,
) -> str:
    """A line chart as inline SVG (``currentColor``, so it follows the theme), with the first
    and last values on each axis. ``title`` is its accessible name."""
    if not points:
        return ""
    pad_l, pad_r, pad_t, pad_b = 48, 8, 8, 28
    xs = [p[0] for p in points]
    ys = [p[1] for p in points]
    x0, x1 = min(xs), max(xs)
    y0, y1 = min(0.0, min(ys)), max(ys)
    sx = (width - pad_l - pad_r) / ((x1 - x0) or 1)
    sy = (height - pad_t - pad_b) / ((y1 - y0) or 1)

    def fmt(v: float) -> str:
        return f"{v:.1f}".rstrip("0").rstrip(".")

    def ypos(y: float) -> float:
        return height - pad_b - (y - y0) * sy

    path = " ".join(
        ("M" if i == 0 else "L") + fmt(pad_l + (x - x0) * sx) + "," + fmt(ypos(y))
        for i, (x, y) in enumerate(points)
    )
    base = height - pad_b

    def num(v: float) -> str:
        return f"{v:,.0f}" if float(v).is_integer() else f"{v:,.2f}"

    return (
        f'<figure class="chart"><svg viewBox="0 0 {width} {height}" role="img"'
        f' aria-label="{esc(title)}" preserveAspectRatio="xMidYMid meet">'
        f"<title>{esc(title)}</title>"
        f'<g class="axis" stroke="currentColor" stroke-opacity="0.5" fill="none">'
        f'<path d="M{pad_l},{pad_t}V{base}H{width - pad_r}"/></g>'
        f'<path class="line" d="{path}" fill="none" stroke="currentColor" stroke-width="2"/>'
        f'<g class="labels" fill="currentColor" font-size="11">'
        f'<text x="{pad_l - 4}" y="{base}" text-anchor="end">{esc(num(y0))}</text>'
        f'<text x="{pad_l - 4}" y="{pad_t + 10}" text-anchor="end">{esc(num(y1))}</text>'
        f'<text x="{pad_l}" y="{height - 8}">{esc(num(x0))}</text>'
        f'<text x="{width - pad_r}" y="{height - 8}" text-anchor="end">{esc(num(x1))}</text>'
        f"</g></svg>"
        + (
            f"<figcaption>{esc(title)}"
            + (f" ({esc(x_label)} against {esc(y_label)})" if x_label and y_label else "")
            + "</figcaption>"
        )
        + "</figure>\n"
    )
