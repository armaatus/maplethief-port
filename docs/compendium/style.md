# Compendium style guide

This guide sets the voice, layout and terms of every page of the MapleThief compendium, the public site about MapleStory Thief Edition (iOS, 2010). The aim is a reference that reads like an established game wiki: the entity pages of Bulbapedia, the infoboxes and drop tables of the MapleStory fan wikis, the walkthroughs of StrategyWiki, the unused-content notes of The Cutting Room Floor and the dense tables of Serebii. Those sites are models for structure only; no text is copied from them.

The mechanical rules are enforced by the prose lint, `PYTHONPATH=tools python3 -m mtport.prose_lint`, which `scripts/lint` runs on every change. The rest is for the writer and the reviewer.

## The lint

- **What it reads:** with no arguments, the tracked `docs/compendium/**/*.md` and the site generator's templates (`tools/mtport/compendium/templates/`, once they exist). Given paths, it reads those files, or every `.md`, `.html` and `.htm` file under a directory, so `PYTHONPATH=tools python3 -m mtport.prose_lint build/compendium/site` checks the built site.
- **Rules:** `em-dash`, `en-dash`, `other-dash`, `double-hyphen`, `banned-phrase`, `first-person`, `term`, `emoji` and `title-case-heading`, each one described in the sections below. One line per finding, `path:line:col: rule: message`; the exit status is 1 when there is any.
- **Exempt in Markdown:** fenced code blocks, code spans, `{{...}}` references and template tags, link targets, inline HTML tags and any inline element with a `data-verbatim` attribute.
- **Exempt in HTML:** any element with a `data-verbatim` attribute and everything inside it, `<script>`, `<style>`, `<code>` and `<pre>`. The site generator puts `data-verbatim` on every piece of game text, entity names included.
- **From Python:** `mtport.prose_lint.lint_text(text, kind="md")` (or `kind="html"`) returns the findings, so the site generator's own check can call it.

## Whose words are whose

- **Prose is the project's own.** Leads, notes, strategy and walkthrough steps are written by hand in `docs/compendium/**/*.md` and are covered by this guide.
- **Game text is the game's.** Names, descriptions, dialogue and quest text are never pasted into the repository. Pages pull them from the knowledge base at build time, and the built site marks them with a `data-verbatim` attribute. The lint skips that text, so the game's own spelling and punctuation stay as they are.
- **Game names in prose are facts** and may be written out ("Blue Snail drops Snail Shell"). Prefer an entity reference where the page links to the entity: `{{monster:1}}`, `{{item:etc:3}}`, `{{npc:10}}`, `{{map:2-3}}`, `{{skill:5}}`, `{{quest:main:10}}`. The site renders the name and the link.

## Voice

- **Encyclopedic, third person, present tense.** "Ligator drops Ligator Skin." "The quest opens after level 15." Describe what the game does, not what a player might feel about it.
- **Second person only in walkthrough steps,** as plain instructions: "Talk to Manji." "Collect 12 Leaves from Dark Stumps."
- **No first person.** Never `I`, `we`, `our` or `let's`. The lint flags these.
- **No hype and no filler.** State the fact and stop. Banned phrases (the lint flags each):

  `it's worth noting`, `it is worth noting`, `worth mentioning`, `needless to say`, `it goes without saying`, `in conclusion`, `in summary`, `to sum up`, `first and foremost`, `at the end of the day`, `delve`, `delves`, `delving`, `dive into`, `deep dive`, `embark`, `tapestry`, `testament to`, `plays a crucial role`, `plays a vital role`, `game-changer`, `game changer`, `must-have`, `seamless`, `seamlessly`, `unleash`, `look no further`, `fear not`, `whether you're`, `nestled`, `bustling`, `vibrant`, `amazing`, `awesome`, `incredible`, `epic`

- **No emoji** and no decorative symbols. The lint flags pictographs and dingbats.
- **Short sentences, one fact each** where the fact is a number. Put long lists of numbers in a table.
- **Name the source of a claim** when it is not obvious: a table column, a script, a test that played it.

## Punctuation

- **No em dash (U+2014) and no en dash (U+2013)** in hand-written prose, anywhere: body, headings, captions, tables, alt text. Use a comma, a colon, parentheses or two sentences instead.
- **Ranges** use "to" in prose ("levels 10 to 15", "drops 3 to 5 meso") and a plain hyphen in table cells (`10-15`).
- **No double hyphen** (`--`) as a dash.
- **Serial comma:** "Red Potion, Blue Potion, and Elixir".
- **Straight quotes** in the source files. Quotation marks are for words used as words; game text is never quoted in prose (it is pulled from the knowledge base).
- **Lists:** a list item that is a full sentence ends with a full stop; a fragment does not. Do not mix the two in one list.

