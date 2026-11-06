"""The compendium site's page API (ADR 0122): what section plugins and page builders use.

A **section** (:class:`Section`) is one entry of the site's navigation. Its ``pages(ctx)``
yields :class:`Page` objects. Section plugins are modules in
``mtport.compendium.sections``, each exposing ``SECTION``; a plugin whose ``id`` matches a
default section (:mod:`mtport.compendium.defaults`) replaces it.

A page's ``body`` is finished markup except for **link markers**: :meth:`Context.ref`,
:meth:`Context.href` and :meth:`Context.url` return placeholders that the generator resolves
once every page is known, to a path relative to the page that holds them. So a builder never
needs to know where another section puts its pages, and every link on the site is relative
(the site is served from ``/maplethief-port/`` and opens from ``file://``).

Game text (names, descriptions, dialogue) is wrapped in an element with ``data-verbatim``
(:func:`verbatim`, :meth:`Context.name`); the prose lint skips those and checks the rest.

The knowledge base is read by :func:`load_kb_dir` from ``<kind>.json`` files
(``{"schema": 1, "kind": k, "entities": [...]}``). Everything in it comes from the game's
files: it is untrusted text, escaped on output and never executed.
"""

from __future__ import annotations

import json
import posixpath
import re
from collections.abc import Callable, Iterable, Mapping
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from mtport import htmlpage as hp

KB_SCHEMA = 1
IMAGES_SCHEMA = 1

KB_DIR = Path("build/compendium/kb")
PAGES_DIR = Path("docs/compendium/pages")
OUT_DIR = Path("build/compendium/site")
IMG_DIR = "img"  # inside the site; written by the images step (#216), never by the generator


class BuildError(Exception):
    """A problem that stops the build (unknown reference, bad input, colliding pages)."""


Entity = Mapping[str, Any]
Key = tuple[str, str]  # (kind, id)


# --- the knowledge base -------------------------------------------------------------------


class KB:
    """The knowledge base: entities by kind, in the order of their files."""

    def __init__(self, kinds: Mapping[str, Iterable[Entity]]):
        self._kinds: dict[str, list[Entity]] = {}
        self._index: dict[Key, Entity] = {}
        for kind in sorted(kinds):
            rows = list(kinds[kind])
            for e in rows:
                eid = e.get("id")
                if not isinstance(eid, str) or not eid or "\x00" in eid or "\x01" in eid:
                    raise BuildError(f"kb: {kind}: entity without a usable string id: {eid!r}")
                if (kind, eid) in self._index:
                    raise BuildError(f"kb: {kind}: duplicate id {eid!r}")
                self._index[(kind, eid)] = e
            self._kinds[kind] = rows

    def kinds(self) -> list[str]:
        return sorted(self._kinds)

    def entities(self, kind: str) -> list[Entity]:
        return list(self._kinds.get(kind, ()))

    def get(self, kind: str, eid: str) -> Entity | None:
        return self._index.get((kind, eid))

    def keys(self) -> list[Key]:
        return [(k, str(e["id"])) for k in self.kinds() for e in self._kinds[k]]

    def __len__(self) -> int:
        return len(self._index)


