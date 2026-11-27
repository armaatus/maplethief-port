"""The playthrough checklist (#296): every quest, boss, job advancement, climb and scene in the
order a player meets them, generated from the knowledge base for #79 and #92.

The order is the walkthrough's (:mod:`mtport.compendium.sections.walkthrough`): its parts, the
story quests of each part in an order that satisfies every prerequisite, and the side quests
that open there. A boss, job advancement, climb or scene follows the quest that brings it: the
quest that asks for the boss's kill, or the latest quest the conditions of the advancement,
of the way into the climb or of the scene need accepted or done. One that names no quest goes
at the end of its chapter, or to the post-game when it has no chapter.

Two files, both in git-ignored ``build/checklists/`` (they hold the game's names, so they are
filled in locally and never committed):

- ``playthrough.md`` (#79): the tutorial, chapters 1 to 3 part by part, the endings and the
  post-game, both job branches, the three climbs, save and load. Each row has a tick box and
  names the automated run that covers it (:data:`RUNS`), or ``none yet``;
- ``platforms.md`` (#92): a shorter pass per platform with a column per preset (Original,
  Original + controller, Enhanced), a controller-first section and the Switch row.

:data:`RUNS` is ours: the ``play.*`` CTests that play an entity, from
docs/testing/chapter-1-quests.md, chapter-2-quests.md and chapter-3-quests.md. #295's run
coverage report can extend it.

CLI::

    PYTHONPATH=tools python3 -m mtport.compendium.playthrough [--kb DIR] [--out DIR]

Prints the counts and exits 1 when the knowledge base is missing.
"""

from __future__ import annotations

import argparse
import sys
from collections.abc import Iterable, Mapping
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from mtport.compendium.sections import monsters as mons
from mtport.compendium.sections import walkthrough as wt
from mtport.compendium.siteapi import BuildError, Context, Entity, Images, load_kb_dir, name_of

_REPO = Path(__file__).resolve().parents[3]
DEFAULT_KB = _REPO / "build" / "compendium" / "kb"
DEFAULT_OUT = _REPO / "build" / "checklists"
NONE_YET = "none yet"

