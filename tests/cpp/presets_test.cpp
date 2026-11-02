// The settings registry and the presets (#144, ADR 0070): the table-driven check that each
// preset sets exactly its column (and nothing outside the presets), that changing one key makes
// it custom, that Assists stay off in every column (check_registry, also on bad synthetic
// tables), the preset line in settings.ini (precedence over the keys it controls, custom, the
// rewrite), what "no settings file" means, the registry's menu helpers, and the first-launch
// picker driven by synthetic key, pad, mouse and touch input, with its drawing checked by hash.
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "canvas.h"
#include "game.h"
#include "preset_picker.h"
#include "settings.h"

namespace {

namespace fe = maplethief::frontend;
namespace fs = std::filesystem;
using fe::Preset;
using fe::SettingGroup;
using fe::SettingInfo;
using fe::Settings;

// #89's table for today's keys (Original, Original + controller, Enhanced), written out here
// independently of the registry: the registry must match it. nullptr: not in the presets.
struct Expected {
    const char *key;
    const char *columns[3];
};
const Expected kExpected[] = {
    {"scale_mode", {"integer", "integer", "integer"}},
    {"border_color", {nullptr, nullptr, nullptr}},
    {"render_scale", {"1", "1", "auto"}},
    {"gfx_backend", {"software", "software", "software"}},
    {"filter", {"none", "none", "none"}},
    {"hd_text", {"off", "off", "on"}},
    {"master_volume", {nullptr, nullptr, nullptr}},
    {"music_volume", {nullptr, nullptr, nullptr}},
    {"effects_volume", {nullptr, nullptr, nullptr}},
    {"mute_unfocused", {"on", "on", "on"}},
    {"gapless_music", {"off", "off", "on"}},
    {"right_touch", {nullptr, nullptr, nullptr}},
    {"hide_keypad", {"never", "auto", "auto"}},
    {"rumble", {"on", "on", "on"}},
    {"rumble_strength", {nullptr, nullptr, nullptr}},
    {"perf_overlay", {"off", "off", "off"}},
    {"cash_shop", {"off", "off", "off"}},
    {"pacing", {"display", "display", "display"}},
    {"save_dir", {nullptr, nullptr, nullptr}},
    {"save_backups", {nullptr, nullptr, nullptr}},
    {"fullscreen", {"off", "off", "desktop"}},
    {"vsync", {"on", "on", "on"}},
    {"pause_unfocused", {"off", "on", "on"}},
    {"language", {nullptr, nullptr, nullptr}},
    {"fix_original_bugs", {"off", "off", "on"}},
    {"texture_pack", {nullptr, nullptr, nullptr}},
    {"texture_filter", {"game", "game", "nearest"}},
    {"hide_online_ui", {"off", "on", "on"}},
    {"local_game_center", {"off", "off", "on"}},
    {"menu_repeat", {"off", "on", "on"}},
    {"confirm_guard", {"off", "on", "on"}},
    {"focus_highlight", {"on", "on", "on"}},
    {"button_prompts", {"off", "on", "on"}},
    {"keypad_layout", {nullptr, nullptr, nullptr}},
    {"border", {"color", "color", "blur"}},
    {"border_image", {nullptr, nullptr, nullptr}},
    {"game_speed", {"game", "game", "game"}},
    {"fast_forward", {"off", "off", "off"}},
};
const Preset kColumns[] = {Preset::kOriginal, Preset::kOriginalController, Preset::kEnhanced};

// A value of the row other than `not_this` (the next of its choices; for a colour or a custom
// value one written here).
std::string other_value(const SettingInfo &info, const std::string &not_this) {
    for (const std::string &v : fe::setting_choices(info)) {
        if (v != not_this) {
            return v;
        }
    }
    if (info.value.kind == fe::ValueKind::kColor) {
        return not_this == "#123456" ? "#654321" : "#123456";
    }
    if (std::string_view(info.key) == "save_dir") {
        return not_this == "/saves/a" ? "/saves/b" : "/saves/a";
    }
    if (std::string(info.key) == "keypad_layout") {
        return not_this == "original" ? "dpad=80,10,-10,60" : "original";
    }
    return not_this == "10,20" ? "30,40" : "10,20"; // right_touch
}

// Settings with every row moved away from its default, so a preset has something to change.
Settings scrambled() {
    Settings s;
    for (const SettingInfo &info : fe::settings_registry()) {
        REQUIRE(info.value.parse(info.value, other_value(info, fe::setting_text(s, info)), &s));
    }
    return s;
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

SDL_Event key_down(SDL_Keycode k, bool repeat = false) {
    SDL_Event e{};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = k;
    e.key.down = true;
    e.key.repeat = repeat;
    return e;
}
SDL_Event pad_button(SDL_GamepadButton b) {
    SDL_Event e{};
    e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    e.gbutton.button = static_cast<Uint8>(b);
    e.gbutton.down = true;
    return e;
}
SDL_Event stick_y(Sint16 v) {
    SDL_Event e{};
    e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    e.gaxis.axis = SDL_GAMEPAD_AXIS_LEFTY;
    e.gaxis.value = v;
    return e;
}
// The centre of the picker's line i, in screen points.
fe::ScreenPoint line_centre(std::size_t i) {
    return fe::ScreenPoint{
        static_cast<float>(fe::PresetPicker::kListX + fe::PresetPicker::kListW / 2),
        static_cast<float>(fe::PresetPicker::kListY +
                           static_cast<int>(i) * fe::PresetPicker::kRowH + 10),
        true};
}

} // namespace

TEST_CASE("presets: the registry matches #89's table; each preset sets exactly its column") {
    const auto rows = fe::settings_registry();
    REQUIRE(rows.size() == std::size(kExpected));
    for (std::size_t i = 0; i < rows.size(); i++) {
        CAPTURE(rows[i].key);
        CHECK(std::string(rows[i].key) == kExpected[i].key);
        for (std::size_t c = 0; c < 3; c++) {
            const char *got = rows[i].presets[kColumns[c]];
            const char *want = kExpected[i].columns[c];
            CHECK((got == nullptr) == (want == nullptr));
            if (got != nullptr && want != nullptr) {
                CHECK(std::string(got) == want);
            }
        }
        CHECK(rows[i].presets[Preset::kCustom] == nullptr);
    }
    // From the defaults and from settings with every key changed: each preset sets every key it
    // controls to its column, leaves every other key alone, and records itself.
    for (const Settings &start : {Settings{}, scrambled()}) {
        for (std::size_t c = 0; c < 3; c++) {
            CAPTURE(fe::preset_name(kColumns[c]));
            Settings s = start;
            fe::apply_preset(&s, kColumns[c]);
            REQUIRE(s.preset == kColumns[c]);
            for (const Expected &e : kExpected) {
                const SettingInfo *info = fe::find_setting(e.key);
                REQUIRE(info != nullptr);
                const std::string now = fe::setting_text(s, *info);
                if (e.columns[c] != nullptr) {
                    CHECK_MESSAGE(now == e.columns[c], e.key);
                } else {
                    CHECK_MESSAGE(now == fe::setting_text(start, *info), e.key);
                }
            }
            CHECK(fe::preset_differences(s, kColumns[c]).empty());
            CHECK(fe::matching_preset(s).has_value());
            // Through the file's key and the command line's path, the same.
            Settings t = start;
            std::string why;
            CHECK(fe::apply_setting(&t, "preset", fe::preset_name(kColumns[c]), &why));
            CHECK(t == s);
        }
        // Custom changes nothing but the preset.
        Settings s = start;
        fe::apply_preset(&s, Preset::kCustom);
        CHECK(s.preset == Preset::kCustom);
        s.preset = start.preset;
        CHECK(s == start);
    }
    // The three columns differ from each other (so the matching preset is the one applied).
    Settings o, oc, en;
    fe::apply_preset(&o, Preset::kOriginal);
    fe::apply_preset(&oc, Preset::kOriginalController);
    fe::apply_preset(&en, Preset::kEnhanced);
    CHECK(fe::matching_preset(o) == Preset::kOriginal);
    CHECK(fe::matching_preset(oc) == Preset::kOriginalController);
    CHECK(fe::matching_preset(en) == Preset::kEnhanced);
}

TEST_CASE("presets: changing one key it controls makes it custom; other keys do not") {
    for (const Preset p : kColumns) {
        for (const SettingInfo &info : fe::settings_registry()) {
            CAPTURE(fe::preset_name(p));
            CAPTURE(info.key);
            Settings s;
            fe::apply_preset(&s, p);
            const Settings before = s;
            const std::string now = fe::setting_text(s, info);
            // The same value again: nothing changes.
            std::string why;
            CHECK(fe::apply_setting(&s, info.key, now, &why));
            CHECK(s == before);
            // Another value.
            const std::string other = other_value(info, now);
            REQUIRE(fe::apply_setting(&s, info.key, other, &why));
            CHECK(fe::setting_text(s, info) == other);
            CHECK(s.preset == (info.presets.in_presets() ? Preset::kCustom : p));
            // Every other key kept its value.
            Settings back = s;
            REQUIRE(info.value.parse(info.value, now, &back));
            back.preset = before.preset;
            CHECK(back == before);
            // A bad value changes nothing, the preset included.
            Settings again = before;
            CHECK_FALSE(fe::apply_setting(&again, info.key, "\x01not a value", &why));
            CHECK(again == before);
        }
        // The menu's step (#89) goes the same way.
        Settings s;
        fe::apply_preset(&s, p);
        REQUIRE(fe::step_setting(&s, *fe::find_setting("gapless_music"), 1));
        CHECK(s.preset == Preset::kCustom);
        fe::apply_preset(&s, p);
        REQUIRE(fe::step_setting(&s, *fe::find_setting("music_volume"), -1));
        CHECK(s.music_volume == 95);
        CHECK(s.preset == p);
    }
    // Custom stays custom; no preset stays none.
    Settings s;
    s.preset = Preset::kCustom;
    CHECK(fe::apply_setting(&s, "hd_text", "on", nullptr));
    CHECK(s.preset == Preset::kCustom);
    Settings none;
    CHECK(fe::apply_setting(&none, "hd_text", "on", nullptr));
    CHECK_FALSE(none.preset.has_value());
    // The command line's order: --preset first, then a flag that differs: custom with the flag.
    Settings cli;
    fe::apply_preset(&cli, Preset::kEnhanced);
    CHECK(fe::apply_setting(&cli, "hide_keypad", "never", nullptr));
    CHECK(cli.preset == Preset::kCustom);
    CHECK(cli.hide_keypad == fe::KeypadMode::kNever);
    CHECK(cli.hd_text);
}

TEST_CASE("presets: Assists are off in every column; check_registry catches a bad row") {
    // The registry itself: no problem (the static_assert in settings.cpp checks the same shape).
    const std::vector<std::string> problems = fe::check_registry(fe::settings_registry());
    for (const std::string &p : problems) {
        MESSAGE(p);
    }
    CHECK(problems.empty());
    const Settings defaults;
    for (const SettingInfo &info : fe::settings_registry()) {
        if (info.group == SettingGroup::kAssists) {
            CAPTURE(info.key);
            REQUIRE(info.presets.in_presets());
            for (const Preset p : kColumns) {
                CHECK(fe::setting_text(defaults, info) == info.presets[p]);
            }
        }
    }
    // Synthetic tables (bound to existing fields: the check is about the table's shape).
    const SettingInfo good_assist = {.key = "turbo_attack",
                                     .group = SettingGroup::kAssists,
                                     .comment = "# attack while the button is held: on | off",
                                     .value = fe::on_off<&Settings::hd_text>(),
                                     .presets = {"off", "off", "off"},
                                     .flag = "--turbo-attack",
                                     .label = "Turbo attack",
                                     .help = "Attacks again while the button is held"};
    CHECK(fe::check_registry(std::span(&good_assist, 1)).empty());
    SettingInfo on_in_enhanced = good_assist;
    on_in_enhanced.presets = {"off", "off", "on"};
    std::vector<std::string> bad = fe::check_registry(std::span(&on_in_enhanced, 1));
    REQUIRE(bad.size() == 1);
    CHECK(bad[0] == "row 0 (turbo_attack): an assist that is 'on' in enhanced, not its neutral "
                    "'off'");
    SettingInfo outside = good_assist;
    outside.presets = fe::kNotInPresets;
    bad = fe::check_registry(std::span(&outside, 1));
    REQUIRE(bad.size() == 1);
    CHECK(bad[0].find("an assist outside the presets") != std::string::npos);
    // Other mistakes a new row can make.
    SettingInfo rows[3] = {good_assist, good_assist, good_assist};
    rows[1].group = SettingGroup::kGameplay;
    rows[1].flag = "--turbo";                       // not the key's flag, and
    rows[1].presets = {"off", "maybe", "on"};       // not a value
    rows[2].key = "Fast_Text";                      // not lower case
    rows[2].comment = "# fast text\nskips nothing"; // a comment line without '#'
    rows[2].presets = {"off", nullptr, nullptr};    // a partial column
    rows[2].label = "";                             // no label
    bad = fe::check_registry(rows);
    std::string all;
    for (const std::string &p : bad) {
        all += p + "\n";
    }
    INFO(all);
    CHECK(all.find("row 1 (turbo_attack): the key again (row 0)") != std::string::npos);
    CHECK(all.find("row 1 (turbo_attack): the flag is not --turbo-attack") != std::string::npos);
    CHECK(all.find("row 1 (turbo_attack): the original_controller column 'maybe' is not a value") !=
          std::string::npos);
    CHECK(all.find("row 2 (Fast_Text): not a lower-case key") != std::string::npos);
    CHECK(all.find("row 2 (Fast_Text): a comment line that does not start with '#'") !=
          std::string::npos);
    CHECK(all.find("row 2 (Fast_Text): no menu label or help") != std::string::npos);
    CHECK(all.find("row 2 (Fast_Text): a column for some presets only") != std::string::npos);
    // A colour row that offers the menu (#89) a value the file would not write as it is.
    static constexpr const char *kOffered[] = {"#000000", "#ffffff", "black"};
    const SettingInfo colour = {.key = "border_color",
                                .group = SettingGroup::kVideo,
                                .comment = "# the bars",
                                .value = fe::color<&Settings::border_color>(kOffered),
                                .flag = "--border-color",
                                .label = "Border colour",
                                .help = "The bars"};
    bad = fe::check_registry(std::span(&colour, 1));
    REQUIRE(bad.size() == 2);
    CHECK(bad[0] == "row 0 (border_color): an offered value '#ffffff' that is not a value as the "
                    "file writes it");
    CHECK(bad[1] == "row 0 (border_color): an offered value 'black' that is not a value as the "
                    "file writes it");
}

TEST_CASE("presets: no settings file is today's defaults: Original but for hide_keypad") {
    // ADR 0070: a run without a settings file keeps the defaults it always had. They are the
    // Original column except hide_keypad (auto since #71; Original keeps the key pad shown).
    // Since #145 they are no longer the Original + controller column: the keys below are on
    // there and off without a file, as the game always was (pause_unfocused, ADR 0077: a run
    // without a file has never paused when the window lost focus; hide_online_ui, ADR 0084;
    // menu_repeat and confirm_guard, #147; button_prompts, #146).
    const Settings defaults;
    CHECK_FALSE(defaults.preset.has_value());
    const std::vector<const SettingInfo *> diff =
        fe::preset_differences(defaults, Preset::kOriginal);
    REQUIRE(diff.size() == 1);
    CHECK(std::string(diff[0]->key) == "hide_keypad");
    std::vector<std::string> controller_keys;
    for (const SettingInfo *info : fe::preset_differences(defaults, Preset::kOriginalController)) {
        controller_keys.emplace_back(info->key);
    }
    CHECK(controller_keys == std::vector<std::string>{"pause_unfocused", "hide_online_ui",
                                                      "menu_repeat", "confirm_guard",
                                                      "button_prompts"});
    CHECK_FALSE(fe::matching_preset(defaults).has_value());
    // #145's keys without a file: windowed, vsync on, no pause when unfocused, English.
    CHECK(defaults.fullscreen == fe::Fullscreen::kOff);
    CHECK(defaults.vsync);
    CHECK_FALSE(defaults.pause_unfocused);
    CHECK(defaults.language == fe::Language::kEn);
    // An empty file, or one without a preset line, leaves the preset empty (the picker's cue).
    Settings s;
    CHECK(fe::parse_settings("", &s).empty());
    CHECK(fe::parse_settings("# only a comment\nhd_text = on\n", &s).empty());
    CHECK_FALSE(s.preset.has_value());
    CHECK(s.hd_text);
}

TEST_CASE("presets: the preset line in settings.ini") {
    // A preset sets its column, wherever its line is; a line it disagrees with is reported and
    // ignored, a line that agrees is not.
    Settings s;
    std::vector<std::string> problems = fe::parse_settings("hd_text = off\n"
                                                           "master_volume = 40\n"
                                                           "PRESET = Enhanced\n"
                                                           "gapless_music = on\n"
                                                           "hide_keypad = never\n",
                                                           &s);
    REQUIRE(problems.size() == 2);
    CHECK(problems[0] == "line 1: hd_text = off: preset = enhanced sets on, ignored (preset = "
                         "custom keeps the file's values)");
    CHECK(problems[1] == "line 5: hide_keypad = never: preset = enhanced sets auto, ignored "
                         "(preset = custom keeps the file's values)");
    Settings want;
    fe::apply_preset(&want, Preset::kEnhanced);
    want.master_volume = 40; // outside the presets: the file's
    CHECK(s == want);
    // custom: every line counts.
    s = {};
    problems = fe::parse_settings("preset = custom\nhd_text = on\nhide_keypad = never\n", &s);
    CHECK(problems.empty());
    CHECK(s.preset == Preset::kCustom);
    CHECK(s.hd_text);
    CHECK(s.hide_keypad == fe::KeypadMode::kNever);
    // The last good preset line counts; a bad one is reported; a repeat is noted.
    s = {};
    problems = fe::parse_settings("preset = enhanced\npreset = classic\npreset = original\n", &s);
    REQUIRE(problems.size() == 2);
    CHECK(problems[0] == "line 2: preset: 'classic' is not original, original_controller, "
                         "enhanced or custom, ignored");
    CHECK(problems[1] == "line 3: preset again (line 1 set it too): this one counts");
    CHECK(s.preset == Preset::kOriginal);
    CHECK(s.hide_keypad == fe::KeypadMode::kNever);
    // Only bad preset lines: no preset, the keys as written.
    s = {};
    problems = fe::parse_settings("preset = \nhd_text = on\n", &s);
    CHECK(problems.size() == 1);
    CHECK_FALSE(s.preset.has_value());
    CHECK(s.hd_text);

    // Writing: a new file starts with the preset; a file of the port's parses with no problem.
    for (const Preset p : fe::kPresets) {
        CAPTURE(fe::preset_name(p));
        Settings w = scrambled();
        fe::apply_preset(&w, p);
        const std::string text = fe::serialise_settings(w);
        CHECK(text.find(std::string("# controls (a line below that disagrees is ignored); custom "
                                    "keeps every value below\npreset = ") +
                        fe::preset_name(p) + "\n# integer") != std::string::npos);
        Settings back;
        CHECK(fe::parse_settings(text, &back).empty());
        CHECK(back == w);
    }
    // No preset: no preset line (a file as before #144).
    CHECK(fe::serialise_settings(Settings{}).find("preset") == std::string::npos);
    // An older file gets the preset appended, its own lines kept byte for byte.
    Settings w;
    w.master_volume = 30;
    const std::string old = "# mine\r\nmaster_volume = 30\r\nunknown = 1\r\n";
    const std::string before = fe::serialise_settings(w, old);
    fe::apply_preset(&w, Preset::kOriginal);
    const std::string after = fe::serialise_settings(w, old);
    CHECK(after.rfind(old, 0) == 0);
    CHECK(after.find("\r\npreset = original\r\n") != std::string::npos);
    CHECK(after.find("hide_keypad = never\r\n") != std::string::npos);
    CHECK(before.find("preset") == std::string::npos);
    Settings back;
    fe::parse_settings(after, &back);
    CHECK(back == w);
    // Rewriting a preset file over itself changes nothing; a change of preset rewrites its line
    // and the keys it controls in place.
    CHECK(fe::serialise_settings(w, after) == after);
    fe::apply_preset(&w, Preset::kEnhanced);
    const std::string switched = fe::serialise_settings(w, after);
    // Each line the presets control changes by its two columns' lengths ("never" -> "auto",
    // "off" -> "on", "off" -> "desktop", ...), the preset line by its names'.
    long delta = static_cast<long>(std::string_view(fe::preset_name(Preset::kEnhanced)).size()) -
                 static_cast<long>(std::string_view(fe::preset_name(Preset::kOriginal)).size());
    for (const SettingInfo &info : fe::settings_registry()) {
        if (info.presets.in_presets()) {
            delta += static_cast<long>(std::string_view(info.presets.enhanced).size()) -
                     static_cast<long>(std::string_view(info.presets.original).size());
        }
    }
    CHECK(static_cast<long>(switched.size()) == static_cast<long>(after.size()) + delta);
    back = {};
    problems = fe::parse_settings(switched, &back);
    REQUIRE(problems.size() == 1);
    CHECK(problems[0] == "line 3: unknown key 'unknown', ignored");
    CHECK(back == w);
    // A preset line while no preset is set would set one: it becomes a comment.
    Settings none;
    const std::string commented = fe::serialise_settings(none, "preset = enhanced\n");
    CHECK(commented.rfind("# preset = enhanced\n", 0) == 0);
    back = {};
    fe::parse_settings(commented, &back);
    CHECK(back == none);
}

TEST_CASE("presets: the registry's names, flags and menu helpers") {
    for (int i = 0; i < 4; i++) {
        CHECK(std::string(fe::scale_mode_name(static_cast<fe::ScaleMode>(i))) ==
              fe::kScaleModeNames[i]);
        CHECK(std::string(fe::shader_filter_name(static_cast<fe::ShaderFilter>(i))) ==
              fe::kShaderFilterNames[i]);
        CHECK(std::string(fe::preset_name(static_cast<Preset>(i))) == fe::kPresetNames[i]);
    }
    for (int i = 0; i < 3; i++) {
        CHECK(std::string(fe::keypad_mode_name(static_cast<fe::KeypadMode>(i))) ==
              fe::kKeypadModeNames[i]);
    }
    for (int i = 0; i < 2; i++) {
        CHECK(std::string(fe::gfx_backend_name(static_cast<fe::GfxBackend>(i))) ==
              fe::kGfxBackendNames[i]);
    }
    Preset p{};
    CHECK(fe::parse_preset("Original_Controller", &p));
    CHECK(p == Preset::kOriginalController);
    CHECK_FALSE(fe::parse_preset("original controller", &p));
    // Every row: a flag (the key's), a label, help, a group name; its file line round trips.
    for (const SettingInfo &info : fe::settings_registry()) {
        CAPTURE(info.key);
        REQUIRE(info.flag != nullptr);
        CHECK(fe::find_setting_flag(info.flag) == &info);
        CHECK(fe::find_setting(info.key) == &info);
        CHECK(std::string(fe::setting_group_name(info.group)) != "");
        CHECK(fe::preset_summary(Preset::kOriginal) != nullptr);
    }
    CHECK(fe::find_setting("HD_TEXT") == fe::find_setting("hd_text"));
    CHECK(fe::find_setting("preset") == nullptr);
    CHECK(fe::find_setting_flag("--preset") == nullptr);
    CHECK(fe::find_setting_flag("--hd_text") == nullptr);
    CHECK(fe::find_setting_flag("--hd-text")->game_only);
    CHECK_FALSE(fe::find_setting_flag("--gfx-backend")->game_only);
    // What the menu shows and steps through.
    const SettingInfo &scale = *fe::find_setting("scale_mode");
    CHECK(fe::setting_expects(scale) == "integer, fit, smooth or sharp");
    CHECK(fe::setting_choices(scale) ==
          std::vector<std::string>{"integer", "fit", "smooth", "sharp"});
    Settings s;
    CHECK(fe::step_setting(&s, scale, -1)); // wraps
    CHECK(s.scale_mode == fe::ScaleMode::kSharp);
    CHECK(fe::step_setting(&s, scale, 1));
    CHECK(s.scale_mode == fe::ScaleMode::kInteger);
    const SettingInfo &render = *fe::find_setting("render_scale");
    CHECK(fe::setting_expects(render) == "auto or an integer from 1 to 8");
    CHECK(fe::setting_choices(render) ==
          std::vector<std::string>{"auto", "1", "2", "3", "4", "5", "6", "7", "8"});
    CHECK(fe::step_setting(&s, render, -1)); // auto comes before 1 (#152)
    CHECK(s.render_scale == fe::kRenderScaleAuto);
    CHECK(fe::step_setting(&s, render, -1)); // clamped
    CHECK(s.render_scale == fe::kRenderScaleAuto);
    CHECK(fe::step_setting(&s, render, 1));
    CHECK(s.render_scale == 1);
    CHECK(fe::step_setting(&s, render, 20));
    CHECK(s.render_scale == 8);
    CHECK(fe::step_setting(&s, render, 1)); // clamped
    CHECK(s.render_scale == 8);
    const SettingInfo &volume = *fe::find_setting("master_volume");
    CHECK(fe::setting_choices(volume).size() == 21); // 0, 5, ..., 100
    s.master_volume = 42;
    CHECK(fe::step_setting(&s, volume, 1));
    CHECK(s.master_volume == 47);
    // A colour and a custom value step through what they offer (#89); a value of their own
    // steps to the first (+1) or the last (-1) of them.
    const SettingInfo &border = *fe::find_setting("border_color");
    CHECK(fe::setting_expects(border) == "#RRGGBB");
    CHECK(fe::setting_choices(border) ==
          std::vector<std::string>{"#000000", "#202020", "#404040", "#808080", "#FFFFFF"});
    CHECK(fe::step_setting(&s, border, 1));
    CHECK(fe::setting_text(s, border) == "#202020");
    s.border_color = fe::Color{0x12, 0x34, 0x56};
    CHECK(fe::step_setting(&s, border, -1));
    CHECK(fe::setting_text(s, border) == "#FFFFFF");
    s.border_color = fe::Color{0x12, 0x34, 0x56};
    CHECK(fe::step_setting(&s, border, 1));
    CHECK(fe::setting_text(s, border) == "#000000");
    const SettingInfo &right = *fe::find_setting("right_touch");
    CHECK(fe::setting_choices(right) == std::vector<std::string>{"off", "440,290", "370,290"});
    CHECK(fe::step_setting(&s, right, 1));
    CHECK(fe::setting_text(s, right) == "440,290");
    CHECK(fe::step_setting(&s, right, -2)); // wraps
    CHECK(fe::setting_text(s, right) == "370,290");
    CHECK(fe::setting_expects(*fe::find_setting("right_touch")) ==
          "off or X,Y (screen points, 0-479, 0-319)");
    CHECK(std::string(fe::setting_group_name(SettingGroup::kAssists)) == "Assists");
}

// --- The first-launch picker ------------------------------------------------------------------

TEST_CASE("preset picker: shown only in an interactive game run without a preset") {
    Settings none;
    Settings chosen;
    chosen.preset = Preset::kEnhanced;
    const fe::GameOptions plain;
    CHECK(fe::wants_preset_picker(true, none, plain, "cocoa"));
    CHECK(fe::wants_preset_picker(true, none, plain, nullptr));
    CHECK_FALSE(fe::wants_preset_picker(false, none, plain, "cocoa"));  // the placeholder
    CHECK_FALSE(fe::wants_preset_picker(true, chosen, plain, "cocoa")); // file or --preset
    CHECK_FALSE(fe::wants_preset_picker(true, none, plain, "dummy"));
    CHECK_FALSE(fe::wants_preset_picker(true, none, plain, "offscreen"));
    // Every scripted run.
    std::vector<fe::GameOptions> scripted(11);
    scripted[0].frames = 30;
    scripted[1].seconds = 5;
    scripted[2].capture_dir = "out";
    scripted[3].capture_at = {10};
    scripted[4].shots = {fe::Shot{}};
    scripted[5].taps = {fe::Tap{}};
    scripted[6].fixed_step_us = 1000;
    scripted[7].audio_capture = "a.wav";
    scripted[8].suspends = {fe::Suspend{}};
    scripted[9].quit_at = 3;
    scripted[10].quit_at = 0;
    for (std::size_t i = 0; i < scripted.size(); i++) {
        CAPTURE(i);
        CHECK_FALSE(fe::wants_preset_picker(true, none, scripted[i], "cocoa"));
    }
}

TEST_CASE("preset picker: keys, the pad, the mouse and touch choose a line") {
    // No settings file: Original selected first, three lines.
    {
        fe::PresetPicker picker(false);
        REQUIRE(picker.lines() == std::vector<Preset>{Preset::kOriginal,
                                                      Preset::kOriginalController,
                                                      Preset::kEnhanced});
        CHECK(picker.selected() == 0);
        CHECK(picker.handle(key_down(SDLK_DOWN)));
        CHECK(picker.handle(key_down(SDLK_DOWN, true))); // the cursor repeats
        CHECK(picker.selected() == 2);
        CHECK(picker.handle(key_down(SDLK_DOWN)));
        CHECK(picker.selected() == 0); // wraps
        CHECK(picker.handle(key_down(SDLK_UP)));
        CHECK(picker.selected() == 2);
        CHECK(picker.handle(key_down(SDLK_RETURN, true))); // a repeat chooses nothing
        CHECK_FALSE(picker.chosen().has_value());
        CHECK(picker.handle(key_down(SDLK_RETURN)));
        CHECK(picker.chosen() == Preset::kEnhanced);
        // Chosen once: nothing changes it.
        CHECK(picker.handle(key_down(SDLK_1)));
        CHECK(picker.chosen() == Preset::kEnhanced);
    }
    // A number key chooses its line.
    {
        fe::PresetPicker picker(false);
        picker.handle(key_down(SDLK_2));
        CHECK(picker.chosen() == Preset::kOriginalController);
        fe::PresetPicker none(false);
        none.handle(key_down(SDLK_4)); // no fourth line
        CHECK_FALSE(none.chosen().has_value());
    }
    // A settings file from before the presets: "Keep my settings" first, and selected.
    {
        fe::PresetPicker picker(true);
        REQUIRE(picker.lines().size() == 4);
        CHECK(picker.lines()[0] == Preset::kCustom);
        CHECK(picker.selected() == 0);
        picker.handle(key_down(SDLK_SPACE));
        CHECK(picker.chosen() == Preset::kCustom);
    }
    // The pad: the d-pad, the left stick (an edge with a margin), South.
    {
        fe::PresetPicker picker(false);
        CHECK(picker.handle(pad_button(SDL_GAMEPAD_BUTTON_DPAD_DOWN)));
        CHECK(picker.selected() == 1);
        picker.handle(stick_y(30000));
        picker.handle(stick_y(31000)); // held: one step
        CHECK(picker.selected() == 2);
        picker.handle(stick_y(0));
        picker.handle(stick_y(-30000));
        CHECK(picker.selected() == 1);
        CHECK(picker.handle(pad_button(SDL_GAMEPAD_BUTTON_EAST)));
        CHECK_FALSE(picker.chosen().has_value());
        picker.handle(pad_button(SDL_GAMEPAD_BUTTON_SOUTH));
        CHECK(picker.chosen() == Preset::kOriginalController);
    }
    {
        fe::PresetPicker picker(false);
        picker.handle(pad_button(SDL_GAMEPAD_BUTTON_START));
        CHECK(picker.chosen() == Preset::kOriginal);
    }
    // The mouse: hovering selects, a press and release on the same line chooses; a release on
    // another line, the gap between lines or the bars does not.
    {
        fe::PresetPicker picker(false);
        CHECK(picker.line_at(line_centre(1).x, line_centre(1).y) == 1);
        CHECK(picker.line_at(line_centre(1).x,
                             fe::PresetPicker::kListY + 2 * fe::PresetPicker::kRowH - 2) == -1);
        CHECK(picker.line_at(10, line_centre(0).y) == -1);
        CHECK(picker.line_at(line_centre(0).x, line_centre(3).y) == -1);
        picker.pointer(line_centre(2), false, false);
        CHECK(picker.selected() == 2);
        picker.pointer(line_centre(1), true, false);
        picker.pointer(line_centre(0), false, true);
        CHECK_FALSE(picker.chosen().has_value());
        fe::ScreenPoint bars = line_centre(1);
        bars.inside = false;
        picker.pointer(line_centre(1), true, false);
        picker.pointer(bars, false, true);
        CHECK_FALSE(picker.chosen().has_value());
        picker.pointer(line_centre(1), true, false);
        picker.pointer(line_centre(1), false, true);
        CHECK(picker.chosen() == Preset::kOriginalController);
    }
    // A finger: the same as the mouse (run_preset_picker maps both to screen points).
    {
        fe::PresetPicker picker(true);
        picker.pointer(line_centre(3), true, false);
        picker.pointer(line_centre(3), false, true);
        CHECK(picker.chosen() == Preset::kEnhanced);
    }
    // Mouse and finger events are not handle()'s; window events neither.
    {
        fe::PresetPicker picker(false);
        SDL_Event e{};
        e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        CHECK_FALSE(picker.handle(e));
        e.type = SDL_EVENT_WINDOW_RESIZED;
        CHECK_FALSE(picker.handle(e));
    }
}

TEST_CASE("preset picker: the drawing, by hash; every line fits") {
    REQUIRE(SDL_Init(SDL_INIT_GAMEPAD));
    const fs::path out = fs::path(output_dir()) / "preset_picker";
    std::error_code ec;
    fs::create_directories(out, ec);
    // Each summary fits its line at the summary's place.
    for (const Preset p : fe::kPresets) {
        CHECK(fe::Canvas::text_width(fe::preset_summary(p)) <= fe::PresetPicker::kListW - 38);
    }
    struct Case {
        bool keep;
        int downs;
        const char *name;
        std::uint64_t hash;
    };
    // The picker is the port's own drawing (integer arithmetic only), the same bytes everywhere.
    const Case cases[] = {
        {false, 0, "first_launch", 0x549f3447437997b9ull},
        {true, 2, "keep_settings", 0x6fdbd8828b9bb18aull},
    };
    for (const Case &k : cases) {
        CAPTURE(k.name);
        fe::PresetPicker picker(k.keep);
        for (int i = 0; i < k.downs; i++) {
            picker.handle(key_down(SDLK_DOWN));
        }
        CHECK(picker.dirty());
        std::vector<std::uint8_t> px(std::size_t{480} * 320 * 4, 0x55);
        picker.draw(px.data(), 480, 320, 1);
        CHECK_FALSE(picker.dirty());
        const std::uint64_t h = fnv1a(px);
        char hex[32];
        std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(h));
        SDL_Surface *s =
            SDL_CreateSurfaceFrom(480, 320, SDL_PIXELFORMAT_ABGR8888, px.data(), 480 * 4);
        const fs::path png = out / (std::string(k.name) + ".png");
        CHECK((s != nullptr && SDL_SavePNG(s, png.string().c_str())));
        SDL_DestroySurface(s);
        MESSAGE(k.name << ": " << png.string() << " hash " << hex);
        CHECK(h == k.hash);
        // At the picker's own scale (2): the same layout, finer shapes.
        std::vector<std::uint8_t> px2(std::size_t{960} * 640 * 4, 0x55);
        picker.draw(px2.data(), 960, 640, 2);
        SDL_Surface *s2 =
            SDL_CreateSurfaceFrom(960, 640, SDL_PIXELFORMAT_ABGR8888, px2.data(), 960 * 4);
        CHECK((s2 != nullptr &&
               SDL_SavePNG(s2, (out / (std::string(k.name) + "_2x.png")).string().c_str())));
        SDL_DestroySurface(s2);
    }
    SDL_Quit();
}

