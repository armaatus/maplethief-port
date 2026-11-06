"""The compendium site generator (issue #215, ADR 0122).

Turns the knowledge base (``build/compendium/kb/``), the images step's manifest
(``build/compendium/site/img/manifest.json``) and the hand-written prose
(``docs/compendium/pages/``) into a static site in ``build/compendium/site/``: plain HTML,
one stylesheet, one script and a search index, all linked relatively, so the site works
from ``file://`` and from the ``/maplethief-port/`` sub-path of GitHub Pages.

The sections come from :mod:`mtport.compendium.defaults` and the plugins in
:mod:`mtport.compendium.sections` (the API is :mod:`mtport.compendium.siteapi`). The
generator adds the shell (header with search, section navigation, breadcrumbs, footer), the
home page, ``search.html`` and the pages of prose files no section used.

Command line (``PYTHONPATH=tools python3 -m mtport.compendium.site ...``)::

    build [--kb DIR] [--pages DIR] [--out DIR]   write the site (img/ is left alone)
    check [--kb DIR] [--pages DIR] [--out DIR]   build in memory and check: every internal
                                                 link and anchor resolves, every search
                                                 entry has its page, every entity has a
                                                 page or an anchor, the prose lint (when
                                                 present) and the files equal the fresh build

Both print one line per problem and exit non-zero on any. The output is deterministic.
"""

from __future__ import annotations

import argparse
import json
import posixpath
import shutil
import sys
import unicodedata
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass, field
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import unquote, urlsplit

from mtport import htmlpage as hp
from mtport.compendium import assets, defaults, sections, templates
from mtport.compendium.siteapi import (
    IMG_DIR,
    KB_DIR,
    MARK_RE,
    OUT_DIR,
    PAGES_DIR,
    BuildError,
    Context,
    Images,
    Key,
    Page,
    Section,
    load_kb_dir,
    name_of,
)

SITE_NAME = "MapleStory Thief Edition compendium"
# Placeholder until ADR 0121 (#211) settles the wording; the one place it is written.
DISCLAIMER = (
    "Unofficial fan project. Not affiliated with or endorsed by Nexon. "
    "MapleStory and its content belong to Nexon."
)
LEGAL_URL = "https://github.com/armaatus/maplethief-port/blob/main/docs/legal.md"
REPO_URL = "https://github.com/armaatus/maplethief-port"

MARKER = ".mtport-compendium-site"
SEARCH_INDEX = "search-index.js"
SEARCH_PREFIX = "window.MTSEARCH = "
STATIC = ("site.css", "site.js", SEARCH_INDEX)


@dataclass
class Site:
    """A built site: its files, and what ``check`` needs to know about them."""

    files: dict[str, bytes]
    pages: dict[str, Page]
    registry: dict[Key, tuple[str, str]]  # (kind, id) -> (page path, anchor)
    index: list[dict]
    kb_keys: list[Key]
    sections: list[Section] = field(default_factory=list)


# --- assembly -----------------------------------------------------------------------------------


def all_sections(plugins: Iterable[Section] | None = None) -> list[Section]:
    """The default sections with the plugins' replacing those of the same id, then the
    plugins' new sections, by ``order``."""
    by_id = {s.id: s for s in defaults.DEFAULT_SECTIONS}
    for s in sections.discover() if plugins is None else plugins:
        by_id[s.id] = s
    return sorted(by_id.values(), key=lambda s: (s.order, s.id))


def _other_section(kb_kinds: Sequence[str], secs: Sequence[Section]) -> Section | None:
    claimed = {k for s in secs for k in s.kinds}
    rest = tuple(k for k in kb_kinds if k not in claimed)
    if not rest:
        return None
    return Section("other", "Other", 1000, rest, blurb="Everything else in the knowledge base.")