# The CTests that play an entity to the end and check it in guest memory
# (docs/testing/chapter-1-quests.md, chapter-2-quests.md, chapter-3-quests.md). Keys are
# "kind:id"; a climb's id is its area.
_CH1 = "play.chapter_1_"
_CH2 = "play.chapter_2_"
_CH3 = "play.chapter_3_"
RUNS: dict[str, str] = {
    **{f"quest:0-main-{n}": _CH1 + "a" for n in range(7)},
    "event:common-portal281-chapter_end": _CH1 + "a",
    "quest:1-main-7": _CH1 + "a",
    "quest:1-main-8": _CH1 + "a",
    "quest:1-main-21": _CH1 + "a",
    "job_advancement:1-1-0": _CH1 + "a",
    **{f"quest:1-sub-{n}": _CH1 + "b" for n in (2, 3, 4, 5, 6, 30)},
    "quest:1-main-9": _CH1 + "c",
    "event:chapter1-PerionMap-scene-15": _CH1 + "c",
    **{f"quest:1-main-{n}": _CH1 + "d" for n in range(10, 16)},
    "quest:1-sub-22": _CH1 + "d",
    "quest:1-sub-23": _CH1 + "d",
    **{f"quest:1-sub-{n}": _CH1 + "e" for n in (21, 26, 31)},
    **{f"quest:1-sub-{n}": _CH1 + "f" for n in (7, 8, 27, 33, 34)},
    **{f"quest:1-sub-{n}": _CH1 + "g" for n in (15, 16, 24, 25, 28)},
    "job_advancement:1-2-2": _CH1 + "g",
    "climb:2": _CH1 + "g",
    "quest:1-sub-29": _CH1 + "h",
    "quest:1-sub-9": _CH1 + "h",
    "quest:1-sub-10": _CH1 + "i",
    "quest:1-sub-14": _CH1 + "i",
    "quest:1-main-16": _CH1 + "j",
    "quest:1-main-17": _CH1 + "j",
    "quest:1-main-18": _CH1 + "k",
    "quest:1-main-19": _CH1 + "k",
    "monster:64": _CH1 + "k",
    "quest:1-main-20": _CH1 + "l",
    "quest:1-sub-11": _CH1 + "l",
    "event:chapter1-Pirate-chapter_end": _CH1 + "l",
    "quest:2-main-0": _CH2 + "a",
    "quest:2-main-1": _CH2 + "a",
    "quest:2-main-2": _CH2 + "b_synthetic",  # from a raised save (levels 34-60)
    **{f"quest:2-main-{n}": _CH2 + "c" for n in (3, 4)},
    **{f"quest:2-sub-{n}": _CH2 + "c" for n in (0, 1, 37)},
    "event:chapter2-WolfMap-boss": _CH2 + "c",
    "monster:66": _CH2 + "c",
    **{f"quest:2-main-{n}": _CH2 + "d" for n in (5, 6)},
    "quest:2-sub-2": _CH2 + "d",
    "event:chapter2-PerionMap-scene-20": _CH2 + "d",
    **{f"quest:2-main-{n}": _CH2 + "e" for n in (7, 71, 72)},
    **{f"quest:2-sub-{n}": _CH2 + "e" for n in (35, 36)},
    "quest:2-main-8": _CH2 + "f_synthetic",  # from a raised save (levels 62-70)
    "quest:2-main-9": _CH2 + "f_synthetic",
    "monster:68": _CH2 + "f_synthetic",
    "event:chapter2-Pirate_Orbis-chapter_end": _CH2 + "f_synthetic",
    **{f"quest:3-main-{n}": _CH3 + "a" for n in (0, 1)},
    "quest:3-sub-0": _CH3 + "a",
    "quest:3-main-2": _CH3 + "b",
    **{f"quest:3-sub-{n}": _CH3 + "b" for n in (4, 15, 58)},
    "event:chapter3-Humanoid3-scene-16": _CH3 + "b",
    **{f"quest:3-main-{n}": _CH3 + "c" for n in (3, 4, 5)},
    "event:chapter3-HomunMap-boss": _CH3 + "c",
    "monster:67": _CH3 + "c",
    "quest:3-main-6": _CH3 + "d",
    "quest:3-sub-25": _CH3 + "d",
    "job_advancement:3-4-same": _CH3 + "d",
    "quest:3-main-7": _CH3 + "e_synthetic",  # from a raised save (levels 72-99)
    "event:chapter3-BalogMap-boss": _CH3 + "e_synthetic",
    "monster:69": _CH3 + "e_synthetic",
    # Chapter 3's side quests (#301): sides 1 and 16 on BrokerHan's path, 42 the 3rd job below.
    **{f"quest:3-sub-{n}": _CH3 + "sides_a" for n in (2, 3, 5, 16, 17)},
    **{f"quest:3-sub-{n}": _CH3 + "sides_b" for n in (6, 7, 8, 10)},
    **{f"quest:3-sub-{n}": _CH3 + "sides_c" for n in (9, 11, 12, 13, 18)},
    "quest:3-sub-34": _CH3 + "climb",
    "climb:6": _CH3 + "climb",
    "quest:3-sub-14": _CH3 + "side_14",
    # From a raised save (levels 73-99).
    **{f"quest:3-sub-{n}": _CH3 + "sides_10_a_synthetic" for n in (20, 22, 23, 24)},
    **{f"quest:3-sub-{n}": _CH3 + "sides_10_b_synthetic" for n in (26, 27, 28, 29, 30, 31)},
    "quest:3-sub-1": "play.jobs_3_1_synthetic",  # the Dark Lord's path; BrokerHan's: sides_a
    "quest:3-sub-42": "play.jobs_3_4_assassin_synthetic",
    # From built start states (#302, docs/testing/skills-and-jobs.md).
    "job_advancement:1-2-1": "play.jobs_1_2_assassin_synthetic",
    "job_advancement:2-2-1": "play.jobs_2_2_assassin_synthetic",
    "job_advancement:2-2-2": "play.jobs_2_2_bandit_synthetic",
    "job_advancement:2-3-same": "play.jobs_2_3_assassin_synthetic",
    "job_advancement:3-1-0": "play.jobs_3_1_synthetic",
    "job_advancement:3-2-1": "play.jobs_3_2_assassin_synthetic",
    "job_advancement:3-2-2": "play.jobs_3_2_bandit_synthetic",
    "job_advancement:3-3-same": "play.jobs_3_4_assassin_synthetic",
}