TEST_CASE("preset picker: the window loop takes keys, a click, a tap, the pad and a close") {
    // A dummy-driver window at 960 x 640 points: integer 2x, so screen point (x, y) is window
    // point (2x, 2y). Events pushed before the loop runs are what the player does.
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    REQUIRE_MESSAGE(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD), SDL_GetError());
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    REQUIRE_MESSAGE(SDL_CreateWindowAndRenderer("presets_test", 960, 640, 0, &window, &renderer),
                    SDL_GetError());
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    const Settings settings;
    bool quit = true;

    // The keyboard: Down, Down, Return.
    for (const SDL_Keycode k : {SDLK_DOWN, SDLK_DOWN, SDLK_RETURN}) {
        SDL_Event e = key_down(k);
        REQUIRE(SDL_PushEvent(&e));
    }
    CHECK(fe::run_preset_picker(window, renderer, settings, false, &quit) == Preset::kEnhanced);
    CHECK_FALSE(quit);

    // The mouse: a click on the second line.
    const fe::ScreenPoint second = line_centre(1);
    for (const Uint32 type :
         {SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
        SDL_Event e{};
        e.type = type;
        if (type == SDL_EVENT_MOUSE_MOTION) {
            e.motion.x = second.x * 2;
            e.motion.y = second.y * 2;
            e.motion.which = 1;
        } else {
            e.button.button = SDL_BUTTON_LEFT;
            e.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            e.button.x = second.x * 2;
            e.button.y = second.y * 2;
            e.button.which = 1;
        }
        REQUIRE(SDL_PushEvent(&e));
    }
    CHECK(fe::run_preset_picker(window, renderer, settings, false, &quit) ==
          Preset::kOriginalController);

    // A finger on a touch screen: a tap on the first line ("Keep my settings" with a file).
    const fe::ScreenPoint first = line_centre(0);
    for (const Uint32 type : {SDL_EVENT_FINGER_DOWN, SDL_EVENT_FINGER_UP}) {
        SDL_Event e{};
        e.type = type;
        e.tfinger.touchID = 77;
        e.tfinger.fingerID = 1;
        e.tfinger.x = first.x * 2 / 960.0f;
        e.tfinger.y = first.y * 2 / 640.0f;
        REQUIRE(SDL_PushEvent(&e));
    }
    CHECK(fe::run_preset_picker(window, renderer, settings, true, &quit) == Preset::kCustom);

    // The pad's South.
    {
        SDL_Event e = pad_button(SDL_GAMEPAD_BUTTON_SOUTH);
        REQUIRE(SDL_PushEvent(&e));
    }
    CHECK(fe::run_preset_picker(window, renderer, settings, false, &quit) == Preset::kOriginal);

    // Closing the window: nothing chosen, quit.
    {
        SDL_Event e{};
        e.type = SDL_EVENT_QUIT;
        REQUIRE(SDL_PushEvent(&e));
    }
    CHECK_FALSE(fe::run_preset_picker(window, renderer, settings, false, &quit).has_value());
    CHECK(quit);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}
