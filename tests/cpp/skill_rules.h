// The skill rules of docs/re/mechanics.md as numbers (#302): a skill's MP, @etc, @dmg and
// duration from its skill.dat row, the damage of one hit (CHero::RollDamage 0x64DA1) and its
// range, the stars a throw uses and the hits it lands on one mob. Pure functions over the table's
// bytes: the play test (play_real_test skills_*) reads the shipped skill.dat with them, and
// skill_rules_test checks them on synthetic rows and the note's worked examples in CI.
#ifndef MAPLETHIEF_TESTS_SKILL_RULES_H
#define MAPLETHIEF_TESTS_SKILL_RULES_H

#include <cstdint>
#include <map>
#include <vector>

namespace skill_rules {

using i32 = std::int32_t;

// skill.dat (docs/formats/data-tables.md): a 6-byte header (the row count at 2, the stride at 4),
// then rows of 297 bytes: id +0x00, branch +0x13, tier +0x14, type +0x15 (0 basic attack, 1
// passive, 2 buff, 3 attack), max_level +0x16, then the i16 pairs (base, per level) of the
// damage +0x19, the duration +0x1D, @etc +0x21 and the MP cost +0x25.
struct SkillRow {
    i32 id = -1, branch = 0, tier = 0, type = 0, max_level = 0;
    i32 damage_base = 0, damage_per_level = 0, time_base = 0, time_per_level = 0;
    i32 etc_base = 0, etc_per_level = 0, mp_base = 0, mp_per_level = 0;
};
constexpr int k_skill_stride = 297;

inline std::map<i32, SkillRow> parse_skills(const std::vector<std::uint8_t> &b) {
    std::map<i32, SkillRow> out;
    if (b.size() < 6) {
        return out;
    }
    const int n = b[2] | (b[3] << 8), stride = b[4] | (b[5] << 8);
    if (stride != k_skill_stride || b.size() != 6u + static_cast<std::size_t>(n) * stride) {
        return out;
    }
    for (int i = 0; i < n; i++) {
        const std::uint8_t *p = &b[6 + static_cast<std::size_t>(i) * stride];
        const auto i16 = [p](int o) {
            return static_cast<i32>(static_cast<std::int16_t>(p[o] | (p[o + 1] << 8)));
        };
        SkillRow s;
        s.id = p[0];
        s.branch = p[0x13];
        s.tier = p[0x14];
        s.type = p[0x15];
        s.max_level = p[0x16];
        s.damage_base = i16(0x19);
        s.damage_per_level = i16(0x1B);
        s.time_base = i16(0x1D);
        s.time_per_level = i16(0x1F);
        s.etc_base = i16(0x21);
        s.etc_per_level = i16(0x23);
        s.mp_base = i16(0x25);
        s.mp_per_level = i16(0x27);
        out[s.id] = s;
    }
    return out;
}

// The values at skill level L: base + (L - 1) x per level (CSkill::MpCost 0x7D251, ::EtcValue
// 0x7D325, ::DamageValue 0x7D551, ::Duration 0x7EC3D). @etc is kept in tenths: a positive value
// is divided by 10.
inline i32 mp_cost(const SkillRow &s, i32 level) {
    return s.mp_base + (level - 1) * s.mp_per_level;
}
inline i32 etc_value(const SkillRow &s, i32 level) {
    const i32 v = s.etc_base + (level - 1) * s.etc_per_level;
    return v > 0 ? v / 10 : v;
}
inline i32 window_damage(const SkillRow &s, i32 level) {
    return s.damage_base + (level - 1) * s.damage_per_level;
}
inline i32 duration(const SkillRow &s, i32 level) {
    return s.time_base + (level - 1) * s.time_per_level;
}

// The skills whose hits come from stars (the star's ATT is added to A): the basic throw (1),
// Lucky Seven (2), Drain (7) and Avenger (10).
inline bool star_skill(i32 id) { return id == 1 || id == 2 || id == 7 || id == 10; }

// x, the damage of one hit before the roll (steps 2 to 5 of the rule): A the hero's attack, the
// skill at `level` (its row's damage percentage uses the full level, one more than the window),
// `star_attack` the star's ATT for a star skill.
inline i32 hit_x(const SkillRow &s, i32 level, i32 attack, i32 star_attack) {
    const i32 a = attack + (star_skill(s.id) ? star_attack : 0);
    i32 x = a * (a / 30 + 3) / 5;
    const i32 p = s.damage_base + s.damage_per_level * level;
    switch (s.id) {
    case 1:
        x = x - x / 10;
        break;
    case 2:
        x = x + x * p / 100 - 5;
        break;
    case 3:
        x = x + x * p / 100 - 3;
        break;
    case 7:
        x = x + x * p / 100 + 2;
        break;
    case 10:
        x = x + x * p / 100 + 4;
        break;
    case 16:
        x = x + x * p / 100 + 2;
        break;
    case 23:
        x = x + x * p / 100 + 3;
        break;
    default:
        break;
    }
    return x < 0 ? 10 : x;
}

// One hit's normal damage range from x: (x + Rand_Range(-(x / 3), x / 5)) x 110 / 100. A
// critical doubles it.
struct Range {
    i32 lo = 0, hi = 0;
};
inline Range hit_range(i32 x) { return {(x - x / 3) * 110 / 100, (x + x / 5) * 110 / 100}; }

// The stars one use of a star skill takes (CSkill::SpawnAttack 0x7D781): the throw 1, Lucky
// Seven 2, Drain 1, Avenger 3; Shadow Partner doubles the throw and Lucky Seven. 0 for the
// others.
inline i32 stars_used(i32 id, bool partner) {
    switch (id) {
    case 1:
        return partner ? 2 : 1;
    case 2:
        return partner ? 4 : 2;
    case 7:
        return 1;
    case 10:
        return 3;
    default:
        return 0;
    }
}

// The hits one use lands on one mob at most: Double Step and Boomerang Stab 2, Lucky Seven 2 (4
// with Shadow Partner), the throw 1 (2), Savage Blow @etc, the others 1.
inline i32 hits_on_one(const SkillRow &s, i32 level, bool partner) {
    switch (s.id) {
    case 1:
        return partner ? 2 : 1;
    case 2:
        return partner ? 4 : 2;
    case 3:
    case 23:
        return 2;
    case 16:
        return etc_value(s, level);
    default:
        return 1;
    }
}

// Whether a mob's HP loss in one turn is k hits of the skill (1 <= k <= `hits`), each normal or
// critical, from x. A killing blow only needs to stay under the most those hits can do.
inline bool loss_fits(i32 dealt, i32 x, i32 hits, bool killed) {
    const Range r = hit_range(x);
    if (killed) {
        return dealt >= 1 && dealt <= hits * 2 * r.hi;
    }
    for (i32 k = 1; k <= hits; k++) {
        if (dealt >= k * r.lo && dealt <= k * 2 * r.hi) {
            return true;
        }
    }
    return false;
}

} // namespace skill_rules

#endif // MAPLETHIEF_TESTS_SKILL_RULES_H