# The ending's two scenes (watched; play.chapter_3_e_skipped_synthetic skips them) and the
# post-game save, quit and relaunch (docs/testing/chapter-3-quests.md).
FINAL_RUN = _CH3 + "e_synthetic"
POST_GAME_RUN = "play.chapter_3_postgame_relaunch"

# Save and load (docs/re/autosave.md), our words: (row, automated run).
SAVE_ROWS: tuple[tuple[str, str], ...] = (
    ("Save from the menu in a town, quit, start again and load the slot", "play.save_quit"),
    ("The loaded game has the hero, map, items and quests of the save", "play.save_relaunch"),
    ("The first start imports an older save folder", "play.save_migrate"),
    ("A save state and its restore on a field map: the same play after it", "play.save_state"),
    ("Quick-save (F5) and quick-load (F9) with save states on", "play.quick_state"),
    ("The autosave on quit in a town, then load that progress", "play.autosave_reload"),
)
NO_SAVE_WORDS = {
    "tutorial": "the tutorial",
    "boss": "boss maps",
    "climb": "climbs",
    "Monster Carnival": "Monster Carnival rooms",
}

# #92: the presets, the platforms and the controller-first checks (docs/testing/handoffs).
PRESETS = ("Original", "Original + controller", "Enhanced")
PLATFORMS: tuple[tuple[str, str], ...] = (
    ("macOS", "the `mac-player` kit"),
    ("macOS under Rosetta", "the `mac-player` kit with `arch -x86_64`"),
    ("Windows", "the `win-player` kit"),
    ("iPhone", "`scripts/build ios`"),
    ("Linux and the Steam Deck", "`scripts/linux-game`"),
)
SWITCH = (
    "Switch",
    "the Switch checklist (docs/testing/switch-checklist.md), signed off on #245",
)
CONTROLLER_FIRST = (
    "Plays with the defaults and no launcher",
    "The button glyphs match the pad",
    "Legible at 1280 x 800",
    "Text entry with a controller (the character name)",
    "A steady 20 ticks a second (#142)",
)


@dataclass
class Row:
    """One tick box: an entity (``kind:id``) or our own words (``kind`` empty)."""

    kind: str
    id: str
    text: str
    run: str = NONE_YET

    @property
    def key(self) -> str:
        return f"{self.kind}:{self.id}"


@dataclass
class Section:
    title: str
    rows: list[Row] = field(default_factory=list)
    parts: list[Section] = field(default_factory=list)
    note: str = ""


@dataclass
class Checklist:
    sections: list[Section]

    def rows(self) -> Iterable[Row]:
        def walk(ss: Iterable[Section]) -> Iterable[Row]:
            for s in ss:
                yield from s.rows
                yield from walk(s.parts)

        yield from walk(self.sections)

    def ids(self, kind: str) -> set[str]:
        """The ids of ``kind`` the checklist ticks (a row in two sections counts once)."""
        return {r.id for r in self.rows() if r.kind == kind}


# --- what a row names --------------------------------------------------------------------------


def _row(kind: str, eid: str, text: str) -> Row:
    return Row(kind, eid, text, RUNS.get(f"{kind}:{eid}", NONE_YET))


def quest_row(ctx: Context, qid: str, side: bool = False) -> Row:
    q = ctx.kb.get("quest", qid) or {}
    what = "side quest" if side else "story"
    return _row("quest", qid, f"Quest `{qid}` {name_of(q)} ({what})")


def boss_row(ctx: Context, mid: str) -> Row:
    m = ctx.kb.get("monster", mid) or {}
    maps = ", ".join(f"`{r['id']}`" for r in wt._refs(m.get("spawns_on")))
    return _row("monster", mid, f"Boss `{mid}` {name_of(m)} (on {maps or 'no map'})")


