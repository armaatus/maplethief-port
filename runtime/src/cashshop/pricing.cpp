// The cash shop's products, prices and counters (#156, ADR 0072); see maplethief/cashshop.h.
// Pure functions: no guest, no files.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "maplethief/cashshop.h"

namespace {

using u32 = std::uint32_t;
using i64 = std::int64_t;

struct Product {
    const char *suffix;
    const char *title;
};
constexpr Product k_products[MT_CASH_PRODUCT_COUNT] = {
    {"InitializeSP", "Initialize SP"}, {"SPMarbleSP1Plus", "SP Marble SP+1"},
    {"mesopack3", "22,000 meso"},      {"mesopack4", "50,000 meso"},
    {"mesopack5", "120,000 meso"},     {"mesopack6", "300,000 meso"},
    {"exppack1", "EXP Explosion 40%"}, {"exppack2", "EXP Explosion 80%"},
    {"powpackage", "POW! Package"},    {"ComebackPack", "Comeback Pack"},
};

// The defaults (ADR 0072, "Pricing"). meso_per_exp: the shipped mobs drop a median 2.05 meso
// per EXP they give (mob.dat, tools/mtport/cashprice.py) with an 80 % chance, 1.64. The shares
// put ADR 0072's anchors at level 59, whose income is nearest a POW! Package's 39,000 meso (30
// Power Elixirs at the NPC shops' 1,300): POW! 39,460, Comeback Pack 78,920 (the anchor
// 79,000), and the items at the same shares of a level. The resale floors are item_etc's
// prices (the item's +0x9C, what a shop pays for it): Initialize SP 10,000, SP Marble 5,000, a
// Power Elixir 300.
constexpr mt_cash_knobs k_defaults = {
    1640,
    {25, 125, 0, 0, 0, 0, 100, 200, 100, 200},
    {10000, 5000, 0, 0, 0, 0, 0, 0, 9000, 9000},
    150,
    4,
    10,
    100,
};

bool parse_uint(const char *s, const char *end, u32 *out) {
    u32 v = 0;
    bool any = false;
    for (; s < end && *s >= '0' && *s <= '9'; s++) {
        if (v > 100000000u) {
            return false;
        }
        v = v * 10 + static_cast<u32>(*s - '0');
        any = true;
    }
    if (!any || s != end) {
        return false;
    }
    *out = v;
    return true;
}

bool blank(char c) { return c == ' ' || c == '\t' || c == '\r'; }

} // namespace

