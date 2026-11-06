# 0122. Compendium site generator

- **Status:** Accepted
- **Date:** 2026-11-06
- **Issue:** #215 (epic #210)

## Context

The compendium (epic #210, ADR 0121) is a static site built from the knowledge base (`build/compendium/kb/`, #213), the images step's files (`build/compendium/site/img/`, #216) and hand-written prose in the repository (`docs/compendium/pages/`). It is built on a machine that has the user's own IPA and is never committed. It is served from a sub-path (`https://armaatus.github.io/maplethief-port/`), must open from `file://` for a local preview, and must work on a phone at 320 px and in light and dark mode.

Nine sections (monsters, items, NPCs and shops, world, quests, walkthrough, skills, mechanics, about the game) are written by separate issues in parallel (#218 to #225, #227), so adding a section must not mean editing a shared list. `compendium check` must prove that the site is complete (every entity has a page or an anchor), that every internal link resolves, and that the files are what a fresh build writes.

The repository's viewers (ADR 0010, `tools/mtport/htmlpage.py`) are already static pages made with the Python standard library: escaped text, a sortable and filterable table, deterministic output.

## Options considered

1. **A site tool (MkDocs, Hugo, Jekyll)**: themes, search and navigation come ready. But each is a new dependency (Python packages or a Go binary) to pin on every OS, their output carries versions and timestamps unless configured away, their Markdown flavours accept raw HTML, and the knowledge-base pages would still need a generator of ours that writes Markdown for them to render. Two layers for one site.
2. **Standard-library Python like the viewers**: no new dependency, deterministic by construction, the pages built directly from the knowledge base, the existing table code reused. We write the shell, the search and a small Markdown reader ourselves.
3. **A JavaScript single-page app over JSON**: one page, the data loaded at run time. Needs a web server for `fetch` (not `file://`), gives search engines nothing to index and needs JavaScript for every page.

## Decision

The generator is standard-library Python in `tools/mtport/compendium/`: `site.py` (the `build` and `check` commands and the shell), `siteapi.py` (the page API), `templates.py`, `prose.py`, `defaults.py`, `assets.py` and the plugin package `sections/`.

- **Output.** `python3 -m mtport.compendium.site build` writes `build/compendium/site/`: one HTML file per page, `site.css`, `site.js` and `search-index.js`. Every link is relative and names a file (`monsters/index.html`, never a directory or a path starting with `/`). The directory holds a `.mtport-compendium-site` marker; a build replaces everything in it except `img/`, which belongs to the images step, and refuses a non-empty directory without the marker. Files are written in sorted order, JSON with sorted keys, nothing dated: two builds are byte-identical.
- **Shared CSS and script files** instead of the viewers' inline ones: the site has around a thousand pages, and one cached copy is smaller and still works from `file://`.
- **Sections are plugins.** Each module in `mtport.compendium.sections` exposes `SECTION = Section(id, title, order, kinds, pages, blurb)`, found with `pkgutil.iter_modules`. `pages(ctx)` yields `Page` objects. A plugin whose `id` matches a default section replaces it; `defaults.py` has a default section and page builder for every knowledge-base kind, so the site is complete before any plugin exists, and a kind no section claims goes to an "Other" section. Shop listings, levels, warps and trades are anchored rows of their section's table rather than pages of their own.
- **Link markers.** A builder never computes a URL: `ctx.ref(kind, id)`, `ctx.href(kind, id)` and `ctx.url(path)` return markers, resolved after every page is known to a path relative to the page holding them. A page claims the entities it documents with `page.documents(kind, id, anchor)`; a whole-page claim wins over an anchor, two whole-page claims are an error, and a link to an entity no page documents is a build error.
- **Game text** (names, descriptions, dialogue) is put in an element with `data-verbatim` (`siteapi.verbatim`, `ctx.name`), including the `<title>` of an entity page. The prose lint (#212) skips those elements; `check` runs it on every page when `mtport.prose_lint` is present.
- **Prose** is a small Markdown subset (headings, paragraphs, one-level lists, block quotes, pipe tables, bold, emphasis, code, links to site paths, same-page anchors and `https://` URLs) plus entity references `{{kind:id}}` and `{{kind:id|label}}`. The first line's `# ` heading is the page title. Raw HTML, an unknown reference or anything else outside the subset is a build error with the file and line. A builder uses a prose file with `ctx.prose(path)`; a prose file nobody used becomes a page of its own at the same path and is listed under "Articles" on its section's index. So no game text is ever committed: prose names things only through references.
- **Search** is a JSON list (English name, Korean name, aliases, kind, URL) of every documented entity and every other page, written as `search-index.js` (`window.MTSEARCH = [...]`) so that a `<script>` element loads it from `file://`, where `fetch` of a JSON file is refused. The header's search box loads it on first focus and lists exact, then prefix, then substring matches after NFKC case folding, with the keyboard (up and down, Enter, Escape) and the ARIA combobox pattern; `search.html?q=` lists every match. `site.search` is the same function in Python for the tests.
- **Shell.** A skip link, a header with the site name and search, the section navigation, breadcrumbs (Home, the section, the page), `<main>` and a footer with ADR 0121's disclaimer (one constant, `site.DISCLAIMER`), its "About this site" link to `docs/legal.md#the-compendium-site` and a link to the source.
- **Themes and accessibility.** Colours are CSS custom properties with a dark set under `prefers-color-scheme: dark`; every text colour meets WCAG AA (4.5:1) on every background in both themes, checked by a test from the theme table. Focus is a visible 3 px outline. Every `<img>` has alt text (the manifest's, else the entity's name). The layout is one column below 720 px; wide tables scroll inside their own box, so no page scrolls sideways at 320 px.
- **`check`** builds in memory and reports: every relative `href`, `src` and form `action` resolves to a file of the site (an `img/` file on disk) and every `#fragment` to an element id, no link is absolute, every search entry points at a page (and anchor) that exists, every knowledge-base entity has a page or an anchor (missing ones listed, counts per kind), the prose lint's findings, and every file on disk equals the fresh build.

## Consequences

- No new dependency; the site builds wherever the tests run, and CI tests it on a synthetic knowledge base (`tests/test_compendium_site.py`, `tests/compendiumfixtures.py`), including loading every page and link from `file://` and from a `/maplethief-port/` sub-path under `http.server`, and the site script's search against the Python one under Node when Node is installed.
- We maintain our own Markdown reader. It stays small on purpose: anything it does not know fails the build rather than rendering wrongly.
- Section children (#218 to #225, #227) each add one module and can build in parallel; `check`'s completeness report tells them what is still missing. Page layouts follow the style guide (#212); the defaults are deliberately plain.
- The search index grows with the knowledge base (around 100 bytes per entity) and is loaded only when the search box is used.
- If the site outgrows this (full-text search over prose, many more pages), a client-side full-text index can be added beside `search-index.js` without changing the page API.
