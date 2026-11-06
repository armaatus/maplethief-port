"""KB stage: the language data tables (docs/formats/data-tables.md, docs/compendium/kb.md).

Kinds: ``monster``, ``item`` (``equip:<id>`` and ``etc:<id>``, one id space), ``npc``,
``shop``, ``listing``, ``skill``, ``level``. English and Korean share every numeric column
(checked by tests/test_datatables_real.py), so numbers come from English and each text field
is ``{"en": ..., "ko": ...}``. Japanese is a copy of English and is not read.

Confidence follows the table spec's *Evidence* column: ``code`` where the use was seen in
the code, ``data`` where the meaning was inferred from the shipped values, ``probable`` for
the spec's "probably", ``unknown`` for the columns nothing reads.
"""

from __future__ import annotations

from typing import Any

from mtport.compendium.kb import KB, Sources, names, relation

# Relations of this stage (both directions are stored).
relation("monster", "drops", "item", "dropped_by")
relation("monster", "meso_drops", "item", "meso_dropped_by")
relation("shop", "sells", "item", "sold_by")
relation("shop", "listings", "listing", "shop")
relation("listing", "item", "item", "listings")

UNUSED_MARKET_ROWS = range(202, 220)  # all zero, never matched by a shop (shop ids are 0-16)
SLIP_ROW = 146  # shop 0 in the middle of shop 8's rows (docs/formats/data-tables.md)

# Drop odds (0x5C8ED, 0x5C699; Rand_Chance(n, d) = Rand_Next() % d + 1 <= n, 0x22685).
EQUIP_PER = 100_000  # equip: chance + hero level out of 100,000
ETC_PER = 1_000  # etc slots 1-3: chance + hero level / 10 out of 1,000
# Meso (0x5C3C1): amount uniform in [meso * 8 / 10, meso * 12 / 10], dropped at 80 %, as
# item_etc 0 for 1-99, 1 for 100-199, 2 for 200 and up.
MESO_PERCENT = 80
MESO_ITEMS = ((1, 99, 0), (100, 199, 1), (200, None, 2))

ITEM_ETC_TYPES = {
    0: "meso",
    1: "potion",
    2: "throwing star",
    3: "scroll",
    4: "return scroll",
    5: "etc",
    6: "quest item",
    7: "special item",
    8: "100% scroll",
}
EQUIP_SLOTS = {0: "hat", 1: "weapon", 2: "overall", 3: "earrings", 4: "shoes"}
WEAPON_TYPES = {0: None, 1: "claw", 2: "dagger"}
SKILL_BRANCHES = {0: "Rogue", 1: "Assassin", 2: "Bandit"}
SKILL_TYPES = {0: "basic attack", 1: "passive", 2: "buff", 3: "active attack"}
MOB_CLASSES = {1: "normal", 2: "boss", 3: "bomb"}  # mob.unknown_24 (mob +0xAC)
BOSS_BAR_FIRST_ID = 0x40  # HUD_Paint draws the top HP bar for mob ids 0x40 and up
EQUIP_STATS = ("attack", "defense", "accuracy", "avoid", "hp", "mp")  # bonus_stat 1..6
PLACEHOLDERS = ("dmg", "time", "etc", "mp")  # skill.detail's @dmg, @time, @etc, @mp


def clean_name(text: str) -> str:
    """A display name: ``$`` marks a line break in the game; here it is a space."""
    return " ".join(text.replace("$", " ").split())


def clean_text(text: str) -> str:
    """A description: ``$`` (and a literal backslash-n) are line breaks."""
    lines = text.replace("\\n", "$").split("$")
    return "\n".join(line.strip() for line in lines).strip()


def _bilingual(en: str, ko: str, clean=clean_text) -> dict[str, str]:
    return {"en": clean(en), "ko": clean(ko)}


RAND_SPAN = 32768  # Rand_Next is 15-bit (docs/re/mechanics.md, "The 15-bit quirk")