def load_kb_dir(path: Path) -> KB:
    """Read the knowledge base in ``path``: with its ``index.json`` through #213's
    :func:`mtport.compendium.kb.load_kb` (the kinds it lists), else every ``<kind>.json``
    (synthetic fixtures). The generator only uses the :class:`KB` interface."""
    path = Path(path)
    if not path.is_dir():
        raise BuildError(f"{path}: no knowledge base (build it first)")
    if (path / "index.json").is_file():
        from mtport.compendium import kb as kbmod

        try:
            loaded = kbmod.load_kb(path)
        except (OSError, ValueError, KeyError, kbmod.KBError) as exc:
            raise BuildError(f"{path}: {exc}") from exc
        return KB({kind: list(rows.values()) for kind, rows in loaded.items()})
    kinds: dict[str, list[Entity]] = {}
    for f in sorted(path.glob("*.json")):
        try:
            doc = json.loads(f.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            raise BuildError(f"{f}: {exc}") from exc
        if not isinstance(doc, dict) or "entities" not in doc:
            continue  # not a kind file
        if doc.get("schema") != KB_SCHEMA:
            raise BuildError(f"{f}: schema {doc.get('schema')!r}, expected {KB_SCHEMA}")
        kind = doc.get("kind") or f.stem
        if not isinstance(doc["entities"], list):
            raise BuildError(f"{f}: entities is not a list")
        kinds[str(kind)] = [e for e in doc["entities"] if isinstance(e, dict)]
    return KB(kinds)


def name_of(e: Entity, lang: str = "en") -> str:
    """The entity's name in ``lang`` (``"en"`` or ``"ko"``), falling back to the other, then
    to its id."""
    n = e.get("name")
    if isinstance(n, Mapping):
        for k in (lang, "en", "ko"):
            if isinstance(n.get(k), str) and n[k].strip():
                return n[k]
    elif isinstance(n, str) and n.strip():
        return n
    return str(e.get("id", ""))


def slug(value: str) -> str:
    """A file-name-safe form of an id: ``equip:3`` gives ``equip-3``."""
    s = re.sub(r"[^A-Za-z0-9._]+", "-", str(value)).strip("-")
    return s or "x"


# --- images (#216's manifest) ---------------------------------------------------------------


@dataclass(frozen=True)
class Image:
    """One image of an entity; ``path`` is relative to the site's ``img/``. ``action``: the
    sprite action of a still or animation (``"move"``, ``"die"``...); ``delays_ms``: an
    animation's frame delays."""

    path: str
    w: int
    h: int
    alt: str
    role: str
    action: str = ""
    delays_ms: tuple[int, ...] = ()


class Images:
    """The images step's manifest; empty when there is none."""

    def __init__(self, by_key: Mapping[str, Iterable[Image]] | None = None):
        self._by = {k: list(v) for k, v in (by_key or {}).items()}
        # A picture's double-size copy (role ``icon2x``, ``render2x``), by the picture's path.
        self._hi: dict[str, Image] = {}
        for rows in self._by.values():
            for i in rows:
                if i.role.endswith("2x"):
                    stem, dot, ext = i.path.rpartition(".")
                    if stem.endswith("@2x"):
                        self._hi[stem[: -len("@2x")] + dot + ext] = i

    @classmethod
    def from_file(cls, path: Path) -> Images:
        path = Path(path)
        if not path.is_file():
            return cls()
        try:
            doc = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            raise BuildError(f"{path}: {exc}") from exc
        if doc.get("schema") != IMAGES_SCHEMA:
            raise BuildError(f"{path}: schema {doc.get('schema')!r}, expected {IMAGES_SCHEMA}")
        out: dict[str, list[Image]] = {}
        for key, rows in sorted((doc.get("images") or {}).items()):
            for r in rows:
                p = str(r.get("path", ""))
                if not p or p.startswith("/") or ".." in p.split("/"):
                    raise BuildError(f"{path}: {key}: bad image path {p!r}")
                out.setdefault(key, []).append(
                    Image(
                        p,
                        int(r.get("w", 0)),
                        int(r.get("h", 0)),
                        str(r.get("alt") or ""),
                        str(r.get("role") or "still"),
                        str(r.get("action") or ""),
                        tuple(int(d) for d in r.get("delays_ms") or ()),
                    )
                )
        return cls(out)

    def main(self, kind: str, eid: str) -> Image | None:
        """The entity's main picture: the ``move`` still of a sprite (else its first still),
        else a map render, else an icon."""
        stills = self.get(kind, eid, ("still",))
        for i in stills:
            if i.action == "move":
                return i
        if stills:
            return stills[0]
        rest = self.get(kind, eid, ("render", "icon"))
        return rest[0] if rest else None

    def hires(self, image: Image) -> Image | None:
        """The double-size copy of ``image`` (``x@2x.png`` beside ``x.png``), if any."""
        return self._hi.get(image.path)

    def anim(self, kind: str, eid: str, still: Image) -> Image | None:
        """The animation of the same action as ``still``, when its frames have the same
        size (so one ``<img>`` box fits both)."""
        if not still.action:
            return None
        for i in self.get(kind, eid, ("anim",)):
            if i.action == still.action and (i.w, i.h) == (still.w, still.h):
                return i
        return None

    def get(self, kind: str, eid: str, roles: Iterable[str] | None = None) -> list[Image]:
        rows = self._by.get(f"{kind}:{eid}", [])
        if roles is None:
            return list(rows)
        wanted = list(roles)
        return sorted((i for i in rows if i.role in wanted), key=lambda i: wanted.index(i.role))


# --- pages and sections -----------------------------------------------------------------------


@dataclass
class Page:
    """One HTML page of the site.

    ``path``: site-relative POSIX path ending in ``.html``. ``title``: plain text for the
    ``<title>``, breadcrumbs and search; ``heading``: the ``<h1>`` markup (default: the
    escaped title; :meth:`Context.name` for a game name). ``title_verbatim`` marks the title
    as game text. ``body``: markup with link markers. ``entities``: the knowledge-base
    entities this page documents, ``(kind, id) -> anchor`` (``""``: the whole page; an
    anchor names an element ``id`` on the page, e.g. a table row). ``crumbs``: extra
    breadcrumbs between the section and the page, as ``(label, site path)``. ``search``:
    whether the page itself gets a search entry (entities always get theirs).
    """

    path: str
    title: str
    body: str
    heading: str = ""
    title_verbatim: bool = False
    section: str = ""
    entities: dict[Key, str] = field(default_factory=dict)
    crumbs: list[tuple[str, str]] = field(default_factory=list)
    description: str = ""
    search: bool = True

    def documents(self, kind: str, eid: str, anchor: str = "") -> Page:
        self.entities[(kind, str(eid))] = anchor
        return self


PagesFn = Callable[["Context"], Iterable[Page]]


@dataclass(frozen=True)
class Section:
    """One section of the site's navigation.

    ``id``: its directory and the key a plugin uses to replace a default section. ``order``:
    position in the navigation (defaults use 10, 20, ... 90). ``kinds``: the knowledge-base
    kinds whose entities it documents (for the defaults and for unclaimed kinds). ``pages``:
    yields the section's pages; ``None`` uses the default builder for ``kinds``. ``blurb``:
    one line for the home page (our own words). ``image``: ``"kind:id"`` of the entity whose
    picture the home page's card shows (optional). ``names``: ``(ctx, kind, id)`` to the
    markup every link to one of its ``kinds`` shows by default (game text marked verbatim),
    or ``None`` for the entity's own name; for entities the knowledge base names by number.
    ``card_link``: ``(label, site path)`` of a second link on the home page's card.
    """

    id: str
    title: str
    order: int
    kinds: tuple[str, ...] = ()
    pages: PagesFn | None = None
    blurb: str = ""
    image: str = ""
    names: Callable[[Context, str, str], str | None] | None = None
    # (label, site path): a second link under the home page's card (our own words), e.g. a
    # section's showpiece page.
    card_link: tuple[str, str] | None = None

    @property
    def index_path(self) -> str:
        return f"{self.id}/index.html"


# --- markup helpers -----------------------------------------------------------------------------


def verbatim(value: str, lang: str = "en", tag: str = "span", cls: str = "") -> str:
    """Game text, escaped, in an element with ``data-verbatim`` (exempt from the prose lint)."""
    c = f' class="{hp.esc(cls)}"' if cls else ""
    return f'<{tag} data-verbatim lang="{hp.esc(lang)}"{c}>{hp.esc(value)}</{tag}>'


_ENTITY_MARK = "\x00E{kind}\x01{eid}\x00"
_PATH_MARK = "\x00P{path}\x00"
MARK_RE = re.compile("\x00([EP])([^\x00]*)\x00")


def path_marker(path: str) -> str:
    """A link marker to a site path (``monsters/index.html``, ``x.html#frag``)."""
    if path.startswith(("/", "\\")) or "\x00" in path or ".." in path.split("#")[0].split("/"):
        raise BuildError(f"bad site path {path!r}: use a path relative to the site root")
    return _PATH_MARK.format(path=path)


class Context:
    """What a page builder gets: the knowledge base, the images, the prose, and helpers that
    make links. One context serves the whole build; :attr:`section` is the section whose
    pages are being built."""

    def __init__(self, kb: KB, images: Images, pages_dir: Path | None):
        self.kb = kb
        self.images = images
        # kind -> a section's ``names`` (Section.names), set when the sections are known
        self.namers: dict[str, Callable[[Context, str, str], str | None]] = {}
        self.pages_dir = Path(pages_dir) if pages_dir is not None else None
        self.section: Section | None = None
        self.consumed: set[str] = set()
        # Files of the site that are not pages (``add_file``): site path -> text with markers.
        self.files: dict[str, str] = {}
        self._prose_cache: dict[str, tuple[str, str] | None] = {}

    # links
    def href(self, kind: str, eid: str) -> str:
        """A marker for the URL of the page (or anchor) that documents ``kind:eid``."""
        if self.kb.get(kind, str(eid)) is None:
            raise BuildError(f"unknown entity {kind}:{eid}")
        return _ENTITY_MARK.format(kind=kind, eid=eid)

    def url(self, path: str) -> str:
        """A marker for a site path; resolved relative to the page that holds it."""
        return path_marker(path)

    def ref(self, kind: str, eid: str, label: str | None = None) -> str:
        """A link to ``kind:eid``: by default its English name as game text, else ``label``
        (our own words, escaped)."""
        href = self.href(kind, eid)
        text = None
        if label is None and kind in self.namers:
            text = self.namers[kind](self, kind, str(eid))
        if text is None:
            text = self.name(kind, eid) if label is None else hp.esc(label)
        return f'<a href="{href}">{text}</a>'

    def name(self, kind: str, eid: str, lang: str = "en") -> str:
        """The entity's name as game text (``data-verbatim``)."""
        e = self.kb.get(kind, str(eid))
        if e is None:
            raise BuildError(f"unknown entity {kind}:{eid}")
        return verbatim(name_of(e, lang), lang)

    def add_file(self, path: str, text: str) -> None:
        """A file of the site that is not a page (a JSON file a page's script reads). Its
        link markers are resolved relative to ``path`` like a page's, but not HTML-escaped, so
        they suit JSON strings. ``path`` must not end in ``.html``."""
        path_marker(path)  # the same path rules as a link
        if path.endswith(".html") or path in self.files:
            raise BuildError(f"extra file {path!r}: a page's path, or written twice")
        self.files[path] = text

    # images
    def entity_images(self, kind: str, eid: str, roles: Iterable[str] | None = None):
        return self.images.get(kind, str(eid), roles)

    def img(self, image: Image, alt: str = "", cls: str = "", scale: int = 1) -> str:
        """An ``<img>`` of ``image``; ``alt`` defaults to the manifest's alt text.

        ``scale`` (2, 3...) shows pixel art that many times its size, crisply (class
        ``pixel``), from the double-size copy when the images step made one."""
        a = alt or image.alt
        classes = [cls] if cls else []
        src = image
        if scale > 1:
            classes.append("pixel")
            src = self.images.hires(image) or image
        c = f' class="{hp.esc(" ".join(classes))}"' if classes else ""
        w, h = image.w * max(scale, 1), image.h * max(scale, 1)
        size = f' width="{w}" height="{h}"' if w and h else ""
        return (
            f'<img src="{self.url(IMG_DIR + "/" + src.path)}" alt="{hp.esc(a)}"{size}{c}'
            ' loading="lazy">'
        )

    def sprite(
        self,
        kind: str,
        eid: str,
        box: int = 160,
        animate: bool = True,
        image: Image | None = None,
        alt: str = "",
    ) -> str:
        """The entity's main picture (:meth:`Images.main`, or ``image``) scaled up crisply by
        the largest whole factor (at most 4) that fits a ``box`` px square; a large picture
        (a map) is shown as it is and shrinks with the page. A sprite with an animation of the
        same action plays it unless the reader asks for reduced motion. Empty when there is
        no picture."""
        img = image or self.images.main(kind, str(eid))
        if img is None:
            return ""
        e = self.kb.get(kind, str(eid))
        a = alt or img.alt or (name_of(e) if e is not None else "")
        big = max(img.w, img.h)
        scale = max(1, min(4, box // big)) if big and big <= box else 1
        tag = self.img(img, alt=a, scale=scale)
        anim = self.images.anim(kind, str(eid), img) if animate else None
        if anim is None:
            return tag
        src = (self.images.hires(anim) or anim) if scale > 1 else anim
        return (
            '<picture><source media="(prefers-reduced-motion: no-preference)"'
            f' srcset="{self.url(IMG_DIR + "/" + src.path)}">{tag}</picture>'
        )

    # prose
    def prose(self, rel: str) -> str | None:
        """The rendered body of ``docs/compendium/pages/<rel>`` (without its ``#`` title), or
        ``None`` when there is no such file. Marks the file as used, so it does not also
        become a page of its own."""
        got = self._prose(rel)
        return None if got is None else got[1]

    def prose_title(self, rel: str) -> str | None:
        got = self._prose(rel)
        return None if got is None else got[0]

    def _prose(self, rel: str) -> tuple[str, str] | None:
        from mtport.compendium import prose  # late: prose uses this module

        rel = posixpath.normpath(rel)
        if rel not in self._prose_cache:
            path = self.pages_dir / rel if self.pages_dir is not None else None
            if path is None or not path.is_file():
                self._prose_cache[rel] = None
            else:
                self._prose_cache[rel] = prose.render(
                    path.read_text(encoding="utf-8"), self, source=rel
                )
        if self._prose_cache[rel] is not None:
            self.consumed.add(rel)
        return self._prose_cache[rel]

    def prose_files(self) -> list[str]:
        if self.pages_dir is None or not self.pages_dir.is_dir():
            return []
        return sorted(
            p.relative_to(self.pages_dir).as_posix() for p in self.pages_dir.rglob("*.md")
        )

    # defaults, for sections that extend rather than replace them
    def default_entity_page(self, kind: str, e: Entity, path: str | None = None) -> Page:
        from mtport.compendium import defaults

        return defaults.entity_page(self, kind, e, path)

    def default_section_pages(self, section: Section) -> list[Page]:
        from mtport.compendium import defaults

        return list(defaults.section_pages(self, section))