def _home(ctx: Context, secs: Sequence[Section]) -> Page:
    parts = []
    intro = ctx.prose("index.md")
    if intro:
        parts.append(intro)
    cards = "".join(
        f'<li><a href="{ctx.url(s.index_path)}">{hp.esc(s.title)}</a>'
        + (f"<p>{hp.esc(s.blurb)}</p>" if s.blurb else "")
        + "</li>"
        for s in secs
    )
    parts.append(f'<ul class="cards">{cards}</ul>')
    return Page("index.html", ctx.prose_title("index.md") or SITE_NAME, "\n".join(parts))


def _search_page() -> Page:
    body = (
        '<div id="search-page" class="search-page">\n'
        '<p><label for="sq">Search the compendium</label></p>\n'
        '<p><input id="sq" type="search" autocomplete="off"></p>\n'
        '<p id="search-count" class="muted" aria-live="polite"></p>\n'
        '<ul id="search-results" class="results"></ul>\n'
        "<noscript><p>Search needs JavaScript. Every section's index lists its pages."
        "</p></noscript>\n</div>\n"
    )
    return Page("search.html", "Search", body, search=False)


def _collect(ctx: Context, secs: Sequence[Section]) -> dict[str, Page]:
    pages: dict[str, Page] = {}

    def add(page: Page, section: str) -> None:
        if not page.path.endswith(".html") or page.path.startswith("/") or "\\" in page.path:
            raise BuildError(f"page path {page.path!r}: a relative path ending in .html")
        norm = posixpath.normpath(page.path)
        if norm != page.path or norm.startswith(".."):
            raise BuildError(f"page path {page.path!r} is not normalised")
        if page.path in pages:
            raise BuildError(
                f"two pages at {page.path} (sections {pages[page.path].section!r} and {section!r})"
            )
        if not page.section:
            page.section = section
        pages[page.path] = page

    for s in secs:
        ctx.section = s
        built = s.pages(ctx) if s.pages is not None else defaults.section_pages(ctx, s)
        for p in built:
            add(p, s.id)
        if s.index_path not in pages:
            raise BuildError(f"section {s.id}: no index page {s.index_path}")
    ctx.section = None
    if "index.html" not in pages:
        add(_home(ctx, secs), "")
    if "search.html" not in pages:
        add(_search_page(), "")
    ids = {s.id for s in secs}
    articles: dict[str, list[str]] = {}
    for rel in ctx.prose_files():
        if rel in ctx.consumed:
            continue
        path = rel[: -len(".md")] + ".html"
        if path in pages:
            raise BuildError(
                f"{PAGES_DIR / rel}: {path} is built by section {pages[path].section!r},"
                " which does not use this file (ctx.prose)"
            )
        body = ctx.prose(rel) or ""
        title = ctx.prose_title(rel)
        if not title:
            raise BuildError(
                f"{ctx.pages_dir / rel}: no page uses this file, and a page of its own needs"
                " a # title"
            )
        top = rel.split("/")[0]
        add(Page(path, title, body), top if top in ids else "")
        if top in ids:
            articles.setdefault(top, []).append(path)
    # A section's index lists the prose pages in its directory that no builder used.
    for sid, paths in sorted(articles.items()):
        links = [f'<a href="{ctx.url(p)}">{hp.esc(pages[p].title)}</a>' for p in sorted(paths)]
        pages[f"{sid}/index.html"].body += templates.section(
            "Articles", "<ul>" + "".join(f"<li>{x}</li>" for x in links) + "</ul>\n", "articles"
        )
    return pages


def _registry(pages: Mapping[str, Page]) -> dict[Key, tuple[str, str]]:
    reg: dict[Key, tuple[str, str]] = {}
    for path in sorted(pages):
        for key, anchor in sorted(pages[path].entities.items()):
            old = reg.get(key)
            if old is None or (old[1] and not anchor):
                reg[key] = (path, anchor)
            elif not old[1] and not anchor:
                raise BuildError(f"{key[0]}:{key[1]} has two pages: {old[0]} and {path}")
    return reg


