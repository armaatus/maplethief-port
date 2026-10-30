/* The offline cash shop and paid revives, earned with meso (#156, ADR 0072,
 * docs/re/cash-shop.md). The original sold 10 products through In-App Purchase and granted them
 * only when Nexon's server confirmed the receipt. With `cash_shop = meso` the port sells the
 * six that are not meso for the hero's own meso, at a price that rises with the hero's level
 * and with how often the player died or bought lately:
 *
 *   the store    -[SKPaymentQueue addPayment:] asks the cash shop (mt_storekit_set_policy,
 *                maplethief/stubs.h). It quotes the price, pays it through the game's own
 *                CHero::setMeso(hero, -price, 1) ("Not enough Mesos." when the hero can't, and
 *                the payment is cancelled), and the transaction completes with a synthetic
 *                receipt "maplethief-offline 1 <product id>".
 *   the server   the CFSocket stubs (runtime/src/stubs/network.cpp) answer CMSocket's receipt
 *                check for synthetic receipts, so the game's own receipt lists, grant, save and
 *                popups run unchanged.
 *   the counters per save slot, in Documents/maplethief-cashshop<slot> (the port's file, never
 *                theif<slot>): deaths and paid revives since the last level-up, and the
 *                purchases of each item at the current level. A level-up clears them; a new
 *                character in the slot starts them afresh. Seen through the observed functions
 *                (maplethief/events.h, ADR 0053).
 *   the quote    what the port's price line shows while the cash shop or the death popup is
 *                open (mt_cash_shop_view), refreshed every run-loop turn.
 *
 * The price (mt_cash_price, a pure function):
 *
 *   income  = the meso a hero earns over its current level: meso_per_exp (the shipped mobs'
 *             meso drop per EXP, times the drop chance) x the EXP the level needs
 *             (CHero::ExpToLevelUp, hero.dat). Every product costs the same share of a level's
 *             progress at level 10 and at level 70.
 *   base    = income x the product's share (percent of a level's income)
 *   price   = base x step_pct^min(steps, max_steps), then at least what the product's items
 *             sell back for (item_etc.price: a purchase is never resold at a profit), at least
 *             wealth_floor_pct of the hero's meso and at least min_price, rounded up to 10 meso.
 *   steps   a revive: deaths since the last level-up - 1 + paid revives since it (the first
 *           fall at a level is the base price); another item: its purchases at this level.
 *
 * Off (the default, every preset): no policy, so every payment is cancelled as under ADR 0034,
 * and the server still answers restores of synthetic receipts that a session with it on left
 * behind (never a real App Store receipt).
 *
 * Single guest thread. C17 that also compiles as C++20; the implementation is
 * runtime/src/cashshop/. */
#ifndef MAPLETHIEF_CASHSHOP_H
#define MAPLETHIEF_CASHSHOP_H

#include <stddef.h>
#include <stdint.h>

#include "maplethief/recomp/recomp.h"

MT_EXTERN_C_BEGIN

/* --- the products --------------------------------------------------------------------------- */

/* The 10 products (docs/re/cash-shop.md), com.nexonmobile.maplestory.thief.<suffix>. */
typedef enum mt_cash_product {
    MT_CASH_INITIALIZE_SP = 0, /* InitializeSP: tile -27 */
    MT_CASH_SP_MARBLE,         /* SPMarbleSP1Plus: tile -28 */
    MT_CASH_MESOPACK3,         /* mesopack3: +22,000 meso, tile -29 (never sold) */
    MT_CASH_MESOPACK4,         /* mesopack4: +50,000 (never sold) */
    MT_CASH_MESOPACK5,         /* mesopack5: +120,000 (never sold) */
    MT_CASH_MESOPACK6,         /* mesopack6: +300,000 (never sold) */
    MT_CASH_EXP40,             /* exppack1: +40 % of the level's EXP, tile -33 */
    MT_CASH_EXP80,             /* exppack2: +80 %, tile -34 */
    MT_CASH_POW,               /* powpackage: revive + 30 Power Elixirs, death popup -25 */
    MT_CASH_COMEBACK,          /* ComebackPack: the same + 40 % EXP, death popup -35 */
    MT_CASH_PRODUCT_COUNT
} mt_cash_product;

