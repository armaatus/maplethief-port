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
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "stubs/json.h"
#include "stubs/stubs_internal.h"

namespace mt_stubs_impl {
namespace {

constexpr u32 k_text_max = 512;
constexpr const char *k_format = "maplethief-gamecenter";
constexpr std::int64_t k_version = 1;

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

struct Store {
    Achievement *achievements;
    u32 achievement_count;
    Score *scores;
    u32 score_count;
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
    std::free(st->achievements);
    std::free(st->scores);
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
    char *text = read_file(path, &size);
    if (text == nullptr) {
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
    json_raw(&o, st->score_count != 0 ? "\n  ]\n}\n" : "]\n}\n");
    const bool ok = !o.failed && write_file(path, o.data, o.size);
    json_out_free(&o);
    return ok;
}

// --- per-runtime state ------------------------------------------------------------------------

struct GameKit {
    u32 local_player;   // the GKLocalPlayer singleton (retained for the runtime's lifetime)
    bool authenticated; // set when the authentication request completes
};
GameKit g_gk;

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

const mt_objc_state_type k_score_state = {"GKScore", sizeof(ScoreState), release_score};
const mt_objc_state_type k_achievement_state = {"GKAchievement", sizeof(AchievementState),
                                                release_achievement};
const mt_objc_state_type k_leaderboard_state = {"GKLeaderboard", sizeof(LeaderboardState),
                                                release_leaderboard};
const mt_objc_state_type k_controller_state = {
    "GKLeaderboardViewController", sizeof(LeaderboardControllerState), release_controller};

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
        if (store_save(&st)) {
            log("GameKit: score %" PRId64 " in \"%s\" saved (best %" PRId64 ")", value, category,
                score->best);
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

// -[GKAchievement reportAchievementWithCompletionHandler:]: saved now (the highest percentage
// wins, clamped to 0-100), the handler runs on the next pump.
void achievement_report(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const u32 handler = mt_arg_u32(&a);
    const auto *s = static_cast<const AchievementState *>(
        mt_objc_find_state(runtime(), mt_objc_self(cpu), &k_achievement_state));
    char identifier[k_text_max];
    string_text(cpu, s != nullptr ? s->identifier : 0u, identifier, sizeof identifier);
    double percent = s != nullptr ? s->percent : 0.0;
    percent = percent < 0.0 || percent != percent ? 0.0 : percent > 100.0 ? 100.0 : percent;
    Store st;
    store_load(&st);
    Achievement *ach = find_achievement(&st, identifier, true);
    if (ach != nullptr) {
        if (percent > ach->percent) {
            ach->percent = percent;
        }
        if (store_save(&st)) {
            log("GameKit: achievement \"%s\" at %g%% saved", identifier, ach->percent);
        }
    }
    store_free(&st);
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
// presented (-[EAGLView viewGameCenterRank:]). There is no online leaderboard to show, so the
// controller finishes at once: its delegate (EAGLView, 0x4DD5) dismisses it.
void controller_view_did_appear(mt_cpu *cpu) {
    const u32 self = mt_objc_self(cpu);
    runtime();
    mt_objc_retain(cpu, self);
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