def advancement_row(ctx: Context, aid: str) -> Row:
    a = ctx.kb.get("job_advancement", aid) or {}
    job = a.get("job_name") or "the same job"
    return _row(
        "job_advancement",
        aid,
        f"Job advancement `{aid}`: advancement {a.get('advancement')}, {job}",
    )


def event_row(ctx: Context, eid: str) -> Row:
    e = ctx.kb.get("event", eid) or {}
    maps = ", ".join(f"`{r['id']}`" for r in wt._refs(e.get("maps")))
    what = {"scene": "Scene", "chapter_end": "Chapter end", "boss": "Boss fight"}
    words = what.get(str(e.get("type")), "Event")
    args = e.get("args") or []
    number = f" {args[0]}" if e.get("type") == "scene" and args else ""
    return _row("event", eid, f"{words}{number} `{eid}` (on {maps or 'no map'})")


def climb_row(ctx: Context, area: str, maps: list[str]) -> Row:
    a = ctx.kb.get("area", area) or {}
    where = ", ".join(f"`{m}`" for m in maps)
    return _row("climb", area, f"Climb in area `{area}` {name_of(a)}: maps {where}, to the top")


# --- placement ---------------------------------------------------------------------------------


def condition_quests(ctx: Context, conditions: Any, chapters: Iterable[int]) -> list[str]:
    """The quests the first way through ``conditions`` needs accepted or done."""
    return wt._event_quests(ctx, {"conditions": conditions, "chapters": list(chapters)})


def climbs(ctx: Context) -> dict[str, list[str]]:
    """Area id to its climb maps (``no_save == "climb"``), in natural order."""
    out: dict[str, list[str]] = {}
    for m in ctx.kb.entities("map"):
        if m.get("no_save") == "climb":
            areas = wt._refs(m.get("area"))
            area = str(areas[0]["id"]) if areas else str(m.get("area_no"))
            out.setdefault(area, []).append(str(m["id"]))
    return out


def climb_entry(ctx: Context, maps: list[str]) -> tuple[list[str], int | None]:
    """The quests the easiest way into a climb needs, and its chapter: of the used warps into
    its maps from outside it, the one that needs the fewest quests."""
    best: tuple[list[str], int | None] | None = None
    for mid in maps:
        for r in wt._refs((ctx.kb.get("map", mid) or {}).get("warps_in")):
            wp = ctx.kb.get("warp", str(r["id"])) or {}
            if not wp.get("used", True):
                continue
            froms = [f for f in wp.get("from") or [] if isinstance(f, Mapping)]
            if froms and all(str(f.get("map")) in maps for f in froms):
                continue  # a step inside the climb
            chapters = [int(c) for c in wp.get("chapters") or [] if isinstance(c, int)]
            got = condition_quests(ctx, wp.get("conditions"), chapters)
            if best is None or len(got) < len(best[0]):
                best = (got, min(chapters, default=None))
    return best or ([], None)


def _chapter_of(ctx: Context, qid: str) -> int:
    return wt.page_chapter(ctx.kb.get("quest", qid) or {})