def percent(chance: int, per: int) -> float:
    """The table's face value: ``chance`` in ``per``."""
    return round(chance * 100 / per, 4)


def effective_percent(chance: int, per: int) -> float:
    """The odds ``Rand_Chance(chance, per)`` really gives (0x22685): true when
    ``Rand_Next % per < chance`` over the 32768 values of ``Rand_Next``, so a ``per`` of
    100000 is really ``chance`` in 32768. The hero-level bonus is not included."""
    n = max(0, min(chance, per))
    hits = (RAND_SPAN // per) * n + min(n, RAND_SPAN % per)
    return round(hits * 100 / RAND_SPAN, 4)


def fill_detail(text: str, values: dict[str, int]) -> str:
    """``detail`` with ``@dmg``, ``@time``, ``@etc`` and ``@mp`` replaced by ``values``."""
    for key in sorted(PLACEHOLDERS, key=len, reverse=True):
        text = text.replace(f"@{key}", str(values[key]))
    return text


def skill_value(base: int, per_level: int, level: int) -> int:
    """``base + (level - 1) * per_level`` (MP cost 0x7D251, the others 0x7D325, 0x7D551,
    0x7EC3D)."""
    return base + (level - 1) * per_level


def etc_value(raw: int) -> int:
    """The skill's ``@etc`` as the game uses and shows it (``CSkill::EtcValue`` ``0x7D325``):
    the column is in tenths, so a positive sum is divided by 10 (rounding down)."""
    return raw // 10 if raw > 0 else raw


def meso_drop(meso: int) -> dict[str, Any] | None:
    if meso <= 0:
        return None
    return {"amount": meso, "min": meso * 8 // 10, "max": meso * 12 // 10, "percent": MESO_PERCENT}


def meso_items(lo: int, hi: int) -> list[int]:
    """The ``item_etc`` ids a meso drop of ``lo..hi`` can turn into."""
    out = []
    for first, last, item in MESO_ITEMS:
        if hi >= first and (last is None or lo <= last) and hi >= 1:
            out.append(item)
    return out


def item_id(table: int | str, ident: int) -> str:
    """``equip:<id>`` for item_equip (market table 0), ``etc:<id>`` otherwise."""
    return f"{'equip' if table in (0, 'equip') else 'etc'}:{ident}"


def stage(kb: KB, src: Sources) -> None:
    en, ko = src.tables["en"], src.tables["ko"]
    _items(kb, en, ko)
    _monsters(kb, en, ko)
    _npcs(kb, en, ko)
    _shops(kb, en)
    _skills(kb, en, ko)
    _levels(kb, en)


def _items(kb: KB, en: dict, ko: dict) -> None:
    for e, k in zip(en["item_equip"].records, ko["item_equip"].records, strict=True):
        stats = {
            s: {"min": getattr(e, f"{s}_min"), "max": getattr(e, f"{s}_max")} for s in EQUIP_STATS
        }
        bonus = None
        if e.bonus_stat:
            bonus = {"stat": EQUIP_STATS[e.bonus_stat - 1], "value": e.bonus_value}
        kb.add(
            "item",
            item_id(0, e.id),
            _bilingual(e.name, k.name, clean_name),
            {
                "table": ("equip", "code"),
                "table_id": (e.id, "code"),
                "category": (EQUIP_SLOTS.get(e.slot, f"slot {e.slot}"), "code"),
                "slot": (e.slot, "code"),
                "weapon_type": (WEAPON_TYPES.get(e.weapon_type), "code"),
                "job": (e.job, "data"),
                "req_level": (e.req_level, "data"),
                "speed": (e.speed, "unknown"),
                "stats": (stats, "code"),
                "bonus": (bonus, "code"),
                "price": (e.price, "code"),
                "icon": (e.icon, "code"),
                "look": (e.look, "code"),
                "description": ({"en": None, "ko": None}, "code"),
            },
            name_confidence="code",
        )
    for e, k in zip(en["item_etc"].records, ko["item_etc"].records, strict=True):
        kb.add(
            "item",
            item_id(1, e.id),
            _bilingual(e.name, k.name, clean_name),
            {
                "table": ("etc", "code"),
                "table_id": (e.id, "code"),
                "category": (ITEM_ETC_TYPES.get(e.type, f"type {e.type}"), "data"),
                "type": (e.type, "code"),
                "hp": (e.hp, "data"),
                "mp": (e.mp, "data"),
                "attack": (e.attack, "data"),
                "defense": (e.defense, "data"),
                "accuracy": (e.accuracy, "data"),
                "avoidability": (e.avoidability, "data"),
                "max_stack": (e.max_stack, "code"),
                "price": (e.price, "code"),
                "unknown_84": (e.unknown_84, "unknown"),
                "icon": (e.icon, "code"),
                "description": (_bilingual(e.description, k.description), "code"),
            },
            name_confidence="code",
        )


def _monsters(kb: KB, en: dict, ko: dict) -> None:
    for m, k in zip(en["mob"].records, ko["mob"].records, strict=True):
        ident = str(m.id)
        meso = meso_drop(m.meso)
        kb.add(
            "monster",
            ident,
            _bilingual(m.name, k.name, clean_name),
            {
                "name_fills_field": (len(m.name.encode("cp949")) >= 16, "code"),
                "hp": (m.hp, "code"),
                "level": (m.level, "code"),
                "exp": (m.exp, "code"),
                "meso": (meso, "code"),
                "class": (MOB_CLASSES.get(m.unknown_24, f"class {m.unknown_24}"), "code"),
                "walk_speed": (m.unknown_23, "code"),
                "movement_kind": (m.unknown_24, "code"),
                "sprite": (m.sprite, "code"),
                "boss_bar": (m.id >= BOSS_BAR_FIRST_ID, "code"),
            },
            name_confidence="code",
        )
        slots = [("equip", 0, m.equip_drop, m.equip_drop_chance, EQUIP_PER)]
        for n in (1, 2, 3):
            slots.append(
                ("etc", n, getattr(m, f"etc_drop_{n}"), getattr(m, f"etc_drop_{n}_chance"), ETC_PER)
            )
        for table, slot, item, chance, per in slots:
            if chance <= 0:  # the roll is skipped for a chance below 1
                continue
            kb.link(
                "monster",
                ident,
                "drops",
                "item",
                item_id(table, item),
                slot=slot,
                chance=chance,
                per=per,
                percent=effective_percent(chance, per),
                nominal_percent=percent(chance, per),
            )
        if meso is not None:
            for item in meso_items(meso["min"], meso["max"]):
                kb.link(
                    "monster",
                    ident,
                    "meso_drops",
                    "item",
                    item_id(1, item),
                    min=meso["min"],
                    max=meso["max"],
                    percent=MESO_PERCENT,
                )


def _npcs(kb: KB, en: dict, ko: dict) -> None:
    for n, k in zip(en["npc"].records, ko["npc"].records, strict=True):
        kb.add(
            "npc",
            str(n.id),
            _bilingual(n.name, k.name, clean_name),
            {
                "sprite": (n.id + 4, "code"),  # object kind = sprite/mob/<id + 4>.sprprj
            },
            name_confidence="code",
        )


def _shops(kb: KB, en: dict) -> None:
    rows = en["market"].records
    shops = sorted({r.shop for i, r in enumerate(rows) if i not in UNUSED_MARKET_ROWS})
    for s in shops:
        kb.add("shop", str(s), names(f"Shop {s}", f"상점 {s}"), {})
    items = kb.entities.get("item", {})
    for row, r in enumerate(rows):
        if row in UNUSED_MARKET_ROWS:
            continue
        target = item_id(r.table, r.item)
        item = items.get(target)
        label = item["name"] if item else names(f"Listing {row}", f"목록 {row}")
        note = None
        if row == SLIP_ROW:
            note = (
                "shop 0 in the middle of shop 8's rows; the game shows it in shop 0, "
                "but it probably belongs to shop 8"
            )
        kb.add(
            "listing",
            str(row),
            label,
            {
                "row": (row, "code"),
                "price": (r.price, "code"),
                "note": (note, "probable" if note else "data"),
            },
        )
        kb.link("listing", str(row), "item", "item", target, price=r.price)
        kb.link("shop", str(r.shop), "listings", "listing", str(row), price=r.price)
        kb.link("shop", str(r.shop), "sells", "item", target, price=r.price, listing=str(row))


def _skills(kb: KB, en: dict, ko: dict) -> None:
    for s, k in zip(en["skill"].records, ko["skill"].records, strict=True):
        levels = []
        for lv in range(1, s.max_level + 1):
            values = {
                "dmg": skill_value(s.damage_base, s.damage_per_level, lv),
                "time": skill_value(s.time_base, s.time_per_level, lv),
                "etc": skill_value(s.etc_base, s.etc_per_level, lv),
                "mp": skill_value(s.mp_base, s.mp_per_level, lv),
            }
            shown = {**values, "etc": etc_value(values["etc"])}
            levels.append(
                {
                    "level": lv,
                    "damage": values["dmg"],
                    "time": values["time"],
                    "etc": values["etc"],
                    "etc_value": shown["etc"],
                    "mp": values["mp"],
                    "detail": {
                        "en": fill_detail(clean_text(s.detail), shown),
                        "ko": fill_detail(clean_text(k.detail), shown),
                    },
                }
            )
        kb.add(
            "skill",
            str(s.id),
            _bilingual(s.name, k.name, clean_name),
            {
                "icon": (s.icon, "code"),
                "branch": (SKILL_BRANCHES.get(s.branch, f"branch {s.branch}"), "code"),
                "tier": (s.tier, "code"),
                "type": (SKILL_TYPES.get(s.type, f"type {s.type}"), "code"),
                "max_level": (s.max_level, "code"),
                "learnable": (s.max_level > 0, "code"),
                "base": (
                    {
                        "damage": s.damage_base,
                        "time": s.time_base,
                        "etc": s.etc_base,
                        "mp": s.mp_base,
                    },
                    "code",
                ),
                "per_level": (
                    {
                        "damage": s.damage_per_level,
                        "time": s.time_per_level,
                        "etc": s.etc_per_level,
                        "mp": s.mp_per_level,
                    },
                    "code",
                ),
                "description": (_bilingual(s.description, k.description), "code"),
                "detail": (_bilingual(s.detail, k.detail), "code"),
                "levels": (levels, "code"),
            },
            name_confidence="code",
        )


def _levels(kb: KB, en: dict) -> None:
    exp_total = hp = mp = 0
    for h in en["hero"].records:
        hp += h.hp_gain
        mp += h.mp_gain
        kb.add(
            "level",
            str(h.level),
            names(f"Level {h.level}", f"레벨 {h.level}"),
            {
                "level": (h.level, "code"),
                "exp": (h.exp, "code"),
                "exp_total": (exp_total, "code"),
                "hp_gain": (h.hp_gain, "probable"),  # summed by code; HP vs MP order inferred
                "mp_gain": (h.mp_gain, "probable"),
                "max_hp": (hp, "probable"),
                "max_mp": (mp, "probable"),
                "unknown_9": (h.unknown_9, "unknown"),
                "stats": (
                    {
                        "strength": h.strength,
                        "dexterity": h.dexterity,
                        "intelligence": h.intelligence,
                        "luck": h.luck,
                    },
                    "probable",
                ),
                "attack": (h.attack, "probable"),
                "defense": (h.defense, "probable"),
                "accuracy": (h.accuracy, "probable"),
                "avoidability": (h.avoidability, "probable"),
            },
        )
        exp_total += h.exp
