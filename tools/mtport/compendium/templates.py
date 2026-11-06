"""Shared page templates of the compendium site (ADR 0122).

Every function returns finished markup (it may hold link markers from
:class:`~mtport.compendium.siteapi.Context`). Arguments named ``*_markup`` must already be
safe HTML; plain ``str`` arguments are escaped here.

- :func:`infobox`: the box of key facts beside an entity page's text;
- :func:`table`: a sortable, filterable table (``htmlpage.table``, re-exported with its
  :class:`Column` and :class:`Cell` helpers; :func:`num_cell` writes thousands separators);
- :func:`drop_table`: drops with the real odds and the table's own value beside them;
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
esc = hp.esc

# Tables with more rows than this show the filter box (smaller ones are read at a glance).
FILTER_ROWS = 10


def number(value: int | float) -> str:
    """A number as the style guide writes it: thousands separators, no trailing zeros."""
    if isinstance(value, bool):
        return "yes" if value else "no"
    if isinstance(value, int) or float(value).is_integer():
        return f"{int(value):,}"
    return f"{value:,.2f}".rstrip("0").rstrip(".")


def num_cell(value: int | float, label: str = "") -> Cell:
    """A numeric cell with thousands separators, optionally followed by a (escaped) unit;
    sorts by ``value``."""
    shown = esc(number(value)) + (f' <span class="unit">{esc(label)}</span>' if label else "")
    return Cell(shown, value)


def markup_cell(markup: str, key: str | int | float | None = None) -> Cell:
    """A cell of safe markup (a link, a verbatim name), sorted by ``key`` or its text."""
    return Cell(markup, key)


def table(
    columns: Sequence[Column], rows: Sequence[Sequence[Cell]], row_ids=None, filter=None
) -> str:
    """A sortable table; ``row_ids`` give each row an anchor. The filter box shows when
    ``filter`` is true, or by default when there are more than :data:`FILTER_ROWS` rows."""
    out = hp.table(columns, rows, row_ids)
    show = len(rows) > FILTER_ROWS if filter is None else filter
    if not show:
        out = out.replace('<p class="tools">', '<p class="tools" hidden>', 1)
    return out


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
    subtitle_markup: str = "",
    footer_markup: str = "",
) -> str:
    """The infobox: a heading (often :meth:`Context.name`) with an optional subtitle (the
    Korean name), an optional picture (:meth:`Context.sprite`) and caption, rows of
    ``(label, value markup)`` and an optional footer (an in-game description). Labels are
    our words, escaped; a row with an empty value is left out."""
    sub = f'<p class="ib-sub">{subtitle_markup}</p>' if subtitle_markup else ""
    body = [
        '<aside class="infobox" aria-label="Key facts">\n'
        f'<div class="ib-title"><p class="ib-head">{heading_markup}</p>{sub}</div>'
    ]
    if image_markup:
        cap = f"<figcaption>{caption_markup}</figcaption>" if caption_markup else ""
        body.append(f'<figure class="ib-image">{image_markup}{cap}</figure>')
    items = [f"<dt>{esc(label)}</dt><dd>{value}</dd>" for label, value in rows if value]
    if items:
        body.append("<dl>" + "".join(items) + "</dl>")
    if footer_markup:
        body.append(f'<div class="ib-foot">{footer_markup}</div>')
    body.append("</aside>\n")
    return "\n".join(body)


@dataclass(frozen=True)
class Drop:
    """One drop: the item (link markup), its chance as the table gives it (``per_1000`` in
    ``per``: 1000 for most items, 100000 for equipment), a note, and ``percent``: the real
    odds when they differ from the face value (the game's random numbers are 15-bit, so
    40 in 1000 is really 4.03%). The column of a ``drop_table`` is named by ``item_title``."""

    item_markup: str
    per_1000: int
    note: str = ""
    per: int = 1000
    percent: float | None = None
    note_markup: str = ""
    face: bool = True  # show the table's own value beside the odds (not for meso)

    @property
    def real_percent(self) -> float:
        return self.percent if self.percent is not None else self.per_1000 * 100 / self.per


def percent(per_1000: int | float, per: int = 1000) -> str:
    """A chance of ``per_1000`` in ``per`` as a percentage: ``25`` gives ``2.5%``."""
    return percent_text(per_1000 * 100 / per)


def percent_text(value: float) -> str:
    """A percentage for reading: two decimals at most from 1% up, two significant digits
    below, never rounded to zero: ``4.0283`` gives ``4.03%``, ``0.00305`` ``0.0031%``."""
    if value == 0:
        return "0%"
    if abs(value) >= 1:
        out = f"{value:.2f}"
    else:
        digits = 1
        while round(value, digits) == 0 or abs(value) < 10 ** (1 - digits):
            digits += 1
        out = f"{value:.{digits}f}"
    if "." in out:
        out = out.rstrip("0").rstrip(".")
    return out + "%"


def chance_markup(d: Drop) -> str:
    """The real odds, with the table's own value beside them: ``4.03% (40/1000)``."""
    if not d.face:
        return f'<span class="pct">{esc(percent_text(d.real_percent))}</span>'
    face = f"{d.per_1000}/{d.per}"
    nominal = percent(d.per_1000, d.per)
    tip = f"The table says {face} ({nominal}); the game's 15-bit random numbers make it"
    tip += f" {percent_text(d.real_percent)} per kill."
    return (
        f'<span class="pct" title="{esc(tip)}">{esc(percent_text(d.real_percent))}</span>'
        f' <span class="per">({esc(face)})</span>'
    )


def drop_table(drops: Iterable[Drop], item_title: str = "Item") -> str:
    """A table of drops: the item, the chance per kill (the real odds, the game's own value
    beside them) and a note."""
    drops = list(drops)
    notes = any(d.note or d.note_markup for d in drops)
    columns = [
        Column(item_title),
        Column("Chance", "Chance per kill; the table's own value in brackets", numeric=True),
    ] + ([Column("Note")] if notes else [])
    rows = []
    for d in drops:
        row = [markup_cell(d.item_markup), Cell(chance_markup(d), round(d.real_percent, 6))]
        if notes:
            row.append(markup_cell(d.note_markup or esc(d.note)))
        rows.append(row)
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