#define MT_CASH_PRODUCT_PREFIX "com.nexonmobile.maplestory.thief."

/* "InitializeSP", ... ; NULL past the last. */
const char *mt_cash_product_suffix(uint32_t product) MT_NOEXCEPT;
/* The product of a full id (or of its suffix alone); -1 if none. */
int32_t mt_cash_product_from_id(const char *id) MT_NOEXCEPT;
/* 1 for the two revives. */
int mt_cash_product_is_revive(uint32_t product) MT_NOEXCEPT;
/* The product's name as the price line shows it ("POW! Package"). */
const char *mt_cash_product_title(uint32_t product) MT_NOEXCEPT;

/* --- the price ------------------------------------------------------------------------------ */

/* The pricing's knobs (ADR 0072: tuned in a playthrough). */
typedef struct mt_cash_knobs {
    uint32_t meso_per_exp_milli;               /* a level's meso income per EXP, x1000 */
    uint32_t share_pct[MT_CASH_PRODUCT_COUNT]; /* of a level's income; 0: not sold */
    uint32_t resale[MT_CASH_PRODUCT_COUNT];    /* what its items sell back for, in meso */
    uint32_t step_pct;                         /* each escalation step, percent */
    uint32_t max_steps;                        /* the escalation's cap, in steps */
    uint32_t wealth_floor_pct;                 /* never below this share of the hero's meso */
    uint32_t min_price;                        /* never below this, in meso */
} mt_cash_knobs;

/* The defaults: meso_per_exp 1.64; shares InitializeSP 25, SP Marble 125, EXP 40 % 100, EXP 80 %
 * 200, POW! 100, Comeback 200, the meso packs 0; resale InitializeSP 10,000, SP Marble 5,000, the
 * revives 9,000 (30 Power Elixirs at 300), the EXP packs 0; step 150 %, 4 steps, floor 10 %, min
 * 100. */
const mt_cash_knobs *mt_cash_default_knobs(void) MT_NOEXCEPT;

/* The meso a hero earns over a level that needs `exp_to_level` EXP. */
int64_t mt_cash_level_income(const mt_cash_knobs *knobs, int32_t exp_to_level) MT_NOEXCEPT;

/* The price of `product` for a hero whose level needs `exp_to_level` EXP and who holds `meso`,
 * after `steps` escalation steps; -1 if the product is not sold. */
int64_t mt_cash_price(const mt_cash_knobs *knobs, uint32_t product, int32_t exp_to_level,
                      int64_t meso, uint32_t steps) MT_NOEXCEPT;

/* --- the counters (per save slot) ----------------------------------------------------------- */

typedef struct mt_cash_counters {
    int32_t level;                          /* the level they count at; 0 unknown */
    uint32_t deaths;                        /* falls (death popups) since the last level-up */
    uint32_t paid_revives;                  /* revives bought since the last level-up */
    uint32_t bought[MT_CASH_PRODUCT_COUNT]; /* purchases of each product at this level */
} mt_cash_counters;

/* All zero, at `level`. */
void mt_cash_counters_reset(mt_cash_counters *c, int32_t level) MT_NOEXCEPT;
/* The escalation steps of `product` (see the price above). */
uint32_t mt_cash_steps(const mt_cash_counters *c, uint32_t product) MT_NOEXCEPT;
/* A death (the first death popup since the hero was alive), a purchase of `product` (a revive
 * also counts as a paid revive). */
void mt_cash_counters_death(mt_cash_counters *c) MT_NOEXCEPT;
void mt_cash_counters_bought(mt_cash_counters *c, uint32_t product) MT_NOEXCEPT;

