// The data folder as the player sees and changes it (#263, ADR 0127, data_switch.h): the path
// shortened to fit, the Saves tab's Data location row for a normal folder and for every fallback
// reason (through the resolver's seam, set_data_location_for_test), the switch both ways (the
// data copied, the source untouched, user-data.txt written or removed), a destination that has
// data, the refusals, --switch-data-location's lines and status, the fallback's one-time notice
// (data_fallback_seen in settings.ini, which parse_settings accepts) and a row dump and drawing of
// the Saves tab with a long path.
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include "data_copy.h"
#include "data_location.h"
#include "data_notice.h"
#include "data_switch.h"
#include "ipa_import.h"
#include "settings.h"
#include "settings_menu.h"

namespace {

namespace fs = std::filesystem;
namespace fe = maplethief::frontend;
using Place = fe::DataPlace;
using Reason = fe::DataReason;
using Tree = std::map<std::string, std::string>;

struct TempDir {
    fs::path path;
    explicit TempDir(const char *name) {
        path = fs::temp_directory_path() / (std::string("maplethief-data-switch-") + name);
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_text(const fs::path &p, const std::string &text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}
std::string read_text(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
Tree tree(const fs::path &dir) {
    Tree t;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return t;
    }
    for (const auto &e : fs::recursive_directory_iterator(dir)) {
        if (e.is_regular_file()) {
            t[fs::relative(e.path(), dir).generic_string()] = read_text(e.path());
        }
    }
    return t;
}
std::string u8(const fs::path &p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

// A player's data folder: a save set the checks accept (the options file and the terms), the
// frontend's files, a backup, a state and a screenshot.
void write_player_data(const fs::path &dir, const std::string &tag) {
    write_text(dir / "saves" / "config", std::string("\x03\x01\x00\x01\x01\x01", 6));
    write_text(dir / "saves" / "CTerms", std::string(2, '\0'));
    write_text(dir / "saves" / "gamecenter.json", "{\"tag\":\"" + tag + "\"}");
    write_text(dir / "settings.ini", "preset = enhanced\n# " + tag + "\n");
    write_text(dir / "bindings.ini", "# bindings " + tag + "\n");
    write_text(dir / "window.ini", "# window " + tag + "\n");
    write_text(dir / "backups" / "slot0" / "20261101-120000-000" / "theif0", "b" + tag);
    write_text(dir / "states" / "slot0.state", "s" + tag);
    write_text(dir / "screenshots" / "shot.png", "p" + tag);
}

// The game folder `base` (its data/ the location kGame) and a per-user folder `user`.
fe::DataLocation game_at(const fs::path &base, Reason reason = Reason::kDefault) {
    return fe::data_location_at(u8(base / "data"), Place::kGame, reason, u8(base));
}
fe::DataLocation user_at(const fs::path &user, const fs::path &base, Reason reason,
                         const std::string &why = "") {
    fe::DataLocation loc = fe::data_location_at(u8(user), Place::kUser, reason, u8(base));
    loc.why = why;
    return loc;
}

fe::DataSwitchInputs inputs(const fs::path &user, int *user_calls = nullptr) {
    fe::DataSwitchInputs in;
    in.user_dir = [user, user_calls] {
        if (user_calls != nullptr) {
            ++*user_calls;
        }
        return u8(user);
    };
    in.local_time = false;
    // The synthetic bundle's executable, "exe" (the copy checks its SHA-256).
    const std::string exe = "exe";
    in.bundle_sha256 =
        fe::sha256_of(std::span(reinterpret_cast<const std::uint8_t *>(exe.data()), exe.size()));
    return in;
}

SDL_Event key_event(SDL_Keycode k) {
    SDL_Event e{};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = k;
    e.key.down = true;
    return e;
}
SDL_Event pad_event(SDL_GamepadButton b) {
    SDL_Event e{};
    e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    e.gbutton.button = static_cast<Uint8>(b);
    return e;
}

std::uint64_t fnv1a(const std::vector<std::uint8_t> &bytes) {
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (const std::uint8_t b : bytes) {
        h = (h ^ b) * 0x100000001B3ull;
    }
    return h;
}

std::string output_dir() {
    const char *env = std::getenv("MAPLETHIEF_TEST_OUTPUT_DIR");
    return env != nullptr && *env != '\0' ? env : MT_TEST_OUTPUT_DIR;
}

// The menu as game.cpp's start_menu wires the Data location row, over data_location() (the
// resolver's seam gives it its answer), with `panel` for mt_input_panel().
struct Menu {
    fe::Settings live;
    fe::SettingsMenu menu{fe::settings_registry(), nullptr, nullptr, nullptr};
    std::int32_t panel = -1;
    int flushes = 0;
    bool switched = false;

    explicit Menu(const fe::DataSwitchInputs &in) {
        menu.attach(&live);
        menu.set_tab_note(fe::SettingGroup::kSaves, [] {
            return fe::data_location_row(fe::data_location(), fe::SettingsMenu::kLineChars).note;
        });
        menu.set_action_view("data_location", [] {
            const fe::DataLocationView v = fe::data_location_view(fe::data_location());
            return fe::SettingsMenu::ActionView{v.value, v.help};
        });
        menu.set_action_question("data_location", [this, in] {
            return fe::data_location_question(fe::data_location(), panel, in);
        });
        menu.set_action("data_location", [this, in] {
            const fe::DataSwitchResult r =
                fe::data_location_action(fe::data_location(), panel, in, [this] { flushes++; });
            switched = switched || r.ok;
            return r.message;
        });
        for (const char *key :
             {"export_saves", "import_saves", "backups", "open_data_folder", "open_logs_folder"}) {
            menu.set_action(key, [] { return std::string(); });
        }
    }
    void key(SDL_Keycode k) { menu.handle(key_event(k)); }
    void pad(SDL_GamepadButton b) { menu.handle(pad_event(b)); }
    // Open on the Saves tab's Data location row, with the d-pad (the shoulders, then down).
    void reach() {
        if (!menu.is_open()) {
            key(SDLK_F1);
        }
        for (std::size_t i = 0;
             i < menu.tabs().size() && menu.tabs()[menu.tab()] != fe::SettingGroup::kSaves; i++) {
            pad(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
        }
        REQUIRE(menu.tabs()[menu.tab()] == fe::SettingGroup::kSaves);
        for (std::size_t i = 0; i < menu.items().size(); i++) {
            const auto &it = menu.items()[menu.selected()];
            if (it.kind == fe::SettingsMenu::ItemKind::kAction &&
                std::string(it.action->key) == "data_location") {
                return;
            }
            pad(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        }
        FAIL("no Data location row");
    }
};

struct Seam {
    explicit Seam(const fe::DataLocation &loc) { fe::set_data_location_for_test(loc); }
    ~Seam() { fe::set_data_location_for_test(std::nullopt); }
};

} // namespace

TEST_CASE("data switch: a path shortened in the middle to fit, whole when it fits") {
    CHECK(fe::shorten_path_middle("/a/b", 10) == "/a/b");
    const std::string p = "/Users/someone/Games/MapleThief-1.0.0-macos-universal/data";
    const std::string s = fe::shorten_path_middle(p, 30);
    CHECK(s.size() == 30);
    CHECK(s.find("...") != std::string::npos);
    CHECK(s.substr(0, 3) == "/Us");
    CHECK(s.substr(s.size() - 10) == p.substr(p.size() - 10)); // the end, the folder, is kept
    CHECK(fe::shorten_path_middle(p, 3) == "...");
    // UTF-8: never cut inside a character.
    const std::string u = "/home/j\xC3\xB6rg/\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9/data";
    for (std::size_t max = 4; max < u.size(); max++) {
        const std::string t = fe::shorten_path_middle(u, max);
        CAPTURE(max);
        CHECK(t.size() <= max);
        for (std::size_t i = 0; i + 1 < t.size(); i++) {
            const bool lead = (static_cast<unsigned char>(t[i]) & 0xE0) == 0xC0;
            if (lead) {
                CHECK((static_cast<unsigned char>(t[i + 1]) & 0xC0) == 0x80);
            }
        }
    }
}

TEST_CASE("data switch: the Saves tab's row for a normal folder and for every fallback reason") {
    TempDir t("rows");
    const fs::path base = t.path / "MapleThief-1.0-macos-universal";
    const fs::path user = t.path / "Application Support" / "MapleThief";
    struct Case {
        fe::DataLocation loc;
        const char *value;
        const char *help; // a part of the help
        bool switchable;
    };
    fe::DataLocation env = fe::data_location_at(u8(t.path / "env"));
    const Case cases[] = {
        {game_at(base), "Game folder", "Your data is next to the game", true},
        {game_at(base, Reason::kFlag), "Game folder", "--data-location game", true},
        {user_at(user, base, Reason::kMarker), "User folder", "user-data.txt", true},
        {user_at(user, base, Reason::kFlag), "User folder", "--data-location user", true},
        {user_at(user, base, Reason::kApplicationsFolder), "User folder", "in Applications", false},
        {user_at(user, base, Reason::kFallbackNotWritable, "Permission denied"), "User folder",
         "Can't write next to the game (Permission denied)", false},
        {user_at(user, base, Reason::kFallbackTranslocated), "User folder", "App Translocation",
         false},
        {user_at(user, "", Reason::kFallbackNoBase), "User folder", "folder is unknown", false},
        {env, "MAPLETHIEF_DATA_DIR", "Set by MAPLETHIEF_DATA_DIR", false},
    };
    for (const Case &k : cases) {
        CAPTURE(fe::data_reason_name(k.loc.reason));
        Seam seam(k.loc);
        Menu m(inputs(user));
        m.reach();
        const std::size_t row = m.menu.selected();
        CHECK(m.menu.row_label(row) == "Data location");
        CHECK(m.menu.row_value(row) == k.value);
        CHECK(m.menu.help().find(k.help) != std::string::npos);
        CHECK(m.menu.note() == "Data: " + fe::shorten_path_middle(k.loc.dir, 73 - 6));
        CHECK(m.menu.note().size() <= fe::SettingsMenu::kLineChars);
        // Choosing it: a question (No focused) when it can switch, else the reason at once.
        m.pad(SDL_GAMEPAD_BUTTON_SOUTH);
        if (k.switchable) {
            CHECK(m.menu.confirming());
            CHECK_FALSE(m.menu.confirm_yes());
            CHECK(m.menu.help().find("Copy your data to ") == 0);
            CHECK(m.menu.row_value(row) == "No");
            m.pad(SDL_GAMEPAD_BUTTON_SOUTH); // No: nothing
            CHECK_FALSE(m.menu.confirming());
            CHECK_FALSE(m.switched);
        } else {
            CHECK_FALSE(m.menu.confirming());
            CHECK(m.menu.status().find("Not switched") == std::string::npos);
            CHECK_FALSE(m.menu.status().empty());
            CHECK_FALSE(m.switched);
        }
        CHECK(m.flushes == 0);
        // The fallback's notice text names the folder in use.
        if (k.loc.fallback()) {
            CHECK(fe::data_fallback_text(k.loc).find(k.loc.dir) != std::string::npos);
        } else {
            CHECK(fe::data_fallback_text(k.loc).empty());
        }
    }
    // A fallback refuses the game folder with its own words.
    const fe::DataSwitchPlan p = fe::plan_data_switch(
        user_at(user, base, Reason::kFallbackNotWritable, "Read-only file system"), Place::kGame,
        inputs(user));
    CHECK(p.refused == "The game folder can't be used: Read-only file system");
    CHECK(fe::plan_data_switch(env, Place::kUser, inputs(user)).refused ==
          "Set by MAPLETHIEF_DATA_DIR");
    // Nothing on the disk was made by drawing or asking.
    CHECK_FALSE(fs::exists(base));
    CHECK_FALSE(fs::exists(user));
}

TEST_CASE("data switch: game folder to user folder and back: copied, source untouched, marker") {
    TempDir t("both-ways");
    const fs::path base = t.path / "kit";
    const fs::path user = t.path / "user";
    write_player_data(base / "data", "game");
    write_text(base / "game" / "MapleThief", "exe");
    write_text(base / "data" / "saves" / "x.mt-tmp", "half");
    const Tree game_before = tree(base / "data");
    const Tree bundle_before = tree(base / "game");

    // To the user folder, through the menu with the pad: South asks, Right picks Yes, South.
    {
        Seam seam(game_at(base));
        int user_calls = 0;
        Menu m(inputs(user, &user_calls));
        m.reach();
        CHECK(user_calls == 0); // drawing never asks for the per-user folder
        m.pad(SDL_GAMEPAD_BUTTON_SOUTH);
        REQUIRE(m.menu.confirming());
        CHECK(m.menu.help() ==
              "Copy your data to " + fe::shorten_path_middle(u8(user), 70) + " and restart?");
        m.pad(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
        CHECK(m.menu.confirm_yes());
        m.pad(SDL_GAMEPAD_BUTTON_SOUTH);
        CHECK(m.switched);
        CHECK(m.flushes == 1);
        CHECK(m.menu.status() == "Done. MapleThief will close; start it again to use " +
                                     fe::shorten_path_middle(u8(user), 70) + ".");
    }
    CHECK(tree(base / "data") == game_before); // the source is never changed
    CHECK(tree(base / "game") == bundle_before);
    Tree expected = game_before;
    expected.erase("saves/x.mt-tmp");
    CHECK(tree(user) == [&] {
        Tree e = expected;
        e["game/MapleThief"] = "exe"; // the bundle, which the user folder lacked
        return e;
    }());
    REQUIRE(fs::exists(base / "user-data.txt"));
    CHECK(read_text(base / "user-data.txt").find(u8(user)) != std::string::npos);
    CHECK(fe::data_dir_has_player_data(user));

    // And back to the game folder: its data/ has data, so the question says so; Yes uses it.
    // Remove it first to copy back into an empty game folder.
    fs::remove_all(base / "data");
    write_text(user / "saves" / "theif-extra.mt-tmp", "x");
    write_text(user / "settings.ini", "preset = original\n# changed in the user folder\n");
    const Tree user_before = tree(user);
    {
        Seam seam(user_at(user, base, Reason::kMarker));
        Menu m(inputs(user));
        m.reach();
        CHECK(m.menu.row_value(m.menu.selected()) == "User folder");
        m.key(SDLK_RETURN);
        REQUIRE(m.menu.confirming());
        CHECK(m.menu.help() == "Copy your data to " +
                                   fe::shorten_path_middle(u8(base / "data"), 70) +
                                   " and restart?");
        m.key(SDLK_RIGHT);
        m.key(SDLK_RETURN);
        CHECK(m.switched);
    }
    CHECK(tree(user) == user_before);
    CHECK_FALSE(fs::exists(base / "user-data.txt"));
    CHECK(read_text(base / "data" / "settings.ini") ==
          "preset = original\n# changed in the user folder\n");
    CHECK(read_text(base / "data" / "saves" / "gamecenter.json") == "{\"tag\":\"game\"}");
    CHECK_FALSE(fs::exists(base / "data" / "saves" / "theif-extra.mt-tmp"));
    CHECK(tree(base / "game") == bundle_before);
    // No staging folder is left behind.
    for (const auto &e : fs::directory_iterator(base)) {
        CHECK(e.path().filename().string().find(".tmp-") == std::string::npos);
    }
    for (const auto &e : fs::directory_iterator(t.path)) {
        CHECK(e.path().filename().string().find(".tmp-") == std::string::npos);
    }
}

TEST_CASE("data switch: a destination with data is used as it is, never overwritten") {
    TempDir t("has-data");
    const fs::path base = t.path / "kit";
    const fs::path user = t.path / "user";
    write_player_data(base / "data", "game");
    write_player_data(user, "user");
    const Tree game_before = tree(base / "data");
    const Tree user_before = tree(user);
    const fs::file_time_type when = fs::file_time_type::clock::now() - std::chrono::hours(48);
    for (const auto &e : fs::recursive_directory_iterator(user)) {
        fs::last_write_time(e.path(), when);
    }
    const fe::DataSwitchPlan plan = fe::plan_data_switch(game_at(base), Place::kUser, inputs(user));
    CHECK(plan.refused.empty());
    CHECK(plan.dest_has_data);
    REQUIRE(plan.dest_saved.size() == 19); // "YYYY-MM-DD HH:MM:SS"
    CHECK(plan.question == fe::shorten_path_middle(u8(user), 70) + " already has data (saved " +
                               plan.dest_saved +
                               "). Switch and use that data? Your data here stays here.");
    const fe::DataSwitchResult r = fe::run_data_switch(game_at(base), plan, inputs(user));
    CHECK(r.ok);
    CHECK(tree(user) == user_before);
    CHECK(tree(base / "data") == game_before);
    CHECK(fs::exists(base / "user-data.txt"));
}

TEST_CASE("data switch: refused with a character loaded, under the env, the fallbacks and "
          "Applications; a game folder that can't be written") {
    TempDir t("refused");
    const fs::path base = t.path / "kit";
    const fs::path user = t.path / "user";
    write_player_data(base / "data", "game");
    {
        Seam seam(game_at(base));
        Menu m(inputs(user));
        m.panel = 0; // a character is loaded
        m.reach();
        m.pad(SDL_GAMEPAD_BUTTON_SOUTH);
        CHECK_FALSE(m.menu.confirming());
        CHECK(m.menu.status() == "Switch from the title or the main menu (a character is loaded)");
        CHECK_FALSE(m.switched);
        CHECK(m.flushes == 0);
    }
    CHECK_FALSE(fs::exists(user));
    CHECK(fe::plan_data_switch(user_at(user, base, Reason::kApplicationsFolder), Place::kGame,
                               inputs(user))
              .refused.find("Applications") != std::string::npos);
    CHECK(fe::plan_data_switch(user_at(user, base, Reason::kFallbackTranslocated), Place::kGame,
                               inputs(user))
              .refused.find("App Translocation") != std::string::npos);
    CHECK_FALSE(
        fe::plan_data_switch(user_at(user, "", Reason::kFallbackNoBase), Place::kGame, inputs(user))
            .refused.empty());
    // The probe of the game folder fails: refused, nothing written, the marker stays.
    write_player_data(user, "user");
    write_text(base / "user-data.txt", "marker");
    fs::remove_all(base / "data");
    fe::DataSwitchInputs in = inputs(user);
    in.probe = [](const std::string &, std::string *why) {
        *why = "Read-only file system";
        return false;
    };
    const fe::DataLocation cur = user_at(user, base, Reason::kMarker);
    const fe::DataSwitchResult r =
        fe::run_data_switch(cur, fe::plan_data_switch(cur, Place::kGame, in), in);
    CHECK_FALSE(r.ok);
    CHECK(r.message.find("Read-only file system") != std::string::npos);
    CHECK(read_text(base / "user-data.txt") == "marker");
    // A save set that fails the checks: nothing is copied, the marker stays.
    write_text(user / "saves" / "theif0", "too short");
    in.probe = {};
    const fe::DataSwitchResult bad =
        fe::run_data_switch(cur, fe::plan_data_switch(cur, Place::kGame, in), in);
    CHECK_FALSE(bad.ok);
    CHECK(bad.message.find("Not switched") == 0);
    CHECK(tree(base / "data").empty());
    CHECK(read_text(base / "user-data.txt") == "marker");
}

TEST_CASE("data switch: --switch-data-location's lines and status") {
    TempDir t("command");
    const fs::path base = t.path / "kit";
    const fs::path user = t.path / "user";
    write_player_data(base / "data", "game");
    std::vector<std::string> lines;
    CHECK(fe::switch_data_location_command(game_at(base), Place::kGame, inputs(user), &lines) == 0);
    CHECK(lines.back().find("already uses") != std::string::npos);
    lines.clear();
    CHECK(fe::switch_data_location_command(game_at(base), Place::kUser, inputs(user), &lines) == 0);
    CHECK(lines.back() == "location: user");
    CHECK(std::find(lines.begin(), lines.end(), "data: " + u8(user)) != lines.end());
    CHECK(fs::exists(base / "user-data.txt"));
    lines.clear();
    CHECK(fe::switch_data_location_command(fe::data_location_at(u8(t.path / "env")), Place::kUser,
                                           inputs(user), &lines) == 1);
    CHECK(lines.back() == "Set by MAPLETHIEF_DATA_DIR");
    lines.clear();
    // Back with the destination holding data (the game folder's): used, the marker removed.
    CHECK(fe::switch_data_location_command(user_at(user, base, Reason::kMarker), Place::kGame,
                                           inputs(user), &lines) == 0);
    CHECK_FALSE(fs::exists(base / "user-data.txt"));
}

TEST_CASE("data switch: the fallback's title notice once per reason (data_fallback_seen)") {
    TempDir t("notice");
    const fs::path user = t.path / "user";
    const fs::path base = t.path / "kit";
    (void)fe::take_data_notices();
    CHECK_FALSE(fe::post_data_fallback_notice(game_at(base))); // no fallback, no notice
    const fe::DataLocation trans = user_at(user, base, Reason::kFallbackTranslocated);
    CHECK(fe::post_data_fallback_notice(trans));
    std::vector<std::string> shown = fe::take_data_notices();
    REQUIRE(shown.size() == 1);
    CHECK(shown[0] == fe::data_fallback_text(trans));
    CHECK(fe::read_frontend_key(u8(user), fe::kFallbackSeenKey) == "fallback-translocated");
    CHECK_FALSE(fe::post_data_fallback_notice(trans)); // seen
    CHECK(fe::take_data_notices().empty());
    // Another reason shows once too; the line is replaced, not repeated.
    const fe::DataLocation ro = user_at(user, base, Reason::kFallbackNotWritable, "Access denied");
    CHECK(fe::post_data_fallback_notice(ro));
    shown = fe::take_data_notices();
    REQUIRE(shown.size() == 1);
    CHECK(shown[0] == "MapleThief can't write next to itself in " + u8(base) +
                          " (Access denied), so your data is in " + u8(user) +
                          ". To keep it next to the game, move the game's folder somewhere you "
                          "can write, such as Documents or a Games folder.");
    const std::string text = read_text(user / "settings.ini");
    CHECK(text.find("data_fallback_seen = fallback-not-writable") != std::string::npos);
    CHECK(text.find("fallback-translocated") == std::string::npos);
    // settings.ini keeps the line through the settings' own reading and writing, quietly.
    fe::Settings s;
    CHECK(fe::parse_settings(text, &s).empty());
    const std::string again = fe::serialise_settings(s, text);
    CHECK(again.find("data_fallback_seen = fallback-not-writable") != std::string::npos);
}

TEST_CASE("data switch: the Saves tab with a long path, as a row dump and a drawing") {
    REQUIRE(SDL_Init(SDL_INIT_GAMEPAD));
    const std::string long_base = "/Users/a-player-with-a-long-name/Games/Downloads/Unpacked/"
                                  "MapleThief-1.0.0-rc.1-macos-universal";
    fe::DataLocation loc = fe::data_location_at(
        "/Users/a-player-with-a-long-name/Library/Application Support/MapleThief", Place::kUser,
        Reason::kFallbackNotWritable, long_base);
    loc.why = "Permission denied";
    Seam seam(loc);
    Menu m(inputs("/nowhere"));
    m.reach();
    const std::string dump = m.menu.dump();
    MESSAGE(dump);
    CHECK(dump ==
          "tab: Saves\n"
          "note: Data: /Users/a-player-with-...name/Library/Application Support/MapleThief\n"
          "  Save folder | \n"
          "  Backups per character | 10\n"
          "  Export saves... | Open...\n"
          "  Import saves... | Open...\n"
          "  Backups | List...\n"
          "> Data location | User folder\n"
          "  Open data folder | Open...\n"
          "  Open logs folder | Open...\n"
          "  Quit | not available\n"
          "help: Can't write next to the game (Permission denied). To keep your data\n"
          "help: there, move the game's folder somewhere you can write, such as Documents.\n");
    std::vector<std::uint8_t> px(480 * 320 * 4, 0);
    for (std::size_t i = 0; i < px.size(); i += 4) {
        px[i] = 40, px[i + 1] = 90, px[i + 2] = 130, px[i + 3] = 255; // a plain screen, no art
    }
    m.menu.draw(px.data(), 480, 320, 1);
    const fs::path out = fs::path(output_dir()) / "settings_menu";
    std::error_code ec;
    fs::create_directories(out, ec);
    SDL_Surface *s = SDL_CreateSurfaceFrom(480, 320, SDL_PIXELFORMAT_ABGR8888, px.data(), 480 * 4);
    CHECK((s != nullptr && SDL_SavePNG(s, (out / "saves_data_location.png").string().c_str())));
    SDL_DestroySurface(s);
    char hex[32];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(fnv1a(px)));
    MESSAGE("saves_data_location hash " << hex);
    CHECK(fnv1a(px) == 0xad2550a9707117bbull);
    SDL_Quit();
}