## Headings

- **Sentence case:** only the first word and proper nouns are capitalised ("Drops and locations", not "Drops And Locations"). The lint flags a capitalised word after the first unless it is a name from the glossary below, an acronym (EXP, NPC, HP) or the first word after a colon.
- An entity name in a heading is written as a reference (`{{monster:1}}`) or comes from the knowledge base, so it never trips the rule. A name the lint does not know and that appears in headings belongs in the glossary.
- **Fixed section order per page type,** given in the templates below. Leave a section out when it has nothing to say; never reorder.
- No heading ends with punctuation. No heading is a question.

## Numbers and units

- **Thousands separators:** 1,250 and 12,000 (not 1250 or 12 000). Four-digit years and ids have none.
- **Meso:** the currency is "meso", singular and plural ("costs 1,500 meso"); never `mesos`. The lint flags it.
- **Stats** are written as the game writes them, in capitals: EXP, HP, MP, ATT, DEF, ACC, AVOID, STR, DEX, INT, LUK. "Gives 15 EXP", "+3 ATT".
- **Levels:** "level 12" in prose, "Lv. 12" only in infobox cells.
- **Drop chances** are a percentage with the game's per-mille value beside it: "2.5% (25/1000)". Never round away a non-zero chance: "0.1% (1/1000)".
- **Times** are in seconds: "lasts 30 seconds" in prose, `30 s` in table cells. Convert game ticks to seconds and give the tick count only on a page about timing.
- **Coordinates and map ids** use the game's (area, map) numbering, written `2.3` in prose and `{{map:2-3}}` as a reference.
- Numbers below ten are words in running prose ("three quests") unless they carry a unit or a stat ("3 HP", "level 5").

## Page templates

Every entity page has the same three parts in this order: a **lead** (one or two sentences: what it is and where it appears), an **infobox** (filled from the knowledge base), then the **sections** for its type. Sections are `##` headings. The examples below use made-up names and numbers.

### Monster page

Infobox: name, Korean name, sprite, level, HP, EXP, meso, boss (yes or no), found on (maps), id.

Sections, in order: Drops, Locations, Behaviour, Strategy, Notes, Trivia.

```markdown
Moss Snail is a level 3 monster found in the fields east of {{map:2-1}}.

## Drops

| Item | Chance |
|---|---|
| {{item:etc:40}} | 60% (600/1000) |
| {{item:equip:7}} | 0.5% (5/1000) |
| meso | 80%, 8-12 meso |

## Locations

Moss Snail appears on {{map:2-1}} and {{map:2-2}}, five at a time.

## Behaviour

It walks back and forth on its platform and turns at the edges. Contact deals 4 HP.

## Strategy

Two hits of a basic attack defeat it from level 2.

## Notes

The level column of the monster table is probable, not confirmed
([spec](../formats/data-tables.md#mobdat), field `level`).
```

### Item page

Infobox: name, Korean name, icon, type (equipment slot or item type), required level, stats (equipment: minimum to maximum of each stat), price, sell price, stack size, id.

Sections, in order: Obtained from (drops, shops, quests), Used in (quests, crafting), Notes, Trivia.

```markdown
Granite Claw is a level 15 claw sold in Kerning City.

## Obtained from

| Source | Detail |
|---|---|
| {{shop:3}} | 4,500 meso |
| {{monster:21}} | 0.2% (2/1000) |

## Used in

{{quest:sub:12}} takes one as its reward.

## Notes

A dropped Granite Claw rolls each stat between its minimum and maximum; a bought one has the minimum.
```

### NPC page

Infobox: name, Korean name, sprite, location (map and position), role (shop, quest giver, story), id.

Sections, in order: Location, Shop (link to the shop page), Quests (given and ended here), Story, Notes.

```markdown
Old Tamsin is the weapon merchant of {{map:0-0}}.

## Location

She stands at the west end of the town square.

## Shop

Old Tamsin runs {{shop:2}}.

## Quests

| Quest | Role |
|---|---|
| {{quest:sub:4}} | Starts and ends |
```

### Shop page

Infobox: shop name, keeper (NPC), location, id.

Sections, in order: Stock, Notes.

```markdown
Shop 2 is Old Tamsin's weapon shop in {{map:0-0}}.

## Stock

| Item | Price | Required level |
|---|---|---|
| {{item:equip:3}} | 1,200 meso | 10 |
| {{item:etc:11}} | 500 meso | 10 |

## Notes

The stock never changes during the game.
```

