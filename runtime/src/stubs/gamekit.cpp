// GameKit, emulated offline (#52, ADR 0029): the host classes GameCenterManager talks to.
//
// The game's Game Center code is Apple's GKTapper sample (docs/re/objc-shell.md#gamecentermanager):
// at boot -[EAGLView initWithCoder:] asks +[GameCenterManager IsGameCenterAvailable] (a
// GKLocalPlayer class and iOS >= 4.1), creates the manager and sends -AuthenticationUser,
// whose completion block sets -[EAGLView setGameCenterLoginOKorError:YES]. The engine
// reports the total-ranking score with -ReportScore:forCategory: from CMapleWorld::changeMap
// and 0x65089. -SubmitAchievement:PercentComplete: (achievements, through
// +loadAchievementsWithCompletionHandler: and -reportAchievementWithCompletionHandler:) is in
// the binary, but only its own load block sends it: the shipped game reports no achievement.
// The emulation implements every GameKit selector the game's code sends all the same.
//
// Offline, the local player is always authenticated (after its authentication request
// completes, as on iOS), and scores and achievements are saved to gamecenter.json in the
// stubs' data directory (maplethief/stubs.h):
//
//     {"format": "maplethief-gamecenter", "version": 1,
//      "achievements": [{"identifier": "...", "percentComplete": 100}],
//      "scores": [{"category": "...", "best": 1234, "last": 1200, "reports": 7}]}
//
// An achievement keeps its highest percentage (Game Center never lowers one); a score
// category keeps its best and last value. Completion handlers are copied (mt_block_copy) and
// called later by mt_stubs_pump with a nil error.
//
// The local Game Center (#154, ADR 0084) adds, still in version 1 (readers ignore keys they
// don't know), each score category's numbers per player, under the alias the frontend sets
// from the chosen save slot:
//
//      "entries": [{"category": "...", "player": "Slot 1", "best": 1234, "last": 1200,
//                   "reports": 7}]
//
// and a leaderboard screen: with a host hook installed, GKLeaderboardViewController stays
// presented until the host closes it (the frontend draws the entries meanwhile).
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if !defined(MT_HLE_CORE_ONLY)
#include "snapshot/snapshot_internal.h"
#endif
#include "stubs/json.h"
#include "stubs/stubs_internal.h"

