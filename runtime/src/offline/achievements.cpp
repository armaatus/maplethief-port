// Local achievements (#154, ADR 0084); see maplethief/offline.h. The shipped game reports no
// achievement (ADR 0034: -SubmitAchievement:PercentComplete: has no sender), so the port defines
// its own from what the observed functions see (maplethief/events.h, ADR 0053), and keeps them
// in gamecenter.json through GameKit's store (mt_gamekit_report_achievement), where
// +[GKAchievement loadAchievementsWithCompletionHandler:] finds them like any other.
#include <cstdint>
#include <cstdio>

#include "maplethief/events.h"
#include "maplethief/offline.h"
#include "maplethief/stubs.h"

namespace {

using u32 = std::uint32_t;

// The table. Identifiers are the file's: never change one once shipped.
constexpr mt_achievement k_table[] = {
    {"maplethief.local.new_character", "A New Thief", "Create a character."},
    {"maplethief.local.level_10", "Rookie", "Reach level 10."},
    {"maplethief.local.level_30", "Seasoned Thief", "Reach level 30."},
    {"maplethief.local.level_50", "Veteran", "Reach level 50."},
    {"maplethief.local.level_70", "Master Thief", "Reach level 70."},
    {"maplethief.local.first_fall", "Knocked Out", "Fall in battle."},
    {"maplethief.local.revived", "Back on Your Feet", "Get up again after a fall."},
};
constexpr u32 k_count = sizeof k_table / sizeof k_table[0];
constexpr u32 k_new_character = 0;
constexpr u32 k_level_first = 1; // the level achievements, in the order of k_levels
constexpr int k_levels[] = {10, 30, 50, 70};
constexpr u32 k_first_fall = 5;
constexpr u32 k_revived = 6;

bool g_installed = false;
// The unlock notices, oldest first (a ring).
u32 g_notices[MT_ACHIEVEMENT_NOTICES];
u32 g_first = 0, g_waiting = 0;

void notify(u32 index) {
    if (g_waiting == MT_ACHIEVEMENT_NOTICES) { // the oldest goes
        g_first = (g_first + 1) % MT_ACHIEVEMENT_NOTICES;
        g_waiting--;
    }
    g_notices[(g_first + g_waiting) % MT_ACHIEVEMENT_NOTICES] = index;
    g_waiting++;
}

void on_event(void *, const mt_game_event *event) {
    char alias[32];
    switch (event->kind) {
    case MT_GAME_EVENT_LEVEL_UP:
        for (u32 i = 0; i < sizeof k_levels / sizeof k_levels[0]; i++) {
            if (event->value >= k_levels[i]) {
                mt_achievement_unlock(k_level_first + i);
            }
        }
        break;
    case MT_GAME_EVENT_DEATH_POPUP:
        mt_achievement_unlock(k_first_fall);
        break;
    case MT_GAME_EVENT_REVIVED:
        mt_achievement_unlock(k_revived);
        break;
    case MT_GAME_EVENT_NEW_CHARACTER:
        mt_achievement_unlock(k_new_character);
        [[fallthrough]];
    case MT_GAME_EVENT_LOAD_CHARACTER:
        // The leaderboard's row: the save slot, as the character select screen numbers it.
        std::snprintf(alias, sizeof alias, "Slot %d", static_cast<int>(event->value) + 1);
        mt_gamekit_set_player_alias(alias);
        break;
    default:
        break;
    }
}

} // namespace

extern "C" std::uint32_t mt_achievements_count(void) noexcept { return k_count; }

extern "C" const mt_achievement *mt_achievement_at(std::uint32_t i) noexcept {
    return i < k_count ? &k_table[i] : nullptr;
}

extern "C" int mt_achievement_unlocked(std::uint32_t i) noexcept {
    return i < k_count && mt_gamekit_achievement_percent(k_table[i].identifier) >= 100.0 ? 1 : 0;
}

extern "C" int mt_achievement_unlock(std::uint32_t i) noexcept {
    if (i >= k_count) {
        return -1;
    }
    if (mt_achievement_unlocked(i) != 0) { // nothing to write
        return 0;
    }
    const double before = mt_gamekit_report_achievement(k_table[i].identifier, 100.0);
    if (before < 0.0) {
        return -1;
    }
    if (before >= 100.0) {
        return 0;
    }
    notify(i);
    return 1;
}

extern "C" void mt_achievements_install(int enabled) noexcept {
    if (enabled != 0) {
        g_installed = mt_game_events_add_listener(on_event, nullptr) == 0;
    } else {
        mt_game_events_remove_listener(on_event, nullptr);
        g_installed = false;
    }
}

extern "C" int mt_achievements_installed(void) noexcept { return g_installed ? 1 : 0; }

extern "C" int mt_achievement_next_notice(std::uint32_t *index) noexcept {
    if (g_waiting == 0) {
        return 0;
    }
    if (index != nullptr) {
        *index = g_notices[g_first];
    }
    g_first = (g_first + 1) % MT_ACHIEVEMENT_NOTICES;
    g_waiting--;
    return 1;
}

extern "C" void mt_achievement_clear_notices(void) noexcept { g_first = g_waiting = 0; }