/* The counters file's text ("key = value" lines with a comment). Returns the length written
 * (NUL not counted; truncated if `size` is short). */
size_t mt_cash_counters_format(const mt_cash_counters *c, char *out, size_t size) MT_NOEXCEPT;
/* Reads that text over a reset `out`; unknown keys and bad lines are skipped. Returns 0, or -1
 * if nothing was recognised. */
int mt_cash_counters_parse(const char *text, size_t size, mt_cash_counters *out) MT_NOEXCEPT;

/* --- the running cash shop ------------------------------------------------------------------ */

typedef enum mt_cash_mode { MT_CASH_SHOP_OFF = 0, MT_CASH_SHOP_MESO = 1 } mt_cash_mode;

/* Sets the mode. MESO installs the store's policy, the game events listener (the counters) and
 * a run-loop turn source (the quotes); OFF removes the policy, and the listener and the source
 * do nothing while it is off. Call before the guest runs or on the guest thread; again to
 * change the mode. */
void mt_cash_shop_install(uint32_t mode) MT_NOEXCEPT;
/* Removes all of it (tests); the mode goes back to OFF. */
void mt_cash_shop_uninstall(void) MT_NOEXCEPT;
uint32_t mt_cash_shop_mode(void) MT_NOEXCEPT;
/* The knobs in use (the defaults unless set; tests). NULL: the defaults again. */
void mt_cash_shop_set_knobs(const mt_cash_knobs *knobs) MT_NOEXCEPT;

/* The counters file of `slot`: <Documents>/maplethief-cashshop<slot>. Returns 0, or -1. */
int mt_cash_shop_counters_path(int32_t slot, char *out, uint32_t size) MT_NOEXCEPT;

/* What is open and its prices, as of the last turn. */
typedef enum mt_cash_screen {
    MT_CASH_SCREEN_NONE = 0,
    MT_CASH_SCREEN_SHOP = 1,        /* the cash shop (game mode 0xB) */
    MT_CASH_SCREEN_DEATH_POPUP = 2, /* popup 0x68 */
} mt_cash_screen;

typedef struct mt_cash_quote {
    uint32_t product;
    int64_t price; /* -1: not sold */
    uint32_t steps;
} mt_cash_quote;

typedef struct mt_cash_view {
    uint32_t screen; /* mt_cash_screen; NONE also while the mode is OFF */
    int32_t slot, level, meso;
    uint32_t count; /* quotes */
    mt_cash_quote quotes[MT_CASH_PRODUCT_COUNT];
} mt_cash_view;

/* What the run-loop turn source does (tests call it): reads the hero and the open screen of
 * the guest `cpu` runs, refreshes the view; may call CHero::ExpToLevelUp. */
void mt_cash_shop_turn(mt_cpu *cpu) MT_NOEXCEPT;

/* The view as of the last run-loop turn. */
mt_cash_view mt_cash_shop_view(void) MT_NOEXCEPT;
/* The current quote of `product` for the hero now (the last turn's level and meso); -1 if not
 * sold or no hero is known. */
int64_t mt_cash_shop_quote(uint32_t product) MT_NOEXCEPT;
/* The counters of the slot in play (zeros before a character is chosen). */
mt_cash_counters mt_cash_shop_counters(void) MT_NOEXCEPT;

typedef struct mt_cash_stats {
    uint64_t sold;      /* payments completed (meso paid) */
    uint64_t refused;   /* payments cancelled: not sold, off, or not enough meso */
    uint64_t deaths;    /* deaths counted */
    uint64_t resets;    /* counters cleared: level-ups and new characters */
    uint64_t writes;    /* counters files written */
    int64_t last_price; /* the last price paid */
} mt_cash_stats;
mt_cash_stats mt_cash_shop_get_stats(void) MT_NOEXCEPT;

MT_EXTERN_C_END

#endif /* MAPLETHIEF_CASHSHOP_H */
