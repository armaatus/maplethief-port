// The frontend's half of local_game_center = on (#154, ADR 0084, platform/src/local_game_center.h):
// the leaderboard's lines from a synthetic gamecenter.json, the achievements list, and the
// screen's input (it takes every event while open and closes on a release). The runtime's half
// (the per-player scores, the achievements' listener, the controller that waits) is
// stubs_test.cpp's.
#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "local_game_center.h"
#include "maplethief/offline.h"
#include "maplethief/stubs.h"

namespace {

namespace fs = std::filesystem;
namespace fe = maplethief::frontend;

// A data directory of its own, with a gamecenter.json, for the test's lifetime.
struct DataDir {
    fs::path dir;
    explicit DataDir(const std::string &json) {
        dir = fs::temp_directory_path() /
              ("maplethief-local-gc-test-" + std::to_string(std::random_device{}()) + "-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(dir);
        std::ofstream(dir / "gamecenter.json", std::ios::binary) << json;
        REQUIRE(mt_stubs_set_data_dir(dir.string().c_str()) == 0);
    }
    ~DataDir() {
        mt_stubs_set_data_dir(nullptr);
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    DataDir(const DataDir &) = delete;
    DataDir &operator=(const DataDir &) = delete;
};

const char *const k_two_slots =
    "{\n  \"format\": \"maplethief-gamecenter\",\n  \"version\": 1,\n"
    "  \"achievements\": [\n"
    "    {\"identifier\": \"maplethief.local.level_10\", \"percentComplete\": 100},\n"
    "    {\"identifier\": \"maplethief.local.level_30\", \"percentComplete\": 40}\n  ],\n"
    "  \"scores\": [\n    {\"category\": \"com.nexonmobile.maplestory.thief.totalranking\", "
    "\"best\": 12987, \"last\": 12987, \"reports\": 9}\n  ],\n"
    "  \"entries\": [\n"
    "    {\"category\": \"com.nexonmobile.maplestory.thief.totalranking\", \"player\": \"Slot 1\", "
    "\"best\": 4995, \"last\": 4995, \"reports\": 3},\n"
    "    {\"category\": \"com.nexonmobile.maplestory.thief.totalranking\", \"player\": \"Slot 2\", "
    "\"best\": 12987, \"last\": 12000, \"reports\": 6},\n"
    "    {\"category\": \"other\", \"player\": \"Slot 3\", \"best\": 99999, \"last\": 1, "
    "\"reports\": 1}\n  ]\n}\n";

SDL_Event key(SDL_EventType type, SDL_Keycode k) {
    SDL_Event e{};
    e.type = type;
    e.key.key = k;
    e.key.down = type == SDL_EVENT_KEY_DOWN;
    return e;
}

SDL_Event of_type(SDL_EventType type) {
    SDL_Event e{};
    e.type = type;
    return e;
}

} // namespace

TEST_CASE("local Game Center: the leaderboard lists the category's entries, best first") {
    const DataDir data(k_two_slots);
    const std::vector<mt_gamekit_entry> entries = fe::leaderboard_entries(fe::kTotalRanking);
    REQUIRE(entries.size() == 2);
    CHECK(std::string(entries[0].player) == "Slot 2");
    CHECK(entries[0].best == 12987);
    CHECK(entries[0].last == 12000);
    CHECK(entries[0].reports == 6);
    CHECK(std::string(entries[1].player) == "Slot 1");

    const std::vector<std::string> lines = fe::leaderboard_lines(entries, fe::kTotalRanking);
    REQUIRE(lines.size() == 7);
    CHECK(lines[0] == "Leaderboard: Total ranking (this computer)");
    CHECK(lines[2].rfind("#   Player", 0) == 0);
    CHECK(lines[3].rfind("1.  Slot 2", 0) == 0);
    CHECK(lines[3].find("12,987") != std::string::npos);
    CHECK(lines[3].find("12,000") != std::string::npos);
    CHECK(lines[4].rfind("2.  Slot 1", 0) == 0);
    CHECK(lines[4].find("4,995") != std::string::npos);
    CHECK(lines.back() == "Esc, Enter, A or a click: back to the menu");
    // The columns line up: every row as wide as the column line.
    CHECK(lines[3].size() == lines[2].size());
    CHECK(lines[4].size() == lines[2].size());

    const std::vector<std::string> none = fe::leaderboard_lines({}, fe::kTotalRanking);
    CHECK(none[2] == "No score yet: the game reports one when your");
}

TEST_CASE("local Game Center: the achievements list reads gamecenter.json") {
    const DataDir data(k_two_slots);
    const std::vector<fe::AchievementRow> rows = fe::achievement_rows();
    REQUIRE(rows.size() == mt_achievements_count());
    REQUIRE(rows.size() == 7);
    CHECK(rows[0].title == "A New Thief");
    CHECK_FALSE(rows[0].unlocked);
    CHECK(rows[1].title == "Rookie");
    CHECK(rows[1].unlocked);       // level_10 at 100 %
    CHECK_FALSE(rows[2].unlocked); // level_30 at 40 % is not unlocked
    CHECK_FALSE(rows[1].description.empty());
    CHECK(fe::achievement_summary(rows) == "1 of 7 unlocked");
    CHECK(fe::achievement_toast_text(1) == "Achievement unlocked: Rookie");
    CHECK(fe::achievement_toast_text(99).empty());

    // An unlock queues its toast: shown at the next tick for 4 s, then the next one.
    fe::LocalGameCenter gc;
    gc.install(true);
    CHECK(mt_achievement_unlock(0) == 1);
    CHECK(mt_achievement_unlock(0) == 0); // once
    CHECK(mt_achievement_unlock(3) == 1);
    CHECK(fe::achievement_summary(fe::achievement_rows()) == "3 of 7 unlocked");
    gc.tick(1'000);
    CHECK(gc.toast() == "Achievement unlocked: A New Thief");
    gc.tick(1'000 + fe::LocalGameCenter::kToastNs - 1);
    CHECK(gc.toast() == "Achievement unlocked: A New Thief");
    gc.tick(1'000 + fe::LocalGameCenter::kToastNs);
    CHECK(gc.toast() == "Achievement unlocked: Veteran");
    gc.install(false);
    CHECK(mt_achievements_installed() == 0);
}

TEST_CASE("local Game Center: the settings menu's Achievements row steps through the list") {
    // #89's action row (game.cpp's start_menu): the summary, each achievement, the summary again.
    const std::vector<fe::AchievementRow> rows = {
        {"A New Thief", "Create a character", false},
        {"Rookie", "Reach level 10", true},
    };
    CHECK(fe::achievements_status(rows, 0, true) == "1 of 2 unlocked; choose again for each");
    CHECK(fe::achievements_status(rows, 1, true) == "1/2 A New Thief: Create a character");
    CHECK(fe::achievements_status(rows, 2, true) == "2/2 Rookie (unlocked): Reach level 10");
    CHECK(fe::achievements_status(rows, 3, true) == fe::achievements_status(rows, 0, true));
    CHECK(fe::achievements_status(rows, 0, false) == "1 of 2 unlocked (Local Game Center is off)");
    CHECK(fe::achievements_status({}, 5, true) == "0 of 0 unlocked; choose again for each");
}

TEST_CASE("local Game Center: the open leaderboard takes every input and closes on a release") {
    const DataDir data(k_two_slots);
    fe::LocalGameCenter gc;
    // Closed: nothing is taken.
    SDL_Event down = key(SDL_EVENT_KEY_DOWN, SDLK_ESCAPE);
    CHECK_FALSE(gc.handle(down));

    gc.open(fe::kTotalRanking);
    REQUIRE(gc.is_open());
    CHECK(gc.lines()[3].rfind("1.  Slot 2", 0) == 0);
    // The press is eaten and leaves it open; another key's release too.
    CHECK(gc.handle(down));
    CHECK(gc.is_open());
    SDL_Event other = key(SDL_EVENT_KEY_UP, SDLK_LEFT);
    CHECK(gc.handle(other));
    CHECK(gc.is_open());
    SDL_Event motion = of_type(SDL_EVENT_MOUSE_MOTION);
    CHECK(gc.handle(motion));
    // Window and lifecycle events go on to the frontend.
    SDL_Event resized = of_type(SDL_EVENT_WINDOW_RESIZED);
    CHECK_FALSE(gc.handle(resized));
    SDL_Event up = key(SDL_EVENT_KEY_UP, SDLK_ESCAPE);
    CHECK(gc.handle(up));
    CHECK_FALSE(gc.is_open());
    CHECK_FALSE(gc.handle(up));

    gc.open(fe::kTotalRanking);
    SDL_Event click = of_type(SDL_EVENT_MOUSE_BUTTON_DOWN);
    CHECK(gc.handle(click));
    CHECK(gc.is_open());
    click.type = SDL_EVENT_MOUSE_BUTTON_UP;
    CHECK(gc.handle(click));
    CHECK_FALSE(gc.is_open());

    gc.open(fe::kTotalRanking);
    SDL_Event pad = of_type(SDL_EVENT_GAMEPAD_BUTTON_UP);
    pad.gbutton.button = SDL_GAMEPAD_BUTTON_WEST;
    CHECK(gc.handle(pad));
    CHECK(gc.is_open());
    pad.gbutton.button = SDL_GAMEPAD_BUTTON_SOUTH;
    CHECK(gc.handle(pad));
    CHECK_FALSE(gc.is_open());
}

TEST_CASE(
    "local Game Center: the leaderboard is drawn centred and the renderer is left as it was") {
    const DataDir data(k_two_slots);
    SDL_Surface *surface = SDL_CreateSurface(960, 640, SDL_PIXELFORMAT_RGBA32);
    REQUIRE(surface != nullptr);
    SDL_Renderer *r = SDL_CreateSoftwareRenderer(surface);
    REQUIRE(r != nullptr);
    SDL_SetRenderDrawColor(r, 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(r);
    SDL_SetRenderDrawColor(r, 1, 2, 3, 4);

    // Neither open nor a toast: nothing drawn.
    fe::LocalGameCenter gc;
    gc.draw(r, 2);
    SDL_RenderPresent(r);
    auto pixel = [&](int x, int y) {
        const auto *row = static_cast<const std::uint8_t *>(surface->pixels) + y * surface->pitch;
        return std::uint32_t(row[x * 4]) | std::uint32_t(row[x * 4 + 1]) << 8 |
               std::uint32_t(row[x * 4 + 2]) << 16;
    };
    auto lit_in = [&](int x0, int y0, int x1, int y1) {
        int lit = 0;
        for (int y = y0; y < y1; y++) {
            for (int x = x0; x < x1; x++) {
                lit += pixel(x, y) != 0 ? 1 : 0;
            }
        }
        return lit;
    };
    CHECK(lit_in(0, 0, 960, 640) == 0);

    gc.open(fe::kTotalRanking);
    gc.draw(r, 2);
    SDL_RenderPresent(r);
    // The panel is centred: lit in the middle, nothing in the corners.
    CHECK(lit_in(400, 280, 560, 360) > 0);
    CHECK(lit_in(0, 0, 40, 40) == 0);
    CHECK(lit_in(920, 600, 960, 640) == 0);
    Uint8 cr = 0, cg = 0, cb = 0, ca = 0;
    SDL_GetRenderDrawColor(r, &cr, &cg, &cb, &ca);
    CHECK((cr == 1 && cg == 2 && cb == 3 && ca == 4));
    float sx = 0, sy = 0;
    SDL_GetRenderScale(r, &sx, &sy);
    CHECK((sx == 1.0f && sy == 1.0f));

    SDL_DestroyRenderer(r);
    SDL_DestroySurface(surface);
}
