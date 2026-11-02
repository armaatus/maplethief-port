// Game Center on this computer (#154, ADR 0084): the frontend's half of local_game_center = on.
// The runtime keeps the scores and achievements in gamecenter.json (maplethief/stubs.h,
// maplethief/offline.h); this draws them.
//
//   the leaderboard  the title menu's Game Center button presents GameKit's leaderboard
//                    controller, which waits (mt_gamekit_set_leaderboard_hook) while this
//                    screen lists the total ranking's entries: one row per save slot that has
//                    played, best score first, with the last score and the reports. Any of Esc,
//                    Backspace, Enter, Space, the gamepad's South, East or Start, or a click or
//                    tap closes it (mt_gamekit_leaderboard_close), and the game dismisses the
//                    controller as on the device.
//   the toast        an achievement unlocked (mt_achievement_next_notice) shows for 4 s in the
//                    window's top right corner, one after the other.
//   the list         achievement_rows(): every achievement, unlocked or not, for the settings
//                    menu (#89).
//
// Both are drawn with SDL's debug text into the window after the screen image and every
// read-back, like the price line (price_line.h): never in a --shot, a golden or a --capture.
// The leaderboard takes every input event while it is open (the game behind it gets none).
//
// Single thread: the frontend's, which is the guest's in the turn hook.
#ifndef MAPLETHIEF_PLATFORM_LOCAL_GAME_CENTER_H
#define MAPLETHIEF_PLATFORM_LOCAL_GAME_CENTER_H

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "maplethief/stubs.h"

namespace maplethief::frontend {

// The game's one leaderboard category (CMapleWorld::changeMap, CHero::LevelUp report it).
inline constexpr const char *kTotalRanking = "com.nexonmobile.maplestory.thief.totalranking";

// The leaderboard's lines for `entries` (best first) of `category`: a heading, a column line,
// one line per entry ("1. Slot 2   12,345   last 12,001   7 reports"), or a line that says no
// score was reported yet, and the closing hint.
std::vector<std::string> leaderboard_lines(const std::vector<mt_gamekit_entry> &entries,
                                           const char *category);

// gamecenter.json's entries of `category`, best first (at most 32).
std::vector<mt_gamekit_entry> leaderboard_entries(const char *category);

// One row of the achievements list.
struct AchievementRow {
    std::string title;
    std::string description;
    bool unlocked = false;
};
// Every local achievement in the table's order, with whether gamecenter.json has it.
std::vector<AchievementRow> achievement_rows();
// "3 of 7 unlocked".
std::string achievement_summary(const std::vector<AchievementRow> &rows);
// The settings menu's Achievements row (#89): its status line at the row's `n`th choice (from
// 0): the summary, then each achievement in turn ("2/7 Rookie (unlocked): ..."), then the
// summary again. `enabled`: local_game_center is on (else the summary says so).
std::string achievements_status(const std::vector<AchievementRow> &rows, std::size_t n,
                                bool enabled);
// The toast's text for achievement `index`: "Achievement unlocked: Rookie".
std::string achievement_toast_text(std::uint32_t index);

class LocalGameCenter {
  public:
    // How long a toast stays.
    static constexpr std::uint64_t kToastNs = 4'000'000'000ull;

    // Installs the leaderboard hook (on) or removes it; the achievements' listener follows.
    void install(bool enabled);
    bool enabled() const { return enabled_; }

    // The leaderboard hook (GameKit, on the guest thread): opens the screen.
    void open(const char *category);
    bool is_open() const { return open_; }
    const std::vector<std::string> &lines() const { return lines_; }
    // Closes the screen and lets the game dismiss its controller.
    void close();

    // An event: true if the leaderboard took it (it is open). Pass every event.
    bool handle(const SDL_Event &e);

    // Every turn: follows the runtime (a controller it dropped), takes the next notice when no
    // toast shows. `now_ns`: SDL_GetTicksNS.
    void tick(std::uint64_t now_ns);
    // The toast's text while it shows ("" if none).
    const std::string &toast() const { return toast_; }

    // Draws the open leaderboard and the toast into the window, each font pixel `scale` x
    // `scale` output pixels. Leaves the renderer's colour, blend mode and scale as they were.
    void draw(SDL_Renderer *renderer, int scale) const;

  private:
    bool enabled_ = false;
    bool open_ = false;
    std::string category_;
    std::vector<std::string> lines_;
    std::string toast_;
    std::uint64_t toast_until_ = 0;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_LOCAL_GAME_CENTER_H