namespace mt_stubs_impl {
namespace {

constexpr u32 k_text_max = 512;
constexpr const char *k_format = "maplethief-gamecenter";
constexpr std::int64_t k_version = 1;
// A larger gamecenter.json is not read (#413, ADR 0154): the port's own is a few kilobytes.
constexpr std::size_t k_max_file_bytes = 1u << 20;

// --- the file ------------------------------------------------------------------------------

struct Achievement {
    char *identifier;
    double percent;
};

struct Score {
    char *category;
    std::int64_t best, last;
    std::int64_t reports;
};

// A score category's numbers for one player: the local leaderboard's rows (#154).
struct Entry {
    char *category;
    char *player;
    std::int64_t best, last;
    std::int64_t reports;
};

struct Store {
    Achievement *achievements;
    u32 achievement_count;
    Score *scores;
    u32 score_count;
    Entry *entries;
    u32 entry_count;
    bool unreadable; // the file exists but is not ours: it is kept as .bad before a save
};

char *dup(const char *s) {
    const std::size_t n = std::strlen(s) + 1;
    char *p = static_cast<char *>(std::malloc(n));
    if (p != nullptr) {
        std::memcpy(p, s, n);
    }
    return p;
}

void store_free(Store *st) {
    for (u32 i = 0; i < st->achievement_count; i++) {
        std::free(st->achievements[i].identifier);
    }
    for (u32 i = 0; i < st->score_count; i++) {
        std::free(st->scores[i].category);
    }
    for (u32 i = 0; i < st->entry_count; i++) {
        std::free(st->entries[i].category);
        std::free(st->entries[i].player);
    }
    std::free(st->achievements);
    std::free(st->scores);
    std::free(st->entries);
    std::memset(st, 0, sizeof *st);
}

Achievement *find_achievement(Store *st, const char *identifier, bool add) {
    for (u32 i = 0; i < st->achievement_count; i++) {
        if (std::strcmp(st->achievements[i].identifier, identifier) == 0) {
            return &st->achievements[i];
        }
    }
    if (!add) {
        return nullptr;
    }
    char *id = dup(identifier);
    auto *grown = static_cast<Achievement *>(
        std::realloc(st->achievements, sizeof(Achievement) * (st->achievement_count + 1)));
    if (id == nullptr || grown == nullptr) {
        std::free(id);
        if (grown != nullptr) {
            st->achievements = grown;
        }
        return nullptr;
    }
    st->achievements = grown;
    Achievement &a = st->achievements[st->achievement_count++];
    a.identifier = id;
    a.percent = 0.0;
    return &a;
}

Score *find_score(Store *st, const char *category, bool add) {
    for (u32 i = 0; i < st->score_count; i++) {
        if (std::strcmp(st->scores[i].category, category) == 0) {
            return &st->scores[i];
        }
    }
    if (!add) {
        return nullptr;
    }
    char *cat = dup(category);
    auto *grown =
        static_cast<Score *>(std::realloc(st->scores, sizeof(Score) * (st->score_count + 1)));
    if (cat == nullptr || grown == nullptr) {
        std::free(cat);
        if (grown != nullptr) {
            st->scores = grown;
        }
        return nullptr;
    }
    st->scores = grown;
    Score &s = st->scores[st->score_count++];
    s.category = cat;
    s.best = s.last = 0;
    s.reports = 0;
    return &s;
}

Entry *find_entry(Store *st, const char *category, const char *player, bool add) {
    for (u32 i = 0; i < st->entry_count; i++) {
        if (std::strcmp(st->entries[i].category, category) == 0 &&
            std::strcmp(st->entries[i].player, player) == 0) {
            return &st->entries[i];
        }
    }
    if (!add) {
        return nullptr;
    }
    char *cat = dup(category);
    char *who = dup(player);
    auto *grown =
        static_cast<Entry *>(std::realloc(st->entries, sizeof(Entry) * (st->entry_count + 1)));
    if (cat == nullptr || who == nullptr || grown == nullptr) {
        std::free(cat);
        std::free(who);
        if (grown != nullptr) {
            st->entries = grown;
        }
        return nullptr;
    }
    st->entries = grown;
    Entry &e = st->entries[st->entry_count++];
    e.category = cat;
    e.player = who;
    e.best = e.last = 0;
    e.reports = 0;
    return &e;
}

std::int64_t integer_of(const JsonValue *v, std::int64_t fallback) {
    if (v == nullptr || v->type != JsonType::number) {
        return fallback;
    }
    return v->is_integer ? v->integer : static_cast<std::int64_t>(v->number);
}

// Reads gamecenter.json into *st. A missing file is an empty store; an unreadable one is
// logged, read as empty, and kept aside (.bad) by the next save.
void store_load(Store *st) {
    std::memset(st, 0, sizeof *st);
    char path[1100];
    if (mt_stubs_gamecenter_path(path, sizeof path) != 0) {
        return;
    }
    std::size_t size = 0;
    bool too_large = false;
    char *text = read_file(path, &size, k_max_file_bytes, &too_large);
    if (text == nullptr) {
        if (too_large) {
            log("GameKit: %s is larger than %zu bytes; starting empty", path, k_max_file_bytes);
            st->unreadable = true;
        }
        return;
    }
    JsonValue root{};
    const char *error = nullptr;
    const bool parsed = json_parse(text, size, &root, &error);
    std::free(text);
    const JsonValue *format = parsed ? json_get(&root, "format") : nullptr;
    if (!parsed || format == nullptr || format->type != JsonType::string ||
        std::strcmp(format->string, k_format) != 0 ||
        integer_of(json_get(&root, "version"), 0) != k_version) {
        log("GameKit: %s is not a version %" PRId64 " %s file (%s); starting empty", path,
            k_version, k_format, parsed ? "wrong format or version" : error);
        json_free(&root);
        st->unreadable = true;
        return;
    }
    const JsonValue *list = json_get(&root, "achievements");
    for (u32 i = 0; list != nullptr && list->type == JsonType::array && i < list->count; i++) {
        const JsonValue *id = json_get(&list->items[i], "identifier");
        const JsonValue *pct = json_get(&list->items[i], "percentComplete");
        if (id != nullptr && id->type == JsonType::string && pct != nullptr &&
            pct->type == JsonType::number) {
            Achievement *a = find_achievement(st, id->string, true);
            if (a != nullptr && pct->number > a->percent) {
                a->percent = pct->number;
            }
        }
    }
    list = json_get(&root, "scores");
    for (u32 i = 0; list != nullptr && list->type == JsonType::array && i < list->count; i++) {
        const JsonValue *cat = json_get(&list->items[i], "category");
        if (cat != nullptr && cat->type == JsonType::string) {
            Score *s = find_score(st, cat->string, true);
            if (s != nullptr) {
                s->best = integer_of(json_get(&list->items[i], "best"), 0);
                s->last = integer_of(json_get(&list->items[i], "last"), s->best);
                s->reports = integer_of(json_get(&list->items[i], "reports"), 1);
            }
        }
    }
    list = json_get(&root, "entries");
    for (u32 i = 0; list != nullptr && list->type == JsonType::array && i < list->count; i++) {
        const JsonValue *cat = json_get(&list->items[i], "category");
        const JsonValue *who = json_get(&list->items[i], "player");
        if (cat != nullptr && cat->type == JsonType::string && who != nullptr &&
            who->type == JsonType::string) {
            Entry *e = find_entry(st, cat->string, who->string, true);
            if (e != nullptr) {
                e->best = integer_of(json_get(&list->items[i], "best"), 0);
                e->last = integer_of(json_get(&list->items[i], "last"), e->best);
                e->reports = integer_of(json_get(&list->items[i], "reports"), 1);
            }
        }
    }
    json_free(&root);
}

bool store_save(const Store *st) {
    char path[1100];
    if (mt_stubs_gamecenter_path(path, sizeof path) != 0 || path[0] == '\0') {
        log("GameKit: no data directory: nothing is saved");
        return false;
    }
    if (st->unreadable) {
        char bad[1110];
        std::snprintf(bad, sizeof bad, "%s.bad", path);
        std::remove(bad);
        if (std::rename(path, bad) == 0) {
            log("GameKit: kept the unreadable file as %s", bad);
        }
    }
    JsonOut o{};
    json_raw(&o, "{\n  \"format\": ");
    json_string(&o, k_format);
    json_raw(&o, ",\n  \"version\": ");
    json_int(&o, k_version);
    json_raw(&o, ",\n  \"achievements\": [");
    for (u32 i = 0; i < st->achievement_count; i++) {
        json_raw(&o, i == 0 ? "\n    {\"identifier\": " : ",\n    {\"identifier\": ");
        json_string(&o, st->achievements[i].identifier);
        json_raw(&o, ", \"percentComplete\": ");
        json_double(&o, st->achievements[i].percent);
        json_raw(&o, "}");
    }
    json_raw(&o, st->achievement_count != 0 ? "\n  ],\n  \"scores\": [" : "],\n  \"scores\": [");
    for (u32 i = 0; i < st->score_count; i++) {
        const Score &s = st->scores[i];
        json_raw(&o, i == 0 ? "\n    {\"category\": " : ",\n    {\"category\": ");
        json_string(&o, s.category);
        json_raw(&o, ", \"best\": ");
        json_int(&o, s.best);
        json_raw(&o, ", \"last\": ");
        json_int(&o, s.last);
        json_raw(&o, ", \"reports\": ");
        json_int(&o, s.reports);
        json_raw(&o, "}");
    }
    json_raw(&o, st->score_count != 0 ? "\n  ]" : "]");
    // The per-player entries (#154): only once there are some, so that a file without them is
    // written as before.
    if (st->entry_count != 0) {
        json_raw(&o, ",\n  \"entries\": [");
        for (u32 i = 0; i < st->entry_count; i++) {
            const Entry &e = st->entries[i];
            json_raw(&o, i == 0 ? "\n    {\"category\": " : ",\n    {\"category\": ");
            json_string(&o, e.category);
            json_raw(&o, ", \"player\": ");
            json_string(&o, e.player);
            json_raw(&o, ", \"best\": ");
            json_int(&o, e.best);
            json_raw(&o, ", \"last\": ");
            json_int(&o, e.last);
            json_raw(&o, ", \"reports\": ");
            json_int(&o, e.reports);
            json_raw(&o, "}");
        }
        json_raw(&o, "\n  ]");
    }
    json_raw(&o, "\n}\n");
    const bool ok = !o.failed && write_file(path, o.data, o.size);
    json_out_free(&o);
    return ok;
}

// --- per-runtime state ------------------------------------------------------------------------

struct GameKit {
    u32 local_player;     // the GKLocalPlayer singleton (retained for the runtime's lifetime)
    bool authenticated;   // set when the authentication request completes
    u32 open_leaderboard; // the controller the host shows (retained), until it closes it
};
GameKit g_gk;

// The process's: the local player's alias (the leaderboard's rows) and the leaderboard hook.
constexpr const char *k_default_alias = "Player";
char g_alias[MT_GAMEKIT_NAME_MAX];
mt_gamekit_leaderboard_fn g_leaderboard_hook;
void *g_leaderboard_user;

const char *alias() { return g_alias[0] != '\0' ? g_alias : k_default_alias; }

// Host state of the GameKit objects (maplethief/objc/host.h).
struct ScoreState {
    u32 category; // retained NSString
    std::int64_t value;
};
struct AchievementState {
    u32 identifier; // retained NSString
    double percent;
};
struct LeaderboardState {
    u32 category; // retained NSString
    std::int32_t time_scope;
    u32 range_location, range_length;
};
struct LeaderboardControllerState {
    u32 category; // retained NSString
    std::int32_t time_scope;
    u32 delegate; // not retained (an assign property, as in GameKit)
};

void release_score(mt_cpu *cpu, u32, void *state) {
    mt_objc_release(cpu, static_cast<ScoreState *>(state)->category);
}
void release_achievement(mt_cpu *cpu, u32, void *state) {
    mt_objc_release(cpu, static_cast<AchievementState *>(state)->identifier);
}
void release_leaderboard(mt_cpu *cpu, u32, void *state) {
    mt_objc_release(cpu, static_cast<LeaderboardState *>(state)->category);
}
void release_controller(mt_cpu *cpu, u32, void *state) {
    mt_objc_release(cpu, static_cast<LeaderboardControllerState *>(state)->category);
}

const mt_objc_state_type k_score_state = {"GKScore", sizeof(ScoreState), release_score, nullptr};
const mt_objc_state_type k_achievement_state = {"GKAchievement", sizeof(AchievementState),
                                                release_achievement, nullptr};
const mt_objc_state_type k_leaderboard_state = {"GKLeaderboard", sizeof(LeaderboardState),
                                                release_leaderboard, nullptr};
const mt_objc_state_type k_controller_state = {
    "GKLeaderboardViewController", sizeof(LeaderboardControllerState), release_controller, nullptr};

template <typename T> T *state_of(mt_cpu *cpu, const mt_objc_state_type *type) {
    return static_cast<T *>(mt_objc_state(runtime(), mt_objc_self(cpu), type));
}

// Replaces a retained object field with `value`, retained.
void assign_retained(mt_cpu *cpu, u32 *field, u32 value) {
    mt_objc_retain(cpu, value);
    mt_objc_release(cpu, *field);
    *field = value;
}

// --- deliveries ------------------------------------------------------------------------------

// handler(nil): words = {block}
void deliver_nil_error(mt_cpu *cpu, const u32 *w) {
    const u32 args[1] = {0};
    mt_block_invoke(cpu, w[0], args, 1);
    mt_block_release(cpu, w[0]);
}

// handler(nil, nil): words = {block}
void deliver_nil_list(mt_cpu *cpu, const u32 *w) {
    const u32 args[2] = {0, 0};
    mt_block_invoke(cpu, w[0], args, 2);
    mt_block_release(cpu, w[0]);
}

void deliver_authenticated(mt_cpu *cpu, const u32 *w) {
    g_gk.authenticated = true;
    deliver_nil_error(cpu, w);
}

// handler(achievements, nil) with the saved achievements, nil when there are none (as
// GameKit answers for a player who has none).
void deliver_achievements(mt_cpu *cpu, const u32 *w) {
    mt_objc *rt = runtime();
    Store st;
    store_load(&st);
    u32 array = 0;
    if (st.achievement_count != 0) {
        const u32 cls = mt_objc_class_named(rt, "GKAchievement");
        array = mt_objc_send0(cpu, mt_objc_class_named(rt, "NSMutableArray"), "alloc");
        array = mt_objc_send1(cpu, array, "initWithCapacity:", st.achievement_count);
        for (u32 i = 0; i < st.achievement_count && array != 0; i++) {
            const u32 id = new_string(cpu, st.achievements[i].identifier);
            const u32 a =
                mt_objc_send1(cpu, mt_objc_send0(cpu, cls, "alloc"), "initWithIdentifier:", id);
            auto *s =
                a != 0 ? static_cast<AchievementState *>(mt_objc_state(rt, a, &k_achievement_state))
                       : nullptr;
            if (s != nullptr) {
                s->percent = st.achievements[i].percent;
            }
            mt_objc_send1(cpu, array, "addObject:", a);
            mt_objc_release(cpu, a);
        }
    }
    store_free(&st);
    const u32 args[2] = {array, 0};
    mt_block_invoke(cpu, w[0], args, 2);
    mt_block_release(cpu, w[0]);
    mt_objc_release(cpu, array);
}

// [delegate leaderboardViewControllerDidFinish:controller]: words = {controller (retained)}
void deliver_leaderboard_finished(mt_cpu *cpu, const u32 *w) {
    const auto *s = static_cast<const LeaderboardControllerState *>(
        mt_objc_find_state(runtime(), w[0], &k_controller_state));
    if (s != nullptr && s->delegate != 0) {
        mt_objc_send1(cpu, s->delegate, "leaderboardViewControllerDidFinish:", w[0]);
    }
    mt_objc_release(cpu, w[0]);
}

// Copies a completion handler and posts `fn` for it; a nil handler posts nothing.
void post_handler(mt_cpu *cpu, u32 handler, Deliver fn) {
    if (handler == 0) {
        return;
    }
    const u32 block = mt_block_copy(cpu, handler);
    if (!post(fn, &block, 1)) {
        mt_block_release(cpu, block);
    }
}

// --- GKPlayer, GKLocalPlayer -----------------------------------------------------------------

// +[GKPlayer loadPlayersForIdentifiers:withCompletionHandler:]: no other players offline.
void player_load_players(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    mt_arg_u32(&a); // identifiers
    post_handler(cpu, mt_arg_u32(&a), deliver_nil_list);
}

// -[GKPlayer playerID]
void player_id(mt_cpu *cpu) { mt_ret_u32(cpu, new_string(cpu, "G:offline")); }

// +[GKLocalPlayer localPlayer]: one per runtime.
void local_player(mt_cpu *cpu) {
    mt_objc *rt = runtime();
    if (g_gk.local_player == 0) {
        g_gk.local_player = mt_objc_create_instance(rt, mt_objc_self(cpu));
    }
    mt_ret_u32(cpu, g_gk.local_player);
}

// -[GKLocalPlayer isAuthenticated]
void local_player_is_authenticated(mt_cpu *cpu) {
    runtime();
    mt_ret_u32(cpu, g_gk.authenticated ? 1u : 0u);
}

// -[GKLocalPlayer authenticateWithCompletionHandler:]: always succeeds, on the next pump.
void local_player_authenticate(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const u32 handler = mt_arg_u32(&a);
    runtime();
    if (handler == 0) {
        g_gk.authenticated = true;
        return;
    }
    const u32 block = mt_block_copy(cpu, handler);
    if (!post(deliver_authenticated, &block, 1)) {
        mt_block_release(cpu, block);
    }
}

// --- GKScore ----------------------------------------------------------------------------------

// -[GKScore initWithCategory:]
void score_init_with_category(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const u32 category = mt_arg_u32(&a);
    const u32 self = mt_objc_self(cpu); // before any send: retain/release may use r0
    if (auto *s = state_of<ScoreState>(cpu, &k_score_state)) {
        assign_retained(cpu, &s->category, category);
    }
    mt_ret_u32(cpu, self);
}

// -[GKScore setValue:(int64_t)]
void score_set_value(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const std::int64_t value = mt_arg_i64(&a);
    if (auto *s = state_of<ScoreState>(cpu, &k_score_state)) {
        s->value = value;
    }
}

// -[GKScore reportScoreWithCompletionHandler:]: saved now, the handler runs on the next pump.
void score_report(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const u32 handler = mt_arg_u32(&a);
    const u32 self = mt_objc_self(cpu);
    const auto *s =
        static_cast<const ScoreState *>(mt_objc_find_state(runtime(), self, &k_score_state));
    char category[k_text_max];
    string_text(cpu, s != nullptr ? s->category : 0u, category, sizeof category);
    const std::int64_t value = s != nullptr ? s->value : 0;
    Store st;
    store_load(&st);
    Score *score = find_score(&st, category, true);
    if (score != nullptr) {
        score->best = score->reports == 0 || value > score->best ? value : score->best;
        score->last = value;
        score->reports++;
        // The player's own numbers (#154): the local leaderboard's row.
        if (Entry *e = find_entry(&st, category, alias(), true)) {
            e->best = e->reports == 0 || value > e->best ? value : e->best;
            e->last = value;
            e->reports++;
        }
        if (store_save(&st)) {
            log("GameKit: score %" PRId64 " in \"%s\" saved for %s (best %" PRId64 ")", value,
                category, alias(), score->best);
        }
    }
    store_free(&st);
    post_handler(cpu, handler, deliver_nil_error);
}

// --- GKAchievement ----------------------------------------------------------------------------

// +[GKAchievement loadAchievementsWithCompletionHandler:]
void achievement_load(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    post_handler(cpu, mt_arg_u32(&a), deliver_achievements);
}

// +[GKAchievement resetAchievementsWithCompletionHandler:]
void achievement_reset(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const u32 handler = mt_arg_u32(&a);
    Store st;
    store_load(&st);
    for (u32 i = 0; i < st.achievement_count; i++) {
        std::free(st.achievements[i].identifier);
    }
    st.achievement_count = 0;
    if (store_save(&st)) {
        log("GameKit: achievements reset");
    }
    store_free(&st);
    post_handler(cpu, handler, deliver_nil_error);
}

// -[GKAchievement initWithIdentifier:]
void achievement_init_with_identifier(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const u32 identifier = mt_arg_u32(&a);
    const u32 self = mt_objc_self(cpu); // before any send: retain/release may use r0
    if (auto *s = state_of<AchievementState>(cpu, &k_achievement_state)) {
        assign_retained(cpu, &s->identifier, identifier);
    }
    mt_ret_u32(cpu, self);
}

// -[GKAchievement identifier]
void achievement_identifier(mt_cpu *cpu) {
    const auto *s = static_cast<const AchievementState *>(
        mt_objc_find_state(runtime(), mt_objc_self(cpu), &k_achievement_state));
    mt_ret_u32(cpu, s != nullptr ? s->identifier : 0u);
}

// -[GKAchievement percentComplete] -> double
void achievement_percent(mt_cpu *cpu) {
    const auto *s = static_cast<const AchievementState *>(
        mt_objc_find_state(runtime(), mt_objc_self(cpu), &k_achievement_state));
    mt_ret_f64(cpu, s != nullptr ? s->percent : 0.0);
}

// -[GKAchievement setPercentComplete:(double)]
void achievement_set_percent(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const double percent = mt_arg_f64(&a);
    if (auto *s = state_of<AchievementState>(cpu, &k_achievement_state)) {
        s->percent = percent;
    }
}

// Saves an achievement: the highest percentage wins, clamped to 0-100. Returns the percentage
// saved before (0 for a new one), or -1 if the file could not be written.
double save_achievement(const char *identifier, double percent) {
    percent = percent < 0.0 || percent != percent ? 0.0 : percent > 100.0 ? 100.0 : percent;
    Store st;
    store_load(&st);
    double before = -1.0;
    Achievement *ach = find_achievement(&st, identifier, true);
    if (ach != nullptr) {
        const double had = ach->percent;
        if (percent > ach->percent) {
            ach->percent = percent;
        }
        if (store_save(&st)) {
            log("GameKit: achievement \"%s\" at %g%% saved", identifier, ach->percent);
            before = had;
        }
    }
    store_free(&st);
    return before;
}

// -[GKAchievement reportAchievementWithCompletionHandler:]: saved now (the highest percentage
// wins, clamped to 0-100), the handler runs on the next pump.
void achievement_report(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const u32 handler = mt_arg_u32(&a);
    const auto *s = static_cast<const AchievementState *>(
        mt_objc_find_state(runtime(), mt_objc_self(cpu), &k_achievement_state));
    char identifier[k_text_max];
    string_text(cpu, s != nullptr ? s->identifier : 0u, identifier, sizeof identifier);
    save_achievement(identifier, s != nullptr ? s->percent : 0.0);
    post_handler(cpu, handler, deliver_nil_error);
}

// --- GKLeaderboard ------------------------------------------------------------------------------

void leaderboard_set_category(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const u32 category = mt_arg_u32(&a);
    if (auto *s = state_of<LeaderboardState>(cpu, &k_leaderboard_state)) {
        assign_retained(cpu, &s->category, category);
    }
}

void leaderboard_set_time_scope(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const std::int32_t scope = mt_arg_i32(&a);
    if (auto *s = state_of<LeaderboardState>(cpu, &k_leaderboard_state)) {
        s->time_scope = scope;
    }
}

// -[GKLeaderboard setRange:(NSRange)]: the structure's two words.
void leaderboard_set_range(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const u32 location = mt_arg_u32(&a), length = mt_arg_u32(&a);
    if (auto *s = state_of<LeaderboardState>(cpu, &k_leaderboard_state)) {
        s->range_location = location;
        s->range_length = length;
    }
}

// -[GKLeaderboard loadScoresWithCompletionHandler:]: no online scores: (nil, nil).
void leaderboard_load_scores(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    post_handler(cpu, mt_arg_u32(&a), deliver_nil_list);
}

// --- GKLeaderboardViewController --------------------------------------------------------------

void controller_set_category(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const u32 category = mt_arg_u32(&a);
    if (auto *s = state_of<LeaderboardControllerState>(cpu, &k_controller_state)) {
        assign_retained(cpu, &s->category, category);
    }
}

void controller_set_time_scope(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const std::int32_t scope = mt_arg_i32(&a);
    if (auto *s = state_of<LeaderboardControllerState>(cpu, &k_controller_state)) {
        s->time_scope = scope;
    }
}

void controller_set_delegate(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const u32 delegate = mt_arg_u32(&a);
    if (auto *s = state_of<LeaderboardControllerState>(cpu, &k_controller_state)) {
        s->delegate = delegate;
    }
}

// -[GKLeaderboardViewController viewDidAppear:]: UIKit (#54) sends it once the controller is
// presented (-[EAGLView viewGameCenterRank:]). There is no online leaderboard to show. With
// the host's hook (the local leaderboard, #154) the controller waits for the host to close it;
// without one it finishes at once: its delegate (EAGLView, 0x4DD5) dismisses it.
void controller_view_did_appear(mt_cpu *cpu) {
    const u32 self = mt_objc_self(cpu);
    runtime();
    mt_objc_retain(cpu, self);
    if (g_leaderboard_hook != nullptr && g_gk.open_leaderboard == 0) {
        const auto *s = static_cast<const LeaderboardControllerState *>(
            mt_objc_find_state(runtime(), self, &k_controller_state));
        char category[k_text_max];
        string_text(cpu, s != nullptr ? s->category : 0u, category, sizeof category);
        g_gk.open_leaderboard = self;
        log("GameKit: the leaderboard of \"%s\" is shown by the host", category);
        g_leaderboard_hook(g_leaderboard_user, category);
        return;
    }
    if (!post(deliver_leaderboard_finished, &self, 1)) {
        mt_objc_release(cpu, self);
    }
}

const mt_objc_host_method k_player_class_methods[] = {
    {"loadPlayersForIdentifiers:withCompletionHandler:", player_load_players},
};
const mt_objc_host_method k_player_methods[] = {
    {"playerID", player_id},
};
const mt_objc_host_method k_local_player_class_methods[] = {
    {"localPlayer", local_player},
};
const mt_objc_host_method k_local_player_methods[] = {
    {"isAuthenticated", local_player_is_authenticated},
    {"authenticateWithCompletionHandler:", local_player_authenticate},
};
const mt_objc_host_method k_score_methods[] = {
    {"initWithCategory:", score_init_with_category},
    {"setValue:", score_set_value},
    {"reportScoreWithCompletionHandler:", score_report},
};
const mt_objc_host_method k_achievement_class_methods[] = {
    {"loadAchievementsWithCompletionHandler:", achievement_load},
    {"resetAchievementsWithCompletionHandler:", achievement_reset},
};
const mt_objc_host_method k_achievement_methods[] = {
    {"initWithIdentifier:", achievement_init_with_identifier},
    {"identifier", achievement_identifier},
    {"percentComplete", achievement_percent},
    {"setPercentComplete:", achievement_set_percent},
    {"reportAchievementWithCompletionHandler:", achievement_report},
};
const mt_objc_host_method k_leaderboard_methods[] = {
    {"setCategory:", leaderboard_set_category},
    {"setTimeScope:", leaderboard_set_time_scope},
    {"setRange:", leaderboard_set_range},
    {"loadScoresWithCompletionHandler:", leaderboard_load_scores},
};
const mt_objc_host_method k_controller_methods[] = {
    {"setCategory:", controller_set_category},
    {"setTimeScope:", controller_set_time_scope},
    {"setLeaderboardDelegate:", controller_set_delegate},
    {"viewDidAppear:", controller_view_did_appear},
};

template <typename T, std::size_t N> constexpr u32 countof(const T (&)[N]) {
    return static_cast<u32>(N);
}

} // namespace

void gamekit_reset() { g_gk = GameKit{}; }

#if !defined(MT_HLE_CORE_ONLY) // the harnesses link the stubs without a C++ runtime library
// --- save states (#203, ADR 0105) ---------------------------------------------------------------
//
// The GameKit objects' values (guest objects as their addresses) and the module's: the local
// player, whether it authenticated, and the leaderboard the host shows. The alias and the hook
// are the host's.

namespace {

bool save_gk_score(mt_cpu *, u32, const void *state, mt_snap::Writer &w) {
    const auto *s = static_cast<const ScoreState *>(state);
    w.u32(s->category);
    w.i64(s->value);
    return true;
}
bool load_gk_score(mt_cpu *, u32, void *state, mt_snap::Reader &r) {
    auto *s = static_cast<ScoreState *>(state);
    return r.u32(&s->category) && r.i64(&s->value);
}
bool save_gk_achievement(mt_cpu *, u32, const void *state, mt_snap::Writer &w) {
    const auto *s = static_cast<const AchievementState *>(state);
    w.u32(s->identifier);
    w.f64(s->percent);
    return true;
}
bool load_gk_achievement(mt_cpu *, u32, void *state, mt_snap::Reader &r) {
    auto *s = static_cast<AchievementState *>(state);
    return r.u32(&s->identifier) && r.f64(&s->percent);
}
bool save_gk_leaderboard(mt_cpu *, u32, const void *state, mt_snap::Writer &w) {
    const auto *s = static_cast<const LeaderboardState *>(state);
    w.u32(s->category);
    w.u32(static_cast<u32>(s->time_scope));
    w.u32(s->range_location);
    w.u32(s->range_length);
    return true;
}
bool load_gk_leaderboard(mt_cpu *, u32, void *state, mt_snap::Reader &r) {
    auto *s = static_cast<LeaderboardState *>(state);
    return r.u32(&s->category) && r.i32(&s->time_scope) && r.u32(&s->range_location) &&
           r.u32(&s->range_length);
}
bool save_gk_controller(mt_cpu *, u32, const void *state, mt_snap::Writer &w) {
    const auto *s = static_cast<const LeaderboardControllerState *>(state);
    w.u32(s->category);
    w.u32(static_cast<u32>(s->time_scope));
    w.u32(s->delegate);
    return true;
}
bool load_gk_controller(mt_cpu *, u32, void *state, mt_snap::Reader &r) {
    auto *s = static_cast<LeaderboardControllerState *>(state);
    return r.u32(&s->category) && r.i32(&s->time_scope) && r.u32(&s->delegate);
}

bool save_gk_module(mt_cpu *, mt_snap::Writer &w, std::string *) {
    w.u32(g_gk.local_player);
    w.boolean(g_gk.authenticated);
    w.u32(g_gk.open_leaderboard);
    return true;
}
bool load_gk_module(mt_cpu *, mt_snap::Reader &r, std::function<void(mt_cpu *)> *apply) {
    GameKit gk{};
    if (!r.u32(&gk.local_player) || !r.boolean(&gk.authenticated) || !r.u32(&gk.open_leaderboard)) {
        return false;
    }
    *apply = [gk](mt_cpu *) { g_gk = gk; };
    return true;
}

} // namespace

extern const mt_snap::StateOps k_snap_gk_score, k_snap_gk_achievement, k_snap_gk_leaderboard,
    k_snap_gk_controller;
extern const mt_snap::Module k_snap_gamekit;
const mt_snap::StateOps k_snap_gk_score = {&k_score_state, "GKScore", save_gk_score, load_gk_score,
                                           nullptr};
const mt_snap::StateOps k_snap_gk_achievement = {&k_achievement_state, "GKAchievement",
                                                 save_gk_achievement, load_gk_achievement, nullptr};
const mt_snap::StateOps k_snap_gk_leaderboard = {&k_leaderboard_state, "GKLeaderboard",
                                                 save_gk_leaderboard, load_gk_leaderboard, nullptr};
const mt_snap::StateOps k_snap_gk_controller = {&k_controller_state, "GKLeaderboardViewController",
                                                save_gk_controller, load_gk_controller, nullptr};
const mt_snap::Module k_snap_gamekit = {"gamekit", save_gk_module, load_gk_module};
#endif // !MT_HLE_CORE_ONLY

const mt_objc_host_class k_gk_player = {"GKPlayer",
                                        "NSObject",
                                        4,
                                        k_player_methods,
                                        countof(k_player_methods),
                                        k_player_class_methods,
                                        countof(k_player_class_methods)};
const mt_objc_host_class k_gk_local_player = {"GKLocalPlayer",
                                              "GKPlayer",
                                              4,
                                              k_local_player_methods,
                                              countof(k_local_player_methods),
                                              k_local_player_class_methods,
                                              countof(k_local_player_class_methods)};
const mt_objc_host_class k_gk_score = {
    "GKScore", "NSObject", 4, k_score_methods, countof(k_score_methods), nullptr, 0};
const mt_objc_host_class k_gk_achievement = {"GKAchievement",
                                             "NSObject",
                                             4,
                                             k_achievement_methods,
                                             countof(k_achievement_methods),
                                             k_achievement_class_methods,
                                             countof(k_achievement_class_methods)};
const mt_objc_host_class k_gk_leaderboard = {
    "GKLeaderboard", "NSObject", 4, k_leaderboard_methods, countof(k_leaderboard_methods),
    nullptr,         0};
// A UIViewController (a UINavigationController) in GameKit: UIKit's UIViewController (#54),
// whose 152 bytes it starts after. The core build (MT_HLE_CORE_ONLY: no UIKit) makes it an
// NSObject; nothing subclasses it, so its size only has to cover NSObject's there.
#ifdef MT_HLE_CORE_ONLY
const mt_objc_host_class k_gk_leaderboard_view_controller = {
    "GKLeaderboardViewController", "NSObject", 4, k_controller_methods,
    countof(k_controller_methods), nullptr,    0};
#else
const mt_objc_host_class k_gk_leaderboard_view_controller = {"GKLeaderboardViewController",
                                                             "UIViewController",
                                                             152,
                                                             k_controller_methods,
                                                             countof(k_controller_methods),
                                                             nullptr,
                                                             0};
#endif

} // namespace mt_stubs_impl