### Map page

Infobox: name, Korean name, area, map id (`2.3`), size in tiles, music track, town (yes or no), boss map (yes or no).

Sections, in order: Overview, Monsters, NPCs, Portals (exits and where they lead), Items, Notes, Unused content.

```markdown
Map 4.2 is a field of the area around Orbis.

## Monsters

| Monster | Count |
|---|---|
| {{monster:24}} | 6 |

## Portals

| Portal | Leads to |
|---|---|
| East edge | {{map:4-3}} |
| Rope, top left | {{map:4-1}} |

## Unused content

The map data has a portal cell with no script; it leads nowhere.
```

### Skill page

Infobox: name, Korean name, icon, branch, tier, type (basic attack, passive, buff, active attack), maximum level, MP cost.

Sections, in order: Effect (a per-level table), Requirements, Strategy, Notes.

```markdown
Moss Step is a tier 2 active attack of the Bandit branch.

## Effect

| Level | Damage | MP |
|---|---|---|
| 1 | 120% | 8 |
| 10 | 165% | 12 |
| 20 | 215% | 16 |

## Requirements

Moss Step needs level 3 of {{skill:3}}.
```

### Quest page

Infobox: name, type (main or sub), chapter, giver (NPC), level, prerequisites, rewards (EXP, meso, items).

Sections, in order: Overview, Walkthrough (numbered steps, second person), Rewards, Notes. Spoilers follow the rules below.

```markdown
Missing Seeds is a sub quest of chapter 1, given by {{npc:5}}.

## Walkthrough

1. Talk to {{npc:5}} in {{map:0-0}}.
2. Collect 10 {{item:etc:44}} from {{monster:4}} on {{map:0-2}}.
3. Return to {{npc:5}}.

## Rewards

| Reward | Amount |
|---|---|
| EXP | 300 |
| meso | 1,000 |
| {{item:etc:3}} | 5 |
```

### Chapter page

Infobox: chapter number, areas, level range, main quests, bosses.

Sections, in order: Overview, Main quests (in order), Sub quests, Bosses, Story (spoiler block), Notes.

```markdown
Chapter 2 covers the Orbis area and runs from about level 20 to 35.

## Main quests

1. {{quest:main:20}}
2. {{quest:main:21}}

## Bosses

{{monster:64}} ends the chapter.

<details class="spoiler"><summary>Story</summary>

The chapter's story in a few sentences, in the writer's own words.

</details>
```

### Guide page

Guides are topic pages: beginner's guide, training spots, skill builds, the port's settings. Infobox: none.

Sections, in order: Overview, then the guide's own sections, then See also.

```markdown
This guide lists good places to train between levels 10 and 30.

## Levels 10 to 15

{{map:2-3}} has 8 {{monster:6}} close together and a rope to escape.

## See also

- [Monster list](../monsters/)
```

## Terms

The game's own names, as the English and Korean tables spell them. Write them exactly like this, including capitals, hyphens and full stops. The heading lint accepts every word of the English column, so a name that keeps appearing in headings belongs here.

Source: `item_etc` and the other data tables are `English/data/*.dat` and `Korean/data/*.dat` ([spec](../formats/data-tables.md)); place names come from the return scrolls of `item_etc`, the rest from the scripts' text (`Scripts.strings`, `ScriptExtra.strings`).

