"""Section plugins of the compendium site (ADR 0122).

Each module in this package (not starting with ``_``) is one section and exposes
``SECTION``, a :class:`mtport.compendium.siteapi.Section`. Adding a section is adding a
module: no list to edit. A section whose ``id`` matches a default section
(:data:`mtport.compendium.defaults.DEFAULT_SECTIONS`: ``monsters``, ``items``, ``npcs``,
``world``, ``quests``, ``walkthrough``, ``skills``, ``mechanics``, ``about``) replaces it.

A minimal plugin::

    from mtport.compendium.siteapi import Page, Section

    def pages(ctx):
        for e in ctx.kb.entities("monster"):
            page = ctx.default_entity_page("monster", e)
            page.body += "<p>More about it.</p>"
            yield page
        yield Page("monsters/index.html", "Monsters", ctx.prose("monsters/index.md") or "")

    SECTION = Section("monsters", "Monsters", 10, ("monster",), pages)
"""

from __future__ import annotations

import importlib
import pkgutil

from mtport.compendium.siteapi import BuildError, Section


def discover() -> list[Section]:
    """Every plugin's ``SECTION``, in module-name order."""
    out = []
    for info in sorted(pkgutil.iter_modules(__path__), key=lambda i: i.name):
        if info.name.startswith("_"):
            continue
        module = importlib.import_module(f"{__name__}.{info.name}")
        section = getattr(module, "SECTION", None)
        if not isinstance(section, Section):
            raise BuildError(f"{module.__name__}: no SECTION (a siteapi.Section)")
        out.append(section)
    return out