def build(ctx: Context) -> Checklist:
    w = wt.walk(ctx)
    seq: list[str] = []  # every quest in the order the checklist lists them
    for p in w.parts:
        seq += w.story.get(p.slug, []) + w.side.get(p.slug, [])
    rank = {q: i for i, q in enumerate(seq)}

    after: dict[str, list[Row]] = {}  # quest -> rows that follow it
    loose: dict[int, list[Row]] = {}  # chapter -> rows with no quest
    post: list[Row] = []

    def place(row: Row, quests: Iterable[str], chapter: int | None) -> None:
        known = [q for q in quests if q in rank]
        if known:
            after.setdefault(max(known, key=rank.__getitem__), []).append(row)
        elif chapter in wt.CHAPTERS:
            loose.setdefault(chapter, []).append(row)
        else:
            post.append(row)

    for m in ctx.kb.entities("monster"):
        if mons.is_boss(m):
            hunted = [str(r["id"]) for r in wt._refs(m.get("hunted_by_quests"))]
            chs = [_chapter_of(ctx, q) for q in hunted]
            place(boss_row(ctx, str(m["id"])), hunted, min(chs) if chs else None)
    for a in ctx.kb.entities("job_advancement"):
        ch = wt._num(a.get("chapter"))
        place(
            advancement_row(ctx, str(a["id"])), condition_quests(ctx, a.get("conditions"), [ch]), ch
        )
    for area, maps in sorted(climbs(ctx).items(), key=lambda kv: wt._num(kv[0])):
        quests, ch = climb_entry(ctx, maps)
        place(climb_row(ctx, area, maps), quests, ch)
    for e in ctx.kb.entities("event"):
        if e.get("type") == "scene" and e.get("used", True):
            chs = [int(c) for c in e.get("chapters") or [] if isinstance(c, int)]
            place(event_row(ctx, str(e["id"])), wt._event_quests(ctx, e), min(chs, default=None))

    def rows_for(quests: list[str], side: bool) -> list[Row]:
        out = []
        for q in quests:
            out.append(quest_row(ctx, q, side))
            out += after.get(q, [])
        return out

    sections: list[Section] = []
    tutorial = Section(
        "Tutorial",
        [Row("", "intro", "The intro scene (scene 0) on a new game, ending on map `2-8`")],
    )
    sections.append(tutorial)
    for ch in wt.CHAPTERS:
        chapter = Section(f"Chapter {ch}")
        for p in w.parts:
            if p.chapter != ch:
                continue
            rows = rows_for(w.story.get(p.slug, []), False) + rows_for(w.side.get(p.slug, []), True)
            target = tutorial if p.slug == "tutorial" else chapter
            if target is tutorial:
                tutorial.rows += rows
            else:
                chapter.parts.append(Section(f"Part {wt.part_number(p)}: {p.title}", rows))
        if loose.get(ch):
            chapter.parts.append(Section(f"Any time in chapter {ch}", loose[ch]))
        sections.append(chapter)

    endings = Section("The endings and the post-game")
    for en in wt.endings(ctx):
        endings.rows.append(event_row(ctx, en.event))
    for number, builder, _steps, _boxes, end_map in wt.FINAL_SCENES:
        endings.rows.append(
            Row(
                "",
                f"final-scene-{number}",
                f"Final scene {number} (`{builder}`), on to `{end_map}`",
                FINAL_RUN,
            )
        )
    endings.rows.append(
        Row("", "post-game", "After the ending: play on, save, quit and load", POST_GAME_RUN)
    )
    endings.rows += post
    sections.append(endings)

    branches = Section(
        "Both job branches",
        note="Play the chapters again from the save before the second advancement, taking the "
        "other branch, and tick each branch's advancements and skills.",
    )
    for job in sorted({str(a.get("job_name")) for a in _jobbed(ctx)}):
        rows = [
            advancement_row(ctx, str(a["id"])) for a in _jobbed(ctx) if a.get("job_name") == job
        ]
        rows += [
            _row("skill", str(s["id"]), f"Skill `{s['id']}` {name_of(s)}")
            for s in ctx.kb.entities("skill")
            if s.get("branch") == job
        ]
        branches.parts.append(Section(job, rows))
    sections.append(branches)

    sections.append(
        Section(
            "The three climbs",
            [
                climb_row(ctx, a, ms)
                for a, ms in sorted(climbs(ctx).items(), key=lambda kv: wt._num(kv[0]))
            ],
            note="Each from the bottom to the top without a fall, and the menu refuses to save.",
        )
    )

    save = Section(
        "Save and load", [Row("", f"save-{i}", t, run) for i, (t, run) in enumerate(SAVE_ROWS)]
    )
    by_kind: dict[str, list[str]] = {}
    for m in ctx.kb.entities("map"):
        if m.get("no_save"):
            by_kind.setdefault(str(m["no_save"]), []).append(f"`{m['id']}`")
    for kind, maps in by_kind.items():
        words = NO_SAVE_WORDS.get(kind, kind)
        save.rows.append(Row("", f"no-save-{kind}", f"No save on {words}: {', '.join(maps)}"))
    sections.append(save)
    return Checklist(sections)


def _jobbed(ctx: Context) -> list[Entity]:
    """The job advancements that pick a branch (not the first, not the ones keeping the job)."""
    return [
        a
        for a in ctx.kb.entities("job_advancement")
        if a.get("job_name") not in (None, "Rogue") and not a.get("keeps_job")
    ]