| English | Korean | Kind | Source |
|---|---|---|---|
| Maple World | 메이플 월드 | world | scripts |
| Robin | 로빈 | hero | scripts |
| Thief | 도적 | job | scripts |
| Rogue | | first skill branch | `skill.branch` 0 |
| Assassin | 어쌔신 | claw branch | scripts, `skill.branch` 1 |
| Bandit | 시프 | dagger branch | scripts, `skill.branch` 2 |
| Kerning City | 커닝시티 | town (area 0) | `item_etc` 25 |
| Perion | 페리온 | town (area 1) | `item_etc` 26 |
| Henesys | 헤네시스 | town (area 2) | `item_etc` 27 |
| Ant Tunnel | 개미굴 | dungeon (area 3) | `item_etc` 28 |
| Orbis | 오르비스 | town (area 4) | `item_etc` 29 |
| Magatia | 마가티아 | town (area 6) | `item_etc` 30 |
| El Nath | 엘나스 | town (area 9) | `item_etc` 31 |
| Abandoned Mine | 폐광 | dungeon | `item_etc` 88 |
| Dead Mine | 폐광 | dungeon | scripts |
| Zenumist | 제뉴미스트 | Magatia society | `npc` 33 |
| Alcadno | 알카드노 | Magatia society | `npc` 32 |
| Meso | 메소 | currency | `item_etc` 0 to 2 |
| Malady | 멜러디 | boss | `mob` 64 |
| Malady Bomb | 멜러디폭탄 | Malady's bomb | `mob` 65 |
| Balrog | 발록 | boss | `mob` 69 |
| Homunscullo | 호문스쿨러 | monster, story | `mob` 67 |
| Werewolf | 웨어울프 | monster | `mob` 66 |
| Evil Lord | 마왕 | story | scripts |
| Dark Lord | 다크로드 | NPC, thief instructor | `npc` 3 |
| MacArthur | 맥아더 | NPC, Robin's master | `npc` 14 |
| Manji | 만지 | NPC | `npc` 10 |
| Chief Stan | 장로스탄 | NPC | `npc` 8 |
| Mr. Lee | 무기상인 | NPC, weapon merchant | `npc` 1 |
| Mr. Wang | 소비상인 | NPC, potion merchant | `npc` 2 |
| Mr. Smith | 스미스 | NPC | `npc` 12 |
| JM | 제이엠 | NPC | `npc` 4 |
| Alex | 알렉스 | NPC | `npc` 5 |
| Ayan | 이얀 | NPC | `npc` 13 |
| Alcaster | 알케스터 | NPC | `npc` 28 |
| Snow Queen | 눈의 여왕 | NPC | `npc` 17 |
| Broker Han | 브로커한 | NPC | `npc` 31 |
| Speigemans | 슈피겔만 | NPC | `npc` 38 |
| Humanoid | 휴머노이드 | NPC | `npc` 34 |
| Red Potion | 빨간포션 | potion | `item_etc` 3 |
| Blue Potion | 파란포션 | potion | `item_etc` 6 |
| Elixir | 엘릭서 | potion | `item_etc` 9 |
| Subi Throwing-Stars | 수비표창 | throwing star | `item_etc` 11 |
| Return Scroll | 귀환주문서 | scroll type | `item_etc` 24 to 31 |
| Lucky Seven | 럭키세븐 | skill | `skill` 2 |
| Double Step | 더블스텝 | skill | `skill` 3 |
| Dark Sight | 다크사이트 | skill | `skill` 5 |
| Shadow Partner | 쉐도우파트너 | skill | `skill` 14 |
| Monster Carnival | | mini-game | scripts (`initMonCa`) |
| Power Elixir | | potion | `item_etc` 10 |

Notes on the names:

- The table's NPC names carry a `$` where the game breaks the line (`Snow $Queen`); the compendium drops it.
- Some Korean names are roles, not names: Mr. Lee is "weapon merchant" (무기상인) and Mr. Wang "consumables merchant" (소비상인).
- Two English names map to 폐광: the return scroll says Abandoned Mine, the scripts say Dead Mine. Pages use the name of the context they describe.
- The tables have a second Yeti (`mob` 68, level 121) beside the field Yeti (`mob` 49); pages tell them apart by level.
- Other abbreviations used on pages: EXP, HP, MP, ATT, DEF, ACC, AVOID, NPC, Lv.

## Spoilers

- **Marked:** story events of chapter 2 onwards, who is behind the story's events, boss identities before the boss is met, and the ending.
- **Not marked:** monster, item, skill and map data, shop stock, quest objectives and rewards, chapter 1's opening, and anything the title screen or App Store page shows.
- **How:** a spoiler is a block, never a sentence fragment:

  ```markdown
  <details class="spoiler"><summary>Story</summary>

  The text, in the writer's own words.

  </details>
  ```

- A chapter page puts its whole story in one spoiler block. Entity pages keep spoilers to a Story or Trivia section.
- Headings, leads and infoboxes never contain spoilers, since search results and link previews show them.

## Evidence

- Facts come from the data tables, the scripts and the port's tests. A page states read facts plainly.
- **Inferred facts** (a column whose meaning was worked out, not read from the game) carry a "probable" note with a link to the spec field, as the data-table specs mark these: "Level 12 (probable; [spec](../formats/data-tables.md#mobdat), `mob.level`)".
- Facts from a playthrough name the test or the date: "confirmed in play (`play.chapter_1_a`)".
- Unknown values are written "unknown", never guessed. A column the specs list as unknown is left out of the infobox.
- Unused content (a map no portal reaches, an item no table drops) goes in an Unused content section with the evidence for "unused".