def rel_url(target: str, from_page: str) -> str:
    """``target`` (a site path, maybe with ``#frag``) relative to the page ``from_page``."""
    path, _, frag = target.partition("#")
    base = posixpath.dirname(from_page) or "."
    if path == from_page and frag:
        out = ""
    else:
        out = posixpath.relpath(path or from_page, base)
    return out + (f"#{frag}" if frag else "")


def _resolve(markup: str, page: str, reg: Mapping[Key, tuple[str, str]], errors: list[str]) -> str:
    def sub(m) -> str:
        if m.group(1) == "P":
            return hp.esc(rel_url(m.group(2), page))
        kind, _, eid = m.group(2).partition("\x01")
        hit = reg.get((kind, eid))
        if hit is None:
            errors.append(f"{page}: link to {kind}:{eid}, which no page documents")
            return "#"
        target = hit[0] + (f"#{hit[1]}" if hit[1] else "")
        return hp.esc(rel_url(target, page))

    return MARK_RE.sub(sub, markup)


def _shell(page: Page, secs: Sequence[Section], pages: Mapping[str, Page]) -> str:
    root = rel_url("index.html", page.path)[: -len("index.html")]
    m = hp.esc  # markers are resolved after the shell is built

    def u(path: str) -> str:
        return m(root + path)

    nav = "".join(
        f'<li><a href="{u(s.index_path)}"'
        + (' aria-current="page"' if page.path == s.index_path else "")
        + f">{m(s.title)}</a></li>"
        for s in secs
    )
    crumbs = []
    if page.path != "index.html":
        crumbs.append(("Home", "index.html"))
        sec = next((s for s in secs if s.id == page.section), None)
        if sec is not None and page.path != sec.index_path:
            crumbs.append((sec.title, sec.index_path))
        crumbs.extend(page.crumbs)
    crumb_html = ""
    if crumbs:
        items = "".join(f'<li><a href="{u(p)}">{m(label)}</a></li>' for label, p in crumbs)
        cur = (
            f'<li aria-current="page"><span data-verbatim>{m(page.title)}</span></li>'
            if page.title_verbatim
            else f'<li aria-current="page">{m(page.title)}</li>'
        )
        crumb_html = (
            f'<nav class="crumbs page-top" aria-label="Breadcrumb"><ol>{items}{cur}</ol></nav>\n'
        )
    title_attr = " data-verbatim" if page.title_verbatim else ""
    full_title = page.title if page.path == "index.html" else f"{page.title} | {SITE_NAME}"
    heading = page.heading or m(page.title)
    desc = page.description or f"{page.title}, from the {SITE_NAME}."
    desc_attr = " data-verbatim" if page.title_verbatim else ""
    return (
        "<!doctype html>\n"
        f'<html lang="en" data-root="{m(root)}">\n<head>\n<meta charset="utf-8">\n'
        '<meta name="viewport" content="width=device-width, initial-scale=1">\n'
        f"<title{title_attr}>{m(full_title)}</title>\n"
        f'<meta name="description" content="{m(desc)}"{desc_attr}>\n'
        f'<link rel="stylesheet" href="{u("site.css")}">\n</head>\n<body>\n'
        '<a class="skip" href="#main">Skip to the content</a>\n'
        f'<header class="site-head">\n<a class="brand" href="{u("index.html")}">'
        f"{m(SITE_NAME)}</a>\n"
        f'<form class="search" role="search" action="{u("search.html")}">'
        '<label class="sr" for="q">Search</label>'
        '<input id="q" name="q" type="search" placeholder="Search monsters, items, maps"'
        ' autocomplete="off" role="combobox" aria-autocomplete="list" aria-expanded="false"'
        ' aria-controls="q-results">'
        '<ul id="q-results" role="listbox" aria-label="Search results" hidden></ul>'
        "</form>\n</header>\n"
        f'<nav class="site-nav" aria-label="Sections"><ul>{nav}</ul></nav>\n'
        + crumb_html
        + f'<main id="main">\n<h1>{heading}</h1>\n{page.body}</main>\n'
        f'<footer class="site-foot"><p>{m(DISCLAIMER)} '
        f'<a href="{m(LEGAL_URL)}">Legal notes</a>. '
        f'<a href="{m(REPO_URL)}">Source code</a>.</p></footer>\n'
        f'<script src="{u("site.js")}"></script>\n</body>\n</html>\n'
    )