extern "C" {

const char *mt_cash_product_suffix(uint32_t product) noexcept {
    return product < MT_CASH_PRODUCT_COUNT ? k_products[product].suffix : nullptr;
}

const char *mt_cash_product_title(uint32_t product) noexcept {
    return product < MT_CASH_PRODUCT_COUNT ? k_products[product].title : nullptr;
}

int32_t mt_cash_product_from_id(const char *id) noexcept {
    if (id == nullptr) {
        return -1;
    }
    const std::size_t n = std::strlen(MT_CASH_PRODUCT_PREFIX);
    if (std::strncmp(id, MT_CASH_PRODUCT_PREFIX, n) == 0) {
        id += n;
    }
    for (u32 p = 0; p < MT_CASH_PRODUCT_COUNT; p++) {
        if (std::strcmp(id, k_products[p].suffix) == 0) {
            return static_cast<int32_t>(p);
        }
    }
    return -1;
}

int mt_cash_product_is_revive(uint32_t product) noexcept {
    return product == MT_CASH_POW || product == MT_CASH_COMEBACK ? 1 : 0;
}

const mt_cash_knobs *mt_cash_default_knobs(void) noexcept { return &k_defaults; }

int64_t mt_cash_level_income(const mt_cash_knobs *knobs, int32_t exp_to_level) noexcept {
    const mt_cash_knobs *k = knobs != nullptr ? knobs : &k_defaults;
    return static_cast<i64>(std::max<int32_t>(exp_to_level, 0)) * k->meso_per_exp_milli / 1000;
}

int64_t mt_cash_price(const mt_cash_knobs *knobs, uint32_t product, int32_t exp_to_level,
                      int64_t meso, uint32_t steps) noexcept {
    const mt_cash_knobs *k = knobs != nullptr ? knobs : &k_defaults;
    if (product >= MT_CASH_PRODUCT_COUNT || k->share_pct[product] == 0) {
        return -1;
    }
    // In thousandths of a meso, so that the steps' rounding stays small.
    i64 milli = static_cast<i64>(std::max<int32_t>(exp_to_level, 0)) * k->meso_per_exp_milli *
                k->share_pct[product] / 100;
    const u32 n = std::min(steps, k->max_steps);
    for (u32 i = 0; i < n; i++) {
        milli = milli * k->step_pct / 100;
    }
    i64 price = (milli + 999) / 1000;
    price = std::max<i64>(price, k->resale[product]);
    price = std::max<i64>(price, std::max<i64>(meso, 0) * k->wealth_floor_pct / 100);
    price = std::max<i64>(price, k->min_price);
    return (price + 9) / 10 * 10;
}

void mt_cash_counters_reset(mt_cash_counters *c, int32_t level) noexcept {
    if (c != nullptr) {
        *c = mt_cash_counters{};
        c->level = level;
    }
}

uint32_t mt_cash_steps(const mt_cash_counters *c, uint32_t product) noexcept {
    if (c == nullptr || product >= MT_CASH_PRODUCT_COUNT) {
        return 0;
    }
    if (mt_cash_product_is_revive(product) != 0) {
        return (c->deaths > 0 ? c->deaths - 1 : 0) + c->paid_revives;
    }
    return c->bought[product];
}

void mt_cash_counters_death(mt_cash_counters *c) noexcept {
    if (c != nullptr) {
        c->deaths++;
    }
}

void mt_cash_counters_bought(mt_cash_counters *c, uint32_t product) noexcept {
    if (c == nullptr || product >= MT_CASH_PRODUCT_COUNT) {
        return;
    }
    c->bought[product]++;
    if (mt_cash_product_is_revive(product) != 0) {
        c->paid_revives++;
    }
}

size_t mt_cash_counters_format(const mt_cash_counters *c, char *out, size_t size) noexcept {
    if (c == nullptr || out == nullptr || size == 0) {
        return 0;
    }
    std::size_t at = 0;
    const auto put = [&](const char *fmt, const char *key, long long v) {
        if (at < size) {
            const int n = std::snprintf(out + at, size - at, fmt, key, v);
            at += n > 0 ? static_cast<std::size_t>(n) : 0;
        }
    };
    if (size > 0) {
        const int n = std::snprintf(out, size,
                                    "# MapleThief: the cash shop's counters for this save slot "
                                    "(ADR 0072). The port's file, not the game's.\n");
        at = n > 0 ? static_cast<std::size_t>(n) : 0;
    }
    put("%s = %lld\n", "level", c->level);
    put("%s = %lld\n", "deaths", c->deaths);
    put("%s = %lld\n", "paid_revives", c->paid_revives);
    for (u32 p = 0; p < MT_CASH_PRODUCT_COUNT; p++) {
        if (k_defaults.share_pct[p] != 0) {
            put("%s = %lld\n", k_products[p].suffix, c->bought[p]);
        }
    }
    return std::min(at, size - 1);
}

int mt_cash_counters_parse(const char *text, size_t size, mt_cash_counters *out) noexcept {
    if (out == nullptr) {
        return -1;
    }
    mt_cash_counters_reset(out, 0);
    if (text == nullptr) {
        return -1;
    }
    bool any = false;
    const char *end = text + size;
    for (const char *line = text; line < end;) {
        const char *eol = static_cast<const char *>(std::memchr(line, '\n', end - line));
        const char *next = eol != nullptr ? eol + 1 : end;
        if (eol == nullptr) {
            eol = end;
        }
        const char *a = line;
        while (a < eol && blank(*a)) {
            a++;
        }
        const char *eq = static_cast<const char *>(std::memchr(a, '=', eol - a));
        if (a < eol && *a != '#' && eq != nullptr) {
            const char *ke = eq;
            while (ke > a && blank(ke[-1])) {
                ke--;
            }
            const char *v = eq + 1;
            while (v < eol && blank(*v)) {
                v++;
            }
            const char *ve = eol;
            while (ve > v && blank(ve[-1])) {
                ve--;
            }
            const std::string_view key(a, static_cast<std::size_t>(ke - a));
            u32 value = 0;
            if (parse_uint(v, ve, &value)) {
                if (key == "level") {
                    out->level = static_cast<int32_t>(value);
                    any = true;
                } else if (key == "deaths") {
                    out->deaths = value;
                    any = true;
                } else if (key == "paid_revives") {
                    out->paid_revives = value;
                    any = true;
                } else {
                    for (u32 p = 0; p < MT_CASH_PRODUCT_COUNT; p++) {
                        if (key == k_products[p].suffix) {
                            out->bought[p] = value;
                            any = true;
                        }
                    }
                }
            }
        }
        line = next;
    }
    return any ? 0 : -1;
}

} // extern "C"