// --- the local Game Center (#154): the host's side --------------------------------------------

using namespace mt_stubs_impl;

extern "C" void mt_gamekit_set_player_alias(const char *name) noexcept {
    g_alias[0] = '\0';
    if (name != nullptr) {
        std::snprintf(g_alias, sizeof g_alias, "%s", name);
    }
}

extern "C" const char *mt_gamekit_player_alias(void) noexcept { return alias(); }

extern "C" std::uint32_t mt_gamekit_entries(const char *category, mt_gamekit_entry *out,
                                            std::uint32_t max) noexcept {
    Store st;
    store_load(&st);
    // Best first, then by name: an insertion sort of the matching entries into `out`.
    u32 n = 0;
    for (u32 i = 0; i < st.entry_count; i++) {
        const Entry &e = st.entries[i];
        if (category != nullptr && std::strcmp(e.category, category) != 0) {
            continue;
        }
        mt_gamekit_entry row{};
        std::snprintf(row.category, sizeof row.category, "%s", e.category);
        std::snprintf(row.player, sizeof row.player, "%s", e.player);
        row.best = e.best;
        row.last = e.last;
        row.reports = e.reports;
        u32 at = n < max ? n : max;
        while (at > 0 &&
               (out[at - 1].best < row.best || (out[at - 1].best == row.best &&
                                                std::strcmp(out[at - 1].player, row.player) > 0))) {
            if (at < max) {
                out[at] = out[at - 1];
            }
            at--;
        }
        if (at < max) {
            out[at] = row;
        }
        n++;
    }
    store_free(&st);
    return n;
}