def _norm(s: str) -> str:
    return unicodedata.normalize("NFKC", s).lower().strip()


def _search_index(
    ctx: Context, pages: Mapping[str, Page], reg: Mapping[Key, tuple[str, str]]
) -> list[dict]:
    out = []
    for kind, eid in sorted(reg):
        e = ctx.kb.get(kind, eid)
        if e is None:
            continue
        path, anchor = reg[(kind, eid)]
        aliases: list[str] = []
        for a in e.get("aliases") or []:
            if isinstance(a, str):
                aliases.append(a)
            elif isinstance(a, Mapping):
                aliases += [str(v) for k, v in sorted(a.items()) if k in ("en", "ko") and v]
        entry = {
            "t": name_of(e, "en"),
            "ko": name_of(e, "ko"),
            "k": defaults.kind_title(kind).rstrip("s") if kind != "dialogue" else "Dialogue",
            "u": path + (f"#{anchor}" if anchor else ""),
        }
        if aliases:
            entry["a"] = aliases
        out.append(entry)
    documented = {p for p, _ in reg.values()}
    for path in sorted(pages):
        p = pages[path]
        if p.search and path not in documented:
            out.append({"t": p.title, "ko": "", "k": "Page", "u": path})
    return out


def search(index: Sequence[Mapping], query: str, limit: int | None = None) -> list[Mapping]:
    """The site script's search, in Python (the tests use it): exact, then prefix, then
    substring matches of the English name, the Korean name or an alias, case-insensitively
    after NFKC normalisation; ties keep the index order."""
    q = _norm(query)
    if not q:
        return []
    hits = []
    for n, e in enumerate(index):
        best = 9
        for h in [e.get("t", ""), e.get("ko", ""), *e.get("a", [])]:
            h = _norm(h)
            if not h:
                continue
            s = 0 if h == q else 1 if h.startswith(q) else 2 if q in h else 9
            best = min(best, s)
        if best < 9:
            hits.append((best, n, e))
    hits.sort(key=lambda x: (x[0], x[1]))
    return [e for _, _, e in hits[:limit]]