# --- rendering ---------------------------------------------------------------------------------


def _rows_md(rows: list[Row]) -> list[str]:
    return [f"- [ ] {r.text}. Automated: {r.run}" for r in rows]


def render_playthrough(cl: Checklist) -> str:
    out = [
        "# Playthrough checklist",
        "",
        "Generated from the knowledge base by `python3 -m mtport.compendium.playthrough` (#296)"
        " for #79. Local only: it holds the game's names, so it is never committed. Tick each"
        " row as you play; *Automated* names the CTest that already plays it.",
        "",
    ]
    for s in cl.sections:
        out += [f"## {s.title}", ""]
        if s.note:
            out += [s.note, ""]
        if s.rows:
            out += _rows_md(s.rows) + [""]
        for p in s.parts:
            out += [f"### {p.title}", ""]
            if p.note:
                out += [p.note, ""]
            out += _rows_md(p.rows) + [""]
    return "\n".join(out)


def milestones(cl: Checklist) -> list[Row]:
    """The rows #92's shorter pass repeats: bosses, job advancements, climbs, scenes, chapter
    ends and save and load, in playthrough order (the quests are #79's)."""
    seen: set[str] = set()
    out = []
    for r in cl.rows():
        if r.kind == "quest" or r.kind == "skill" or r.key in seen:
            continue
        seen.add(r.key)
        out.append(r)
    return out


def render_platforms(cl: Checklist) -> str:
    rows = milestones(cl)
    head = "| | " + " | ".join(PRESETS) + " |"
    rule = "| --- |" + " --- |" * len(PRESETS)
    out = [
        "# Playthrough on every platform",
        "",
        "Generated by `python3 -m mtport.compendium.playthrough` (#296) for #92: the #79"
        " playthrough, shorter, on each platform's player build with the import on first start,"
        " once per preset. Local only, never committed.",
        "",
        "## Sign-off",
        "",
        "| Platform | Build | " + " | ".join(PRESETS) + " | Controller-first |",
        "| --- | --- |" + " --- |" * (len(PRESETS) + 1),
    ]
    for name, build_ in (*PLATFORMS, SWITCH):
        out.append(f"| {name} | {build_} |" + " [ ] |" * (len(PRESETS) + 1))
    out.append("")
    for name, build_ in PLATFORMS:
        out += [f"## {name}", "", f"Build: {build_}.", "", head, rule]
        for r in rows:
            out.append(f"| {r.text} |" + " [ ] |" * len(PRESETS))
        out += ["", f"### {name}: controller-first", ""]
        out += [f"- [ ] {c}" for c in CONTROLLER_FIRST] + [""]
    out += [f"## {SWITCH[0]}", "", f"Use {SWITCH[1]}.", ""]
    return "\n".join(out)


def counts(cl: Checklist) -> Mapping[str, int]:
    return {k: len(cl.ids(k)) for k in ("quest", "monster", "job_advancement", "climb", "event")}


def write(ctx: Context, out: Path) -> Checklist:
    cl = build(ctx)
    out.mkdir(parents=True, exist_ok=True)
    (out / "playthrough.md").write_text(render_playthrough(cl), encoding="utf-8", newline="\n")
    (out / "platforms.md").write_text(render_platforms(cl), encoding="utf-8", newline="\n")
    return cl


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.compendium.playthrough", description=__doc__.splitlines()[0]
    )
    parser.add_argument("--kb", type=Path, default=DEFAULT_KB, help="knowledge base directory")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT, help="output directory")
    args = parser.parse_args(argv)
    try:
        ctx = Context(load_kb_dir(args.kb), Images(), None)
    except BuildError as e:
        print(f"error: {e} (scripts/compendium build, or python3 -m mtport.compendium.kb build)")
        return 1
    cl = write(ctx, args.out)
    c = counts(cl)
    print(
        f"wrote {args.out}/playthrough.md and platforms.md: {c['quest']} quests,"
        f" {c['monster']} bosses, {c['job_advancement']} job advancements, {c['climb']} climbs,"
        f" {c['event']} scenes and endings"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