extern "C" void mt_gamekit_set_leaderboard_hook(mt_gamekit_leaderboard_fn fn, void *user) noexcept {
    g_leaderboard_hook = fn;
    g_leaderboard_user = user;
}

extern "C" int mt_gamekit_leaderboard_is_open(void) noexcept {
    runtime();
    return g_gk.open_leaderboard != 0 ? 1 : 0;
}

extern "C" void mt_gamekit_leaderboard_close(void) noexcept {
    runtime();
    const u32 controller = g_gk.open_leaderboard;
    if (controller == 0) {
        return;
    }
    g_gk.open_leaderboard = 0;
    log("GameKit: the host closed the leaderboard");
    // The delivery owns the retain that -viewDidAppear: took.
    if (!post(deliver_leaderboard_finished, &controller, 1)) {
        log("GameKit: the leaderboard cannot be dismissed: out of memory");
    }
}

extern "C" double mt_gamekit_report_achievement(const char *identifier, double percent) noexcept {
    if (identifier == nullptr || identifier[0] == '\0') {
        return -1.0;
    }
    return save_achievement(identifier, percent);
}

extern "C" double mt_gamekit_achievement_percent(const char *identifier) noexcept {
    Store st;
    store_load(&st);
    const Achievement *a =
        identifier != nullptr ? find_achievement(&st, identifier, false) : nullptr;
    const double percent = a != nullptr ? a->percent : 0.0;
    store_free(&st);
    return percent;
}