def build_site(
    kb_dir: Path = KB_DIR,
    pages_dir: Path | None = PAGES_DIR,
    images_manifest: Path | None = None,
    plugins: Iterable[Section] | None = None,
) -> Site:
    """Build the whole site in memory. Raises :class:`BuildError` on any build error."""
    kb = load_kb_dir(kb_dir)
    images = Images.from_file(images_manifest) if images_manifest else Images()
    ctx = Context(kb, images, pages_dir)
    secs = all_sections(plugins)
    other = _other_section(kb.kinds(), secs)
    if other is not None:
        secs.append(other)
    pages = _collect(ctx, secs)
    reg = _registry(pages)
    for (kind, eid), (path, _) in reg.items():
        if kb.get(kind, eid) is None:
            raise BuildError(f"{path}: documents {kind}:{eid}, which the knowledge base lacks")
    errors: list[str] = []
    files: dict[str, bytes] = {}
    for path in sorted(pages):
        html = _resolve(_shell(pages[path], secs, pages), path, reg, errors)
        files[path] = html.encode("utf-8")
    if errors:
        raise BuildError("\n".join(errors))
    index = _search_index(ctx, pages, reg)
    files["site.css"] = assets.CSS.encode("utf-8")
    files["site.js"] = assets.JS.encode("utf-8")
    payload = json.dumps(index, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
    files[SEARCH_INDEX] = (SEARCH_PREFIX + payload + ";\n").encode("utf-8")
    return Site(files, pages, reg, index, kb.keys(), secs)


# --- output -------------------------------------------------------------------------------------


def _owned(out: Path) -> bool:
    if (out / MARKER).is_file():
        return True
    return all(p.name == IMG_DIR for p in out.iterdir())


def write_site(out: Path, files: Mapping[str, bytes]) -> None:
    """Replace the generator's files in ``out`` with ``files``; ``img/`` (the images step's)
    stays. Refuses a non-empty directory that is not a compendium site."""
    out = Path(out)
    if out.exists():
        if not _owned(out):
            raise FileExistsError(
                f"{out}: not empty and not a compendium site (no {MARKER}); refusing to replace it"
            )
        for p in sorted(out.iterdir()):
            if p.name == IMG_DIR:
                continue
            if p.is_dir() and not p.is_symlink():
                shutil.rmtree(p)
            else:
                p.unlink()
    out.mkdir(parents=True, exist_ok=True)
    for rel in sorted(files):
        path = out / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(files[rel])
    (out / MARKER).write_text("compendium site\n", encoding="utf-8")


def diff_site(out: Path, files: Mapping[str, bytes]) -> list[str]:
    """How ``out`` differs from a fresh build (``img/`` is not the generator's)."""
    out = Path(out)
    if not out.is_dir():
        return [f"{out}: missing (run the build command)"]
    problems = []
    for rel in sorted(files):
        path = out / rel
        if not path.is_file():
            problems.append(f"{rel}: missing")
        elif path.read_bytes() != files[rel]:
            problems.append(f"{rel}: stale (differs from a fresh build)")
    on_disk = {
        p.relative_to(out).as_posix()
        for p in out.rglob("*")
        if p.is_file() and p.relative_to(out).parts[0] != IMG_DIR
    }
    for rel in sorted(on_disk - set(files) - {MARKER}):
        problems.append(f"{rel}: not part of a fresh build")
    return problems


# --- checks -------------------------------------------------------------------------------------


class _Links(HTMLParser):
    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.ids: set[str] = set()
        self.links: list[str] = []

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if a.get("id"):
            self.ids.add(a["id"])
        if tag == "a" and a.get("name"):
            self.ids.add(a["name"])
        for attr in ("href", "src", "action"):
            if a.get(attr) is not None:
                self.links.append(a[attr])


def check_links(files: Mapping[str, bytes], img_root: Path | None = None) -> list[str]:
    """Every relative ``href``/``src``/``action`` of every page resolves to a file of the site
    (``img/`` files on disk under ``img_root``) and, with a ``#fragment``, to an element id
    of that page. Absolute paths (``/x``) are errors: the site lives under a sub-path."""
    parsed: dict[str, _Links] = {}
    for rel in sorted(files):
        if rel.endswith(".html"):
            p = _Links()
            p.feed(files[rel].decode("utf-8"))
            parsed[rel] = p
    problems = []
    for rel, p in parsed.items():
        base = posixpath.dirname(rel)
        for link in p.links:
            parts = urlsplit(link)
            if parts.scheme or parts.netloc:
                if parts.scheme not in ("http", "https", "mailto"):
                    problems.append(f"{rel}: link {link!r}: unexpected scheme")
                continue
            if link.startswith("/"):
                problems.append(f"{rel}: link {link!r} is absolute; links must be relative")
                continue
            if not parts.path:
                target = rel
            else:
                target = posixpath.normpath(posixpath.join(base, unquote(parts.path)))
                if target.startswith("../") or target == "..":
                    problems.append(f"{rel}: link {link!r} leaves the site")
                    continue
            exists = target in files or (
                img_root is not None
                and target.split("/")[0] == IMG_DIR
                and (img_root.parent / target).is_file()
            )
            if not exists:
                problems.append(f"{rel}: broken link {link!r} ({target} does not exist)")
                continue
            if parts.fragment and target.endswith(".html"):
                tp = parsed.get(target)
                if tp is None or unquote(parts.fragment) not in tp.ids:
                    problems.append(f"{rel}: broken link {link!r} (no #{parts.fragment})")
    return problems


def check_index(site: Site) -> list[str]:
    problems = []
    for e in site.index:
        path, _, frag = e["u"].partition("#")
        if path not in site.pages:
            problems.append(f"search: {e['t']!r} points at {e['u']}, which is not a page")
            continue
        if frag:
            p = _Links()
            p.feed(site.files[path].decode("utf-8"))
            if frag not in p.ids:
                problems.append(f"search: {e['t']!r} points at {e['u']}: no such anchor")
    return problems


def completeness(site: Site) -> tuple[list[str], dict[str, tuple[int, int]]]:
    """Entities without a page or an anchor: one line each, and per kind (documented, all)."""
    counts: dict[str, list[int]] = {}
    missing = []
    for kind, eid in site.kb_keys:
        c = counts.setdefault(kind, [0, 0])
        c[1] += 1
        if (kind, eid) in site.registry:
            c[0] += 1
        else:
            missing.append(f"completeness: {kind}:{eid} has no page or anchor")
    return missing, {k: (v[0], v[1]) for k, v in sorted(counts.items())}


def prose_lint(files: Mapping[str, bytes]) -> list[str] | None:
    """Findings of #212's prose lint on every page, or ``None`` when it is not installed."""
    try:
        from mtport import prose_lint as pl  # type: ignore[attr-defined]
    except ImportError:
        return None
    lint = getattr(pl, "lint_text", None)
    if lint is None:
        return None
    out = []
    for rel in sorted(files):
        if rel.endswith(".html"):
            for finding in lint(files[rel].decode("utf-8"), kind="html") or ():
                out.append(f"{rel}: {finding}")
    return out


def check_site(out: Path, site: Site) -> list[str]:
    img_root = Path(out) / IMG_DIR
    problems = check_links(site.files, img_root)
    problems += check_index(site)
    missing, _ = completeness(site)
    problems += missing
    lint = prose_lint(site.files)
    if lint:
        problems += lint
    problems += diff_site(out, site.files)
    return problems


# --- command line -------------------------------------------------------------------------------


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.compendium.site", description=__doc__.split("\n\n")[0]
    )
    sub = parser.add_subparsers(dest="cmd", required=True)
    for cmd, help_ in (("build", "write the site"), ("check", "check the site on disk")):
        p = sub.add_parser(cmd, help=help_)
        p.add_argument("--kb", type=Path, default=KB_DIR, help=f"knowledge base (default {KB_DIR})")
        p.add_argument("--pages", type=Path, default=PAGES_DIR, help=f"prose (default {PAGES_DIR})")
        p.add_argument("--out", type=Path, default=OUT_DIR, help=f"site (default {OUT_DIR})")
    args = parser.parse_args(argv)
    manifest = args.out / IMG_DIR / "manifest.json"
    try:
        site = build_site(args.kb, args.pages, manifest)
    except BuildError as exc:
        for line in str(exc).splitlines():
            print(f"error: {line}", file=sys.stderr)
        return 1
    missing, counts = completeness(site)
    summary = ", ".join(f"{k} {a}/{b}" for k, (a, b) in counts.items())
    if args.cmd == "build":
        try:
            write_site(args.out, site.files)
        except FileExistsError as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 1
        print(f"site: {len(site.pages)} pages, {len(site.index)} search entries -> {args.out}")
        print(f"site: documented {summary}")
        for line in missing:
            print(f"warning: {line}", file=sys.stderr)
        return 0
    problems = check_site(args.out, site)
    for line in problems:
        print(line)
    lint = "prose lint not installed" if prose_lint({}) is None else "prose lint run"
    print(f"check: {len(site.pages)} pages, documented {summary}; {lint}; {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
