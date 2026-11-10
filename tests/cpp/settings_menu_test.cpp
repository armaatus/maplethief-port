// The settings menu (#89, ADR 0076): built from the registry, every row and action reachable with
// the keyboard, a gamepad, the mouse and a touch screen (the epic's exit criterion, walked over
// settings_registry() itself), the preset line, live changes (live_settings.h) and what waits
// for the next start, the action rows and the game options opener, the gear, the routing on a
// dummy-driver window with pushed SDL events (open, change, see it applied, written to
// settings.ini), scripted runs that can't open it, and the drawing by hash.
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "audio_out.h"
#include "canvas.h"
#include "desktop.h"
#include "focus.h"
#include "frame_stats.h"
#include "game.h"
#include "gamepad.h"
#include "gfx_backend.h"
#include "input.h"
#include "keypad_hide.h"
#include "keypad_layout.h"
#include "lifecycle.h"
#include "live_settings.h"
#include "maplethief/audio.h"
#include "maplethief/input.h"
#include "maplethief/uikit.h"
#include "perf_overlay.h"
#include "present.h"
#include "rumble.h"
#include "settings.h"
#include "settings_menu.h"
#include "virtual_pad.h"

namespace {

namespace fe = maplethief::frontend;
namespace fs = std::filesystem;
using fe::MenuActionInfo;
using fe::SettingGroup;
using fe::SettingInfo;
using fe::Settings;
using fe::SettingsMenu;
using Kind = SettingsMenu::ItemKind;

SDL_Event key_event(SDL_Keycode k, bool down = true, SDL_Keymod mod = 0) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.key = k;
    e.key.down = down;
    e.key.mod = mod;
    return e;
}
SDL_Event pad_event(SDL_GamepadButton b, bool down = true) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
    e.gbutton.button = static_cast<Uint8>(b);
    e.gbutton.down = down;
    return e;
}
SDL_Event stick_event(SDL_GamepadAxis a, Sint16 v) {
    SDL_Event e{};
    e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    e.gaxis.axis = static_cast<Uint8>(a);
    e.gaxis.value = v;
    return e;
}

// Screen points of the menu's parts.
fe::ScreenPoint at(float x, float y) { return fe::ScreenPoint{x, y, true}; }
fe::ScreenPoint tab_centre(const SettingsMenu &m, std::size_t i) {
    const auto [x, w] = m.tab_box(i);
    return at(static_cast<float>(x + w / 2),
              static_cast<float>(SettingsMenu::kTabsY + SettingsMenu::kTabH / 2));
}
float item_y(const SettingsMenu &m, std::size_t i) {
    if (i == 0) {
        return SettingsMenu::kPresetY + 6;
    }
    return static_cast<float>(SettingsMenu::kListY +
                              static_cast<int>(i - 1 - m.first_visible()) * SettingsMenu::kRowH +
                              SettingsMenu::kRowH / 2);
}
fe::ScreenPoint label_point(const SettingsMenu &m, std::size_t i) {
    return at(SettingsMenu::kListX + 30, item_y(m, i));
}
// On the value's "<" (-1), ">" (+1).
fe::ScreenPoint arrow_point(const SettingsMenu &m, std::size_t i, int side) {
    return at(
        static_cast<float>(side < 0 ? SettingsMenu::kValueX + 7 : SettingsMenu::kValueEnd - 7),
        item_y(m, i));
}
fe::ScreenPoint scroll_down_point() {
    return at(SettingsMenu::kScrollX + 10,
              SettingsMenu::kListY +
                  static_cast<float>((SettingsMenu::kVisibleRows - 1) * SettingsMenu::kRowH + 8));
}
fe::ScreenPoint close_point() { return at(SettingsMenu::kCloseX + 10, SettingsMenu::kCloseY + 9); }

// A dummy-driver window at 960 x 640 points: integer 2x with no bars, so screen point (x, y) is
// window point (2x, 2y). The devices' events go through the frontend's routing
// (menu_route_event), as game.cpp sends them.
struct Window {
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    fe::Placement placement = fe::place(fe::ScaleMode::kInteger, 960, 640, 480, 320);

    Window() {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
        REQUIRE_MESSAGE(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD), SDL_GetError());
        REQUIRE_MESSAGE(
            SDL_CreateWindowAndRenderer("settings_menu_test", 960, 640, 0, &window, &renderer),
            SDL_GetError());
        SDL_PumpEvents();
        SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    }
    ~Window() {
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
    }
};

SDL_Event mouse_event(Uint32 type, fe::ScreenPoint p) {
    SDL_Event e{};
    e.type = type;
    if (type == SDL_EVENT_MOUSE_MOTION) {
        e.motion.which = 1;
        e.motion.x = p.x * 2;
        e.motion.y = p.y * 2;
    } else {
        e.button.which = 1;
        e.button.button = SDL_BUTTON_LEFT;
        e.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        e.button.x = p.x * 2;
        e.button.y = p.y * 2;
    }
    return e;
}
SDL_Event finger_event(Uint32 type, fe::ScreenPoint p) {
    SDL_Event e{};
    e.type = type;
    e.tfinger.touchID = 77; // not a mouse's, not a trackpad's
    e.tfinger.fingerID = 1;
    e.tfinger.x = p.x * 2 / 960.0f;
    e.tfinger.y = p.y * 2 / 640.0f;
    return e;
}

enum class Device { kKeys, kPad, kMouse, kTouch };
const char *device_name(Device d) {
    switch (d) {
    case Device::kKeys:
        return "keyboard";
    case Device::kPad:
        return "gamepad";
    case Device::kMouse:
        return "mouse";
    case Device::kTouch:
        return "touch";
    }
    return "?";
}

// One device's input into the menu through the frontend's routing.
struct Driver {
    SettingsMenu &menu;
    Window *win; // null: straight to the menu's handle / pointer
    const fe::Rect *gear = nullptr;

    bool send(const SDL_Event &e) {
        if (win != nullptr) {
            return fe::menu_route_event(menu, e, win->window, win->placement, gear);
        }
        return menu.handle(e);
    }
    void key(SDL_Keycode k, SDL_Keymod mod = 0) {
        send(key_event(k, true, mod));
        send(key_event(k, false, mod));
    }
    void button(SDL_GamepadButton b) {
        send(pad_event(b, true));
        send(pad_event(b, false));
    }
    void tap(Device d, fe::ScreenPoint p) {
        if (d == Device::kMouse) {
            if (win != nullptr) {
                send(mouse_event(SDL_EVENT_MOUSE_MOTION, p));
                send(mouse_event(SDL_EVENT_MOUSE_BUTTON_DOWN, p));
                send(mouse_event(SDL_EVENT_MOUSE_BUTTON_UP, p));
            } else {
                menu.pointer(p, false, false);
                menu.pointer(p, true, false);
                menu.pointer(p, false, true);
            }
        } else {
            REQUIRE(win != nullptr);
            send(finger_event(SDL_EVENT_FINGER_DOWN, p));
            send(finger_event(SDL_EVENT_FINGER_UP, p));
        }
    }

    // Drives the open menu with device `d` until `want` is selected: the tab first, then the
    // row. False if it does not get there within the menu's own sizes.
    bool reach(Device d, SettingGroup group, const SettingInfo *row, const MenuActionInfo *action) {
        const auto &tabs = menu.tabs();
        const auto t = std::find(tabs.begin(), tabs.end(), group);
        if (t == tabs.end()) {
            return false;
        }
        const std::size_t tab = static_cast<std::size_t>(t - tabs.begin());
        for (std::size_t n = 0; menu.tab() != tab && n <= tabs.size(); n++) {
            switch (d) {
            case Device::kKeys:
                key(SDLK_TAB);
                break;
            case Device::kPad:
                button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
                break;
            default:
                tap(d, tab_centre(menu, tab));
                break;
            }
        }
        const auto selected_is_it = [&] {
            const SettingsMenu::Item &it = menu.items()[menu.selected()];
            return row != nullptr ? it.kind == Kind::kSetting && it.setting == row
                                  : it.kind == Kind::kAction && it.action == action;
        };
        for (std::size_t n = 0; !selected_is_it() && n <= 2 * menu.items().size(); n++) {
            switch (d) {
            case Device::kKeys:
                key(SDLK_DOWN);
                break;
            case Device::kPad:
                button(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
                break;
            default: {
                std::size_t i = 0;
                while (i < menu.items().size() &&
                       !(row != nullptr ? menu.items()[i].setting == row
                                        : menu.items()[i].action == action)) {
                    i++;
                }
                if (i == menu.items().size()) {
                    return false;
                }
                const bool visible = i - 1 >= menu.first_visible() &&
                                     i - 1 < menu.first_visible() + SettingsMenu::kVisibleRows;
                tap(d, visible ? label_point(menu, i) : scroll_down_point());
                break;
            }
            }
        }
        return menu.tab() == tab && selected_is_it();
    }
};

std::string read(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

struct TempDir {
    fs::path path;
    explicit TempDir(const char *name) {
        path = fs::temp_directory_path() / (std::string("mt_settings_menu_") + name);
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

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

// A synthetic screen behind the menu: bands of colour, no game art.
std::vector<std::uint8_t> backdrop(std::uint32_t w, std::uint32_t h) {
    std::vector<std::uint8_t> px(std::size_t{w} * h * 4);
    for (std::uint32_t y = 0; y < h; y++) {
        for (std::uint32_t x = 0; x < w; x++) {
            std::uint8_t *p = &px[(std::size_t{y} * w + x) * 4];
            p[0] = static_cast<std::uint8_t>(40 + (x * 160) / w);
            p[1] = static_cast<std::uint8_t>(90 + (y * 120) / h);
            p[2] = static_cast<std::uint8_t>(((x / 32 + y / 32) % 2) != 0 ? 150 : 110);
            p[3] = 255;
        }
    }
    return px;
}

// Synthetic rows, as later children add them: an Accessibility row and an Assist (bound to
// existing fields: the menu only reads the rows).
constexpr const SettingInfo kExtraRows[] = {
    {.key = "scale_mode",
     .group = SettingGroup::kVideo,
     .comment = "# scale",
     .value = fe::choice<&Settings::scale_mode>(fe::kScaleModeNames),
     .presets = {"integer", "integer", "integer"},
     .flag = "--scale-mode",
     .label = "Scale mode",
     .help = "How the image fills the window",
     .applies = fe::Applies::kAtOnce},
    {.key = "screen_shake",
     .group = SettingGroup::kAccessibility,
     .comment = "# shake",
     .value = fe::uint_range<&Settings::effects_volume>(0, 100, 25),
     .flag = "--screen-shake",
     .label = "Screen shake",
     .help = "How much the screen shakes, in percent"},
    {.key = "turbo_attack",
     .group = SettingGroup::kAssists,
     .comment = "# turbo",
     .value = fe::on_off<&Settings::hd_text>(),
     .presets = {"off", "off", "off"},
     .flag = "--turbo-attack",
     .label = "Turbo attack",
     .help = "Attacks again while the button is held"},
};

} // namespace

// --- The exit criterion -----------------------------------------------------------------------

TEST_CASE("settings menu: every registry row and action is reachable by keys, pad, mouse and "
          "touch") {
    Window win;
    for (const Device d : {Device::kKeys, Device::kPad, Device::kMouse, Device::kTouch}) {
        CAPTURE(device_name(d));
        // Every row of the registry, and every action row, in exactly one tab (the ios_only
        // rows only in a build for iOS, #194).
        for (const SettingInfo &row : fe::settings_registry()) {
            CAPTURE(row.key);
            if (!fe::setting_shown(row)) {
                continue;
            }
            Settings live;
            SettingsMenu menu(fe::settings_registry(), nullptr, nullptr, nullptr);
            menu.attach(&live);
            Driver drv{menu, &win};
            drv.key(SDLK_F1);
            REQUIRE(menu.is_open());
            CHECK(drv.reach(d, row.group, &row, nullptr));
            // Reached is also changeable with the same device, where the row offers values.
            const std::string before = fe::setting_text(live, row);
            if (!fe::setting_choices(row).empty()) {
                // A range at its top steps down (it is clamped, not wrapped).
                const bool top = row.value.kind == fe::ValueKind::kUint &&
                                 before == fe::setting_choices(row).back();
                switch (d) {
                case Device::kKeys:
                    drv.key(top ? SDLK_LEFT : SDLK_RIGHT);
                    break;
                case Device::kPad:
                    drv.button(top ? SDL_GAMEPAD_BUTTON_DPAD_LEFT : SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
                    break;
                default:
                    drv.tap(d, arrow_point(menu, menu.selected(), top ? -1 : 1));
                    break;
                }
                CHECK(fe::setting_text(live, row) != before);
                CHECK_FALSE(menu.take_changes().empty());
            }
        }
        for (const MenuActionInfo &action : fe::menu_actions()) {
            CAPTURE(action.key);
            Settings live;
            SettingsMenu menu(fe::settings_registry(), nullptr, nullptr, nullptr);
            menu.attach(&live);
            int runs = 0;
            menu.set_action(action.key, [&runs] {
                runs++;
                return std::string("ran");
            });
            Driver drv{menu, &win};
            drv.key(SDLK_F1);
            // An every-tab row (Quit) is in each tab: the first one's.
            CHECK(drv.reach(d, action.every_tab ? menu.tabs().front() : action.group, nullptr,
                            &action));
            // A tap on an action row runs it, so reaching it by pointer may have run it once
            // (a confirm row's tap only asks its question: no change of the row cancels it).
            runs = 0;
            const auto choose = [&] {
                switch (d) {
                case Device::kKeys:
                    drv.key(SDLK_RETURN);
                    break;
                case Device::kPad:
                    drv.button(SDL_GAMEPAD_BUTTON_SOUTH);
                    break;
                default:
                    drv.tap(d, label_point(menu, menu.selected()));
                    break;
                }
            };
            if (action.confirm != nullptr) {
                if (!menu.confirming()) {
                    choose();
                }
                REQUIRE(menu.confirming());
                CHECK_FALSE(menu.confirm_yes()); // No first
                switch (d) {
                case Device::kKeys:
                    drv.key(SDLK_RIGHT);
                    break;
                case Device::kPad:
                    drv.button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
                    break;
                default:
                    drv.tap(d, arrow_point(menu, menu.selected(), 1));
                    break;
                }
                CHECK(menu.confirm_yes());
                CHECK(runs == 0);
            }
            choose();
            CHECK(runs == 1);
            CHECK(menu.status() == "ran");
        }
        // The preset line: the top item, from any tab.
        Settings live;
        SettingsMenu menu(fe::settings_registry(), nullptr, nullptr, nullptr);
        menu.attach(&live);
        Driver drv{menu, &win};
        drv.key(SDLK_F1);
        switch (d) {
        case Device::kKeys:
            drv.key(SDLK_UP);
            drv.key(SDLK_RIGHT);
            break;
        case Device::kPad:
            drv.button(SDL_GAMEPAD_BUTTON_DPAD_UP);
            drv.button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
            break;
        default:
            drv.tap(d, arrow_point(menu, 0, 1));
            break;
        }
        CHECK(menu.selected() == 0);
        CHECK(live.preset == fe::Preset::kOriginal);
    }
    // Every row is in the tab of its group, once; the tabs are the groups that have rows.
    SettingsMenu menu(fe::settings_registry(), nullptr, nullptr, nullptr);
    std::size_t rows = 0;
    std::size_t actions = 0;
    Settings live;
    menu.attach(&live);
    Driver drv{menu, nullptr};
    drv.key(SDLK_F1);
    for (std::size_t t = 0; t < menu.tabs().size(); t++) {
        for (const SettingsMenu::Item &it : menu.items()) {
            if (it.kind == Kind::kSetting) {
                CHECK(it.setting->group == menu.tabs()[t]);
                rows++;
            } else if (it.kind == Kind::kAction) {
                CHECK((it.action->every_tab || it.action->group == menu.tabs()[t]));
                actions += it.action->every_tab ? (t == 0 ? 1 : 0) : 1;
            }
        }
        drv.key(SDLK_TAB);
    }
    const auto shown = std::count_if(fe::settings_registry().begin(), fe::settings_registry().end(),
                                     [](const SettingInfo &r) { return fe::setting_shown(r); });
    CHECK(rows == static_cast<std::size_t>(shown));
    CHECK(actions == fe::menu_actions().size());
}

TEST_CASE("settings menu: the tabs and rows follow the registry; Assists carry a warning") {
    Settings live;
    SettingsMenu menu(kExtraRows, nullptr, nullptr, nullptr);
    menu.attach(&live);
    // No Audio row and no Audio action: no Audio tab. Controls, Gameplay and Saves for the
    // actions; Accessibility and Assists for the new rows, each where its group says. Other for
    // the update notice's action row (#199).
    const std::vector<SettingGroup> want = {SettingGroup::kVideo,    SettingGroup::kControls,
                                            SettingGroup::kGameplay, SettingGroup::kAccessibility,
                                            SettingGroup::kAssists,  SettingGroup::kSaves,
                                            SettingGroup::kOther};
    CHECK(menu.tabs() == want);
    Driver drv{menu, nullptr};
    drv.key(SDLK_F1);
    drv.key(SDLK_TAB, SDL_KMOD_LSHIFT); // back from Video wraps to Other
    CHECK(menu.tab() == 6);
    drv.key(SDLK_Q); // Saves
    CHECK(menu.tab() == 5);
    drv.key(SDLK_Q); // Assists
    CHECK(menu.tab() == 4);
    // The preset, the row, and Quit at the bottom of every tab (#179; not on iOS).
    const bool quit_row = fe::find_menu_action("quit") != nullptr;
    REQUIRE(menu.items().size() == (quit_row ? 3u : 2u));
    CHECK(menu.items()[1].setting == &kExtraRows[2]);
    drv.button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER); // Accessibility
    CHECK(menu.items()[1].setting == &kExtraRows[1]);
    drv.key(SDLK_PAGEDOWN);
    CHECK(menu.tab() == 4);
    // The Assists tab's warning is drawn (orange text on the note line).
    std::vector<std::uint8_t> px(std::size_t{480} * 320 * 4, 0);
    menu.draw(px.data(), 480, 320, 1);
    bool orange = false;
    for (int x = 20; x < 300 && !orange; x++) {
        for (int y = 78; y < 86 && !orange; y++) {
            const std::uint8_t *p = &px[(std::size_t(y) * 480 + x) * 4];
            orange = p[0] == 0xFF && p[1] == 0x8A && p[2] == 0x5C;
        }
    }
    CHECK(orange);
}

TEST_CASE("settings menu: the preset line; a change makes it custom; what waits for the next "
          "start") {
    Settings live; // no preset chosen yet: the defaults
    SettingsMenu menu(fe::settings_registry(), nullptr, nullptr, nullptr);
    menu.attach(&live);
    menu.set_started(live);
    std::vector<std::string> applied;
    menu.set_apply([&applied](const SettingInfo &row, const Settings &) {
        applied.push_back(row.key);
        return std::string(row.key) != "filter"; // as on a window without SDL's GPU renderer
    });
    Driver drv{menu, nullptr};
    drv.key(SDLK_F1);
    CHECK(menu.selected() == 1);
    drv.key(SDLK_UP);
    REQUIRE(menu.selected() == 0);
    drv.key(SDLK_RIGHT);
    CHECK(live.preset == fe::Preset::kOriginal);
    CHECK(live.hide_keypad == fe::KeypadMode::kNever);
    CHECK(applied == std::vector<std::string>{"hide_keypad"}); // the one row Original changes
    CHECK(menu.status() == "Original: 1 setting changed");
    drv.key(SDLK_RIGHT);
    CHECK(live.preset == fe::Preset::kOriginalController);
    drv.key(SDLK_D);
    CHECK(live.preset == fe::Preset::kEnhanced);
    CHECK(live.hd_text);
    CHECK(live.gapless_music);
    // Read at start-up only: they wait for the next start.
    CHECK(menu.waits(*fe::find_setting("hd_text")));
    CHECK(menu.waits(*fe::find_setting("gapless_music")));
    CHECK_FALSE(menu.waits(*fe::find_setting("hide_keypad"))); // applied, and back as started
    drv.key(SDLK_RIGHT);
    CHECK(live.preset == fe::Preset::kCustom);
    CHECK(live.hd_text); // custom keeps the values
    drv.key(SDLK_LEFT);
    CHECK(live.preset == fe::Preset::kEnhanced);
    // One row changed: custom.
    drv.key(SDLK_DOWN); // scale_mode, the first row of Video
    REQUIRE(menu.items()[menu.selected()].setting == fe::find_setting("scale_mode"));
    drv.key(SDLK_RIGHT);
    CHECK(live.scale_mode == fe::ScaleMode::kFit);
    CHECK(live.preset == fe::Preset::kCustom);
    CHECK(menu.status() == "Scale mode: fit");
    // A live row the frontend cannot apply now waits too.
    while (menu.items()[menu.selected()].setting != fe::find_setting("filter")) {
        drv.key(SDLK_DOWN);
    }
    drv.key(SDLK_RIGHT);
    CHECK(live.filter == fe::ShaderFilter::kXbr);
    CHECK(menu.waits(*fe::find_setting("filter")));
    CHECK(menu.status() == "Filter: xbr, at the next start");
    drv.key(SDLK_LEFT); // back to what this run started with: nothing waits
    CHECK_FALSE(menu.waits(*fe::find_setting("filter")));
    // A range clamps at its end and changes nothing there: render_scale is at its first value,
    // auto (its name for 0, #152; Enhanced's).
    while (menu.items()[menu.selected()].setting != fe::find_setting("render_scale")) {
        drv.key(SDLK_UP);
    }
    REQUIRE(live.render_scale == fe::kRenderScaleAuto);
    drv.key(SDLK_LEFT);
    CHECK(live.render_scale == fe::kRenderScaleAuto);
    // What the file gets: the menu's apply_setting calls, in order.
    using Change = std::pair<std::string, std::string>;
    CHECK(menu.take_changes() == std::vector<Change>{{"preset", "original"},
                                                     {"preset", "original_controller"},
                                                     {"preset", "enhanced"},
                                                     {"preset", "custom"},
                                                     {"preset", "enhanced"},
                                                     {"scale_mode", "fit"},
                                                     {"filter", "xbr"},
                                                     {"filter", "none"}});
}

TEST_CASE("settings menu: action rows; the game options opener") {
    Settings live;
    SettingsMenu menu(fe::settings_registry(), nullptr, nullptr, nullptr);
    menu.attach(&live);
    CHECK(fe::find_menu_action("game_options") != nullptr);
    CHECK(fe::find_menu_action("nope") == nullptr);
    menu.set_action("nope", [] { return std::string("x"); }); // logged, ignored
    Driver drv{menu, nullptr};
    drv.key(SDLK_F1);
    REQUIRE(drv.reach(Device::kKeys, SettingGroup::kSaves, nullptr,
                      fe::find_menu_action("export_saves")));
    drv.key(SDLK_RETURN);
    CHECK(menu.status() == "Export saves... is not available in this build");
    // #148's hook: a handler for the key.
    menu.set_action("export_saves", [] { return std::string("Exported 4 files"); });
    drv.key(SDLK_SPACE);
    CHECK(menu.status() == "Exported 4 files");
    // Left / Right do nothing on an action.
    const Settings before = live;
    drv.key(SDLK_RIGHT);
    CHECK(live == before);

    // The opener: the field's Menu action, then the popup menu's option tab.
    std::int32_t panel = -1;
    std::vector<std::string> log;
    fe::GameOptionsOpener opener(
        [&log](std::uint32_t action, bool down) {
            log.push_back(std::string(down ? "+" : "-") +
                          (action == MT_ACTION_MENU ? "menu" : "other"));
        },
        [&panel] { return panel; },
        [&log](std::uint32_t phase, float x, float y) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "touch %u %.0f,%.0f", phase, static_cast<double>(x),
                          static_cast<double>(y));
            log.push_back(buf);
        });
    CHECK_FALSE(opener.available()); // the title, a menu
    CHECK_FALSE(opener.start());
    CHECK(opener.outcome() == fe::GameOptionsOpener::Outcome::kNotInField);
    panel = 0; // the field
    REQUIRE(opener.start());
    opener.turn(10.0);
    opener.turn(10.02);
    CHECK(log == std::vector<std::string>{"+menu", "-menu"});
    opener.turn(10.04); // the popup menu is not up yet
    panel = 1;
    opener.turn(10.06);
    opener.turn(10.2); // settling
    CHECK(log.size() == 2);
    opener.turn(10.4);
    opener.turn(10.42);
    float x = 0, y = 0;
    REQUIRE(fe::popup_menu_tab_point(5, &x, &y));
    char want[64];
    std::snprintf(want, sizeof want, "%.0f,%.0f", static_cast<double>(x), static_cast<double>(y));
    REQUIRE(log.size() == 4);
    CHECK(log[2] == "touch " + std::to_string(MT_UIKIT_TOUCH_BEGAN) + " " + want);
    CHECK(log[3] == "touch " + std::to_string(MT_UIKIT_TOUCH_ENDED) + " " + want);
    CHECK(opener.outcome() == fe::GameOptionsOpener::Outcome::kOpened);
    CHECK_FALSE(opener.busy());
    // The popup menu never comes up: it gives up after kWaitSeconds.
    panel = 0;
    REQUIRE(opener.start());
    opener.turn(20.0);
    opener.turn(20.02);
    opener.turn(20.04 + fe::GameOptionsOpener::kWaitSeconds);
    CHECK(opener.outcome() == fe::GameOptionsOpener::Outcome::kNoMenu);
    CHECK_FALSE(opener.busy());
}

TEST_CASE("settings menu: Quit (#179): at the bottom of every tab, No first; only Yes quits") {
    const MenuActionInfo *quit = fe::find_menu_action("quit");
#if defined(SDL_PLATFORM_IOS)
    CHECK(quit == nullptr); // iOS apps do not quit themselves
#else
    REQUIRE(quit != nullptr);
    CHECK(quit->every_tab);
    CHECK(std::string(quit->confirm) == "Quit MapleThief?");
    Settings live;
    SettingsMenu menu(fe::settings_registry(), nullptr, nullptr, nullptr);
    menu.attach(&live);
    {
        // Without a handler it asks nothing: it says so at once.
        Driver bare{menu, nullptr};
        bare.key(SDLK_F1);
        while (menu.items()[menu.selected()].action != quit) {
            bare.key(SDLK_UP);
        }
        bare.key(SDLK_RETURN);
        CHECK_FALSE(menu.confirming());
        CHECK(menu.status() == "Quit is not available in this build");
        bare.key(SDLK_ESCAPE);
    }
    int quits = 0;
    menu.set_action("quit", [&] {
        quits++;
        menu.close();
        return std::string();
    });
    Driver drv{menu, nullptr};
    drv.key(SDLK_F1);
    // The last row of every tab.
    for (std::size_t t = 0; t < menu.tabs().size(); t++) {
        CAPTURE(t);
        CHECK(menu.items().back().action == quit);
        drv.key(SDLK_TAB);
    }
    const auto to_quit = [&] {
        while (menu.items()[menu.selected()].action != quit) {
            drv.key(SDLK_UP); // from the preset line, Up wraps to the bottom
        }
    };
    // Choosing asks; No is focused: choosing again cancels, nothing quits.
    to_quit();
    drv.key(SDLK_RETURN);
    CHECK(menu.confirming());
    CHECK_FALSE(menu.confirm_yes());
    CHECK(menu.status() == "Quit MapleThief? Right picks Yes, choose again to confirm");
    drv.key(SDLK_RETURN);
    CHECK_FALSE(menu.confirming());
    CHECK(quits == 0);
    CHECK(menu.is_open());
    // Esc and the pad's East cancel the question, not the menu; so do moving and a tab.
    drv.key(SDLK_RETURN);
    drv.key(SDLK_RIGHT);
    CHECK(menu.confirm_yes());
    drv.key(SDLK_ESCAPE);
    CHECK_FALSE(menu.confirming());
    CHECK(menu.is_open());
    drv.key(SDLK_RETURN);
    drv.button(SDL_GAMEPAD_BUTTON_EAST);
    CHECK_FALSE(menu.confirming());
    CHECK(menu.is_open());
    drv.key(SDLK_RETURN);
    drv.key(SDLK_UP);
    CHECK_FALSE(menu.confirming());
    to_quit();
    drv.key(SDLK_RETURN);
    drv.key(SDLK_TAB);
    CHECK_FALSE(menu.confirming());
    CHECK(quits == 0);
    // Yes then choose: the handler runs once (game.cpp's sends SDL_EVENT_QUIT).
    to_quit();
    drv.key(SDLK_RETURN);
    drv.key(SDLK_RIGHT);
    drv.key(SDLK_LEFT);
    CHECK_FALSE(menu.confirm_yes());
    drv.key(SDLK_RIGHT);
    drv.key(SDLK_RETURN);
    CHECK(quits == 1);
    CHECK_FALSE(menu.is_open());
    CHECK(menu.take_changes().empty()); // not a setting
#endif
}

TEST_CASE("settings menu: a list in place of the tab (#192): move, page, ask with No first, back") {
    Settings live;
    SettingsMenu menu(fe::settings_registry(), nullptr, nullptr, nullptr);
    menu.attach(&live);
    std::vector<std::size_t> chosen;
    const auto make = [&](std::size_t n) {
        fe::MenuList list;
        list.title = "BACKUPS";
        list.help = "help";
        for (std::size_t i = 0; i < n; i++) {
            list.rows.push_back({"Slot " + std::to_string(i), "time " + std::to_string(i)});
        }
        // Row 1 is chosen at once (no question), as a refused restore is.
        list.question = [](std::size_t i) {
            return i == 1 ? std::string() : "Restore " + std::to_string(i) + "?";
        };
        list.choose = [&chosen](std::size_t i) {
            chosen.push_back(i);
            return "Restored " + std::to_string(i);
        };
        return list;
    };
    std::size_t rows = 25;
    menu.set_action("backups", [&] {
        menu.open_list(make(rows));
        return std::string();
    });
    const MenuActionInfo *backups = fe::find_menu_action("backups");
    Driver drv{menu, nullptr};
    drv.key(SDLK_F1);
    REQUIRE(drv.reach(Device::kKeys, SettingGroup::kSaves, nullptr, backups));
    const std::size_t backups_row = menu.selected();
    const std::size_t tab = menu.tab();
    const auto open = [&] {
        drv.key(SDLK_RETURN);
        REQUIRE(menu.in_list());
        CHECK(menu.selected() == 1);
        CHECK(menu.items().size() == rows + 1);
        CHECK(menu.items()[1].kind == Kind::kListRow);
    };
    open();
    // Up / Down move over the rows only (wrapping past the preset line).
    drv.key(SDLK_UP);
    CHECK(menu.selected() == rows);
    drv.key(SDLK_DOWN);
    CHECK(menu.selected() == 1);
    drv.button(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    CHECK(menu.selected() == 2);
    menu.handle(stick_event(SDL_GAMEPAD_AXIS_LEFTY, 30000));
    menu.handle(stick_event(SDL_GAMEPAD_AXIS_LEFTY, 0));
    CHECK(menu.selected() == 3);
    // A diagonal is one move (#138): the axis held first made it, the other does nothing.
    menu.handle(stick_event(SDL_GAMEPAD_AXIS_LEFTX, 30000));
    menu.handle(stick_event(SDL_GAMEPAD_AXIS_LEFTY, 30000));
    CHECK(menu.selected() == 3);
    menu.handle(stick_event(SDL_GAMEPAD_AXIS_LEFTY, 0));
    menu.handle(stick_event(SDL_GAMEPAD_AXIS_LEFTX, 0));
    menu.handle(stick_event(SDL_GAMEPAD_AXIS_LEFTY, 30000));
    menu.handle(stick_event(SDL_GAMEPAD_AXIS_LEFTX, 30000));
    CHECK(menu.selected() == 4);
    menu.handle(stick_event(SDL_GAMEPAD_AXIS_LEFTX, 0));
    menu.handle(stick_event(SDL_GAMEPAD_AXIS_LEFTY, 0));
    drv.button(SDL_GAMEPAD_BUTTON_DPAD_UP);
    CHECK(menu.selected() == 3);
    // Paging (ADR 0114): LT held with the d-pad, Shift with the arrows, Page Up / Down, LB / RB;
    // clamped at the ends, never wrapping.
    menu.handle(stick_event(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767));
    drv.button(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    CHECK(menu.selected() == 3 + SettingsMenu::kVisibleRows);
    drv.button(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    drv.button(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    CHECK(menu.selected() == rows);
    CHECK(menu.first_visible() == rows - SettingsMenu::kVisibleRows);
    menu.handle(stick_event(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0));
    drv.button(SDL_GAMEPAD_BUTTON_DPAD_UP);
    CHECK(menu.selected() == rows - 1);
    drv.key(SDLK_UP, SDL_KMOD_LSHIFT);
    CHECK(menu.selected() == rows - 1 - SettingsMenu::kVisibleRows);
    drv.key(SDLK_PAGEUP);
    drv.button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    CHECK(menu.selected() == 1);
    drv.button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    CHECK(menu.selected() == 1 + SettingsMenu::kVisibleRows);
    CHECK(menu.tab() == tab); // the shoulders page; they do not change the tab here
    drv.key(SDLK_PAGEDOWN);
    CHECK(menu.selected() == 1 + 2 * SettingsMenu::kVisibleRows);
    // Choosing asks, with No focused; choosing again on No does nothing.
    while (menu.selected() != 1) {
        drv.key(SDLK_UP);
    }
    drv.key(SDLK_RETURN);
    CHECK(menu.confirming());
    CHECK_FALSE(menu.confirm_yes());
    CHECK(menu.status() == "Right picks Yes, choose again to confirm");
    drv.key(SDLK_RETURN);
    CHECK_FALSE(menu.confirming());
    CHECK(chosen.empty());
    CHECK(menu.in_list());
    // Esc and East cancel the question first, then go back to the tab, on the Backups row.
    drv.key(SDLK_RETURN);
    drv.key(SDLK_ESCAPE);
    CHECK_FALSE(menu.confirming());
    CHECK(menu.in_list());
    drv.key(SDLK_ESCAPE);
    CHECK_FALSE(menu.in_list());
    CHECK(menu.is_open());
    CHECK(menu.selected() == backups_row);
    CHECK(menu.tab() == tab);
    open();
    drv.button(SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK(menu.confirming());
    drv.button(SDL_GAMEPAD_BUTTON_EAST);
    CHECK_FALSE(menu.confirming());
    drv.button(SDL_GAMEPAD_BUTTON_EAST);
    CHECK_FALSE(menu.in_list());
    CHECK(menu.is_open());
    // A tab closes it too.
    open();
    drv.key(SDLK_TAB);
    CHECK_FALSE(menu.in_list());
    REQUIRE(drv.reach(Device::kKeys, SettingGroup::kSaves, nullptr, backups));
    // The pad: South asks, Right picks Yes, South confirms: chosen, and the list closes.
    open();
    drv.button(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    drv.button(SDL_GAMEPAD_BUTTON_DPAD_DOWN); // row 2
    drv.button(SDL_GAMEPAD_BUTTON_SOUTH);
    drv.button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    CHECK(menu.confirm_yes());
    {
        // The question in the help lines, Yes in the row's value, the list's hints: by hash, and
        // a PNG to look at.
        std::vector<std::uint8_t> px = backdrop(480, 320);
        menu.draw(px.data(), 480, 320, 1);
        const fs::path out = fs::path(output_dir()) / "settings_menu";
        std::error_code ec;
        fs::create_directories(out, ec);
        SDL_Surface *s =
            SDL_CreateSurfaceFrom(480, 320, SDL_PIXELFORMAT_ABGR8888, px.data(), 480 * 4);
        CHECK((s != nullptr && SDL_SavePNG(s, (out / "saves_list.png").string().c_str())));
        SDL_DestroySurface(s);
        char hex[32];
        std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(fnv1a(px)));
        MESSAGE("saves_list hash " << hex);
        CHECK(fnv1a(px) == 0x6ebd997fabd69702ull);
    }
    drv.button(SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK(chosen == std::vector<std::size_t>{2});
    CHECK(menu.status() == "Restored 2");
    CHECK_FALSE(menu.in_list());
    CHECK(menu.selected() == backups_row);
    // A row without a question is chosen at once.
    open();
    drv.key(SDLK_DOWN);
    drv.key(SDLK_RETURN);
    CHECK(chosen == std::vector<std::size_t>{2, 1});
    CHECK_FALSE(menu.in_list());
    // A mouse or a finger: a tap on a row asks, on ">" picks Yes, on the row again confirms.
    open();
    menu.pointer(label_point(menu, 1), true, false);
    menu.pointer(label_point(menu, 1), false, true);
    CHECK(menu.confirming());
    menu.pointer(arrow_point(menu, 1, +1), true, false);
    menu.pointer(arrow_point(menu, 1, +1), false, true);
    CHECK(menu.confirm_yes());
    menu.pointer(label_point(menu, 1), true, false);
    menu.pointer(label_point(menu, 1), false, true);
    CHECK(chosen == std::vector<std::size_t>{2, 1, 0});
    // The preset line is not a row of a list.
    open();
    CHECK(menu.item_at(SettingsMenu::kListX + 30, SettingsMenu::kPresetY + 6) == -1);
    // Closing the menu closes the list; it opens again on the tab's rows.
    drv.key(SDLK_F1);
    CHECK_FALSE(menu.is_open());
    drv.key(SDLK_F1);
    CHECK_FALSE(menu.in_list());
    CHECK(menu.selected() == backups_row);
    // An empty list opens nothing.
    rows = 0;
    drv.key(SDLK_RETURN);
    CHECK_FALSE(menu.in_list());
}

TEST_CASE("settings menu: scripted runs never open it; F1, Guide and L3 + R3 do otherwise") {
    fe::GameOptions plain;
    CHECK_FALSE(fe::scripted_run(plain));
    const auto scripted = [](auto set) {
        fe::GameOptions o;
        set(o);
        return fe::scripted_run(o);
    };
    CHECK(scripted([](fe::GameOptions &o) { o.frames = 10; }));
    CHECK(scripted([](fe::GameOptions &o) { o.seconds = 5; }));
    CHECK(scripted([](fe::GameOptions &o) { o.capture_dir = "x"; }));
    CHECK(scripted([](fe::GameOptions &o) { o.capture_at = {3}; }));
    CHECK(scripted([](fe::GameOptions &o) { o.shots = {fe::Shot{1, "a"}}; }));
    CHECK(scripted([](fe::GameOptions &o) { o.taps = {fe::Tap{}}; }));
    CHECK(scripted([](fe::GameOptions &o) { o.fixed_step_us = 16667; }));
    CHECK(scripted([](fe::GameOptions &o) { o.audio_capture = "a.wav"; }));
    CHECK(scripted([](fe::GameOptions &o) { o.suspends = {fe::Suspend{}}; }));
    CHECK(scripted([](fe::GameOptions &o) { o.quit_at = 3; }));

    Settings live;
    SettingsMenu menu(fe::settings_registry(), nullptr, nullptr, nullptr);
    menu.attach(&live);
    menu.set_allowed(false);
    CHECK_FALSE(menu.handle(key_event(SDLK_F1)));
    CHECK_FALSE(menu.handle(pad_event(SDL_GAMEPAD_BUTTON_GUIDE)));
    CHECK_FALSE(menu.is_open());
    menu.set_allowed(true);
    // A repeat of F1 does not open it; other keys pass.
    SDL_Event repeat = key_event(SDLK_F1);
    repeat.key.repeat = true;
    CHECK_FALSE(menu.handle(repeat));
    CHECK_FALSE(menu.handle(key_event(SDLK_ESCAPE)));
    CHECK(menu.handle(pad_event(SDL_GAMEPAD_BUTTON_GUIDE)));
    CHECK(menu.is_open());
    menu.close();
    // While open, every key and pad event is the menu's; Start, East, Esc, F1, Guide close.
    for (const SDL_Event &closer :
         {pad_event(SDL_GAMEPAD_BUTTON_START), pad_event(SDL_GAMEPAD_BUTTON_EAST),
          key_event(SDLK_ESCAPE), key_event(SDLK_F1), key_event(SDLK_BACKSPACE),
          pad_event(SDL_GAMEPAD_BUTTON_GUIDE)}) {
        CHECK(menu.handle(key_event(SDLK_F1))); // opens
        REQUIRE(menu.is_open());
        CHECK(menu.handle(key_event(SDLK_Z, false)));
        CHECK(menu.handle(pad_event(SDL_GAMEPAD_BUTTON_WEST)));
        CHECK(menu.handle(closer));
        CHECK_FALSE(menu.is_open());
    }
    CHECK(menu.opens() == 7);
    // L3 + R3 on a pad: both held opens it, one alone does not.
    menu.close();
    REQUIRE(SDL_Init(SDL_INIT_GAMEPAD));
    virtual_pad::Pad pad(virtual_pad::Kind::kXbox360);
    REQUIRE_MESSAGE(pad.ok(), pad.error);
    SDL_PumpEvents();
    SDL_Gamepad *g = SDL_OpenGamepad(pad.id);
    REQUIRE(g != nullptr);
    const auto pump = [&menu] {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            menu.handle(e);
        }
    };
    pump();
    pad.button(SDL_GAMEPAD_BUTTON_LEFT_STICK, true);
    pump();
    CHECK_FALSE(menu.is_open());
    pad.button(SDL_GAMEPAD_BUTTON_RIGHT_STICK, true);
    pump();
    CHECK(menu.is_open());
    SDL_CloseGamepad(g);
    pad.detach();
    SDL_Quit();
}

TEST_CASE("settings menu: L3 + R3 wins over the fast_forward action R3 in either order") {
    // ADR 0087's addendum: R3 alone holds fast_forward (#151); with L3 it is the menu's chord
    // (#89). The chord opens the menu, and the menu's open lets go of every pad control, so
    // fast_forward is never left held, whichever stick went down first.
    REQUIRE(SDL_Init(SDL_INIT_GAMEPAD));
    fe::Input input;
    fe::Actions actions{&input};
    fe::Gamepads gamepads{&actions, &input};
    actions.set_sink([](void *, std::uint32_t, bool) {}, nullptr);
    actions.set_panel_source([](void *) -> std::int32_t { return 0; }, nullptr); // the field
    Settings live;
    SettingsMenu menu(fe::settings_registry(), &actions, &gamepads, &input);
    menu.attach(&live);
    menu.set_allowed(true);
    virtual_pad::Pad pad(virtual_pad::Kind::kXbox360);
    REQUIRE_MESSAGE(pad.ok(), pad.error);
    // game.cpp's order: the menu first, the pads only with what it did not take.
    const auto pump = [&] {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (!menu.handle(e)) {
                gamepads.handle(e);
            }
        }
    };
    pump();
    // R3 alone: fast_forward is held while it is down.
    pad.button(SDL_GAMEPAD_BUTTON_RIGHT_STICK, true);
    pump();
    CHECK(actions.holding(MT_ACTION_FAST_FORWARD));
    CHECK_FALSE(menu.is_open());
    // Then L3: the chord opens the menu and lets go of fast_forward.
    pad.button(SDL_GAMEPAD_BUTTON_LEFT_STICK, true);
    pump();
    CHECK(menu.is_open());
    CHECK_FALSE(actions.holding(MT_ACTION_FAST_FORWARD));
    menu.close();
    pad.button(SDL_GAMEPAD_BUTTON_LEFT_STICK, false);
    pad.button(SDL_GAMEPAD_BUTTON_RIGHT_STICK, false);
    pump();
    CHECK_FALSE(actions.holding(MT_ACTION_FAST_FORWARD));
    // L3 first, then R3: R3 is the chord's, never the game's.
    pad.button(SDL_GAMEPAD_BUTTON_LEFT_STICK, true);
    pump();
    CHECK_FALSE(menu.is_open());
    pad.button(SDL_GAMEPAD_BUTTON_RIGHT_STICK, true);
    pump();
    CHECK(menu.is_open());
    CHECK_FALSE(actions.holding(MT_ACTION_FAST_FORWARD));
    // Letting go while the menu is open, and after it closed, starts nothing.
    pad.button(SDL_GAMEPAD_BUTTON_RIGHT_STICK, false);
    pump();
    menu.close();
    pad.button(SDL_GAMEPAD_BUTTON_LEFT_STICK, false);
    pump();
    CHECK_FALSE(actions.holding(MT_ACTION_FAST_FORWARD));
    pad.detach();
    SDL_Quit();
}

TEST_CASE("settings menu: open, change and close on a window with every device; applied live; "
          "written to settings.ini") {
    TempDir dir("live");
    Window win;
    // The frontend's objects a change reaches.
    fe::Presenter presenter;
    REQUIRE(presenter.start(win.renderer, 480, 320));
    REQUIRE(presenter.draw());
    fe::Input input;
    fe::Actions actions{&input};
    fe::Gamepads gamepads{&actions, &input};
    fe::Rumble rumble{&gamepads};
    fe::KeypadHider keypad(fe::KeypadMode::kAuto);
    fe::FocusMute focus_mute(true);
    fe::PerfOverlay perf(false, true);
    fe::PresentPass pass;

    // The run started with music_volume 50 from a one-run flag (not in the file).
    REQUIRE(fe::save_settings(dir.path.string(), Settings{}));
    Settings live;
    live.music_volume = 50;
    SettingsMenu menu(fe::settings_registry(), &actions, &gamepads, &input);
    menu.attach(&live);
    menu.set_started(live);
    menu.set_apply([&](const SettingInfo &row, const Settings &s) {
        fe::LiveTargets t;
        t.presenter = &presenter;
        t.filter_pass = &pass;
        t.renderer = win.renderer;
        t.input = &input;
        t.keypad = &keypad;
        t.rumble = &rumble;
        t.focus_mute = &focus_mute;
        t.perf = &perf;
        t.now = 1.0;
        return fe::apply_live_setting(t, row, s);
    });
    // Pushed events through SDL's queue, routed as game.cpp routes them; the gear where it is.
    const fe::GearPlace gear = fe::gear_place(win.placement, 1);
    CHECK(gear.over_image); // a 960 x 640 window at 2x: no bars
    const auto run = [&](const std::vector<SDL_Event> &events) {
        for (SDL_Event e : events) {
            REQUIRE(SDL_PushEvent(&e));
        }
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            fe::menu_route_event(menu, e, win.window, win.placement, &gear.rect);
            menu.track(e);
        }
    };
    const auto find = [&menu](const char *key) {
        for (std::size_t i = 0; i < menu.items().size(); i++) {
            if (menu.items()[i].setting == fe::find_setting(key)) {
                return i;
            }
        }
        return std::size_t{0};
    };

    // The keyboard: F1, the first Video row (scale_mode), Right: fit; Esc.
    run({key_event(SDLK_F1), key_event(SDLK_F1, false)});
    REQUIRE(menu.is_open());
    REQUIRE(menu.items()[menu.selected()].setting == fe::find_setting("scale_mode"));
    run({key_event(SDLK_RIGHT), key_event(SDLK_ESCAPE)});
    CHECK_FALSE(menu.is_open());
    CHECK(live.scale_mode == fe::ScaleMode::kFit);
    CHECK(presenter.scale_mode() == fe::ScaleMode::kFit);

    // The pad: Guide, RB twice (Controls), down to hide_keypad, the stick right: always; East.
    run({pad_event(SDL_GAMEPAD_BUTTON_GUIDE)});
    REQUIRE(menu.is_open());
    run({pad_event(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER),
         pad_event(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)});
    CHECK(menu.tabs()[menu.tab()] == SettingGroup::kControls);
    while (menu.selected() != find("hide_keypad")) {
        run({pad_event(SDL_GAMEPAD_BUTTON_DPAD_DOWN)});
    }
    run({stick_event(SDL_GAMEPAD_AXIS_LEFTX, 30000), stick_event(SDL_GAMEPAD_AXIS_LEFTX, 0),
         pad_event(SDL_GAMEPAD_BUTTON_EAST)});
    CHECK_FALSE(menu.is_open());
    CHECK(live.hide_keypad == fe::KeypadMode::kAlways);
    CHECK(keypad.mode() == fe::KeypadMode::kAlways);

    // The mouse: F1, a click on the Audio tab, on master_volume's "<": 95; the close box.
    run({key_event(SDLK_F1)});
    run({mouse_event(SDL_EVENT_MOUSE_BUTTON_DOWN, tab_centre(menu, 1)),
         mouse_event(SDL_EVENT_MOUSE_BUTTON_UP, tab_centre(menu, 1))});
    REQUIRE(menu.tabs()[menu.tab()] == SettingGroup::kAudio);
    const fe::ScreenPoint minus = arrow_point(menu, find("master_volume"), -1);
    run({mouse_event(SDL_EVENT_MOUSE_MOTION, minus),
         mouse_event(SDL_EVENT_MOUSE_BUTTON_DOWN, minus),
         mouse_event(SDL_EVENT_MOUSE_BUTTON_UP, minus),
         mouse_event(SDL_EVENT_MOUSE_BUTTON_DOWN, close_point()),
         mouse_event(SDL_EVENT_MOUSE_BUTTON_UP, close_point())});
    CHECK_FALSE(menu.is_open());
    CHECK(live.master_volume == 95);
    CHECK(mt_audio_get_gain(MT_AUDIO_BUS_MASTER) == doctest::Approx(0.95));

    // Touch: a tap on the gear, on the Video tab, on border_color's ">": #202020; the close box.
    const fe::ScreenPoint gear_point =
        at((gear.rect.x + gear.rect.w / 2) / 2, (gear.rect.y + gear.rect.h / 2) / 2);
    run({finger_event(SDL_EVENT_FINGER_DOWN, gear_point),
         finger_event(SDL_EVENT_FINGER_UP, gear_point)});
    REQUIRE(menu.is_open());
    run({finger_event(SDL_EVENT_FINGER_DOWN, tab_centre(menu, 0)),
         finger_event(SDL_EVENT_FINGER_UP, tab_centre(menu, 0))});
    const fe::ScreenPoint plus = arrow_point(menu, find("border_color"), 1);
    run({finger_event(SDL_EVENT_FINGER_DOWN, plus), finger_event(SDL_EVENT_FINGER_UP, plus),
         finger_event(SDL_EVENT_FINGER_DOWN, close_point()),
         finger_event(SDL_EVENT_FINGER_UP, close_point())});
    CHECK_FALSE(menu.is_open());
    CHECK(fe::setting_text(live, *fe::find_setting("border_color")) == "#202020");
    CHECK(presenter.border_color() == fe::Color{0x20, 0x20, 0x20});
    CHECK(menu.opens() == 4);

    // Written: the four changes, not the one-run flag; the file reads back as the menu left it.
    REQUIRE(menu.changed());
    REQUIRE(fe::save_menu_changes(dir.path.string(), menu.take_changes()));
    const std::string text = read(dir.path / "settings.ini");
    CHECK(text.find("scale_mode = fit\n") != std::string::npos);
    CHECK(text.find("hide_keypad = always\n") != std::string::npos);
    CHECK(text.find("master_volume = 95\n") != std::string::npos);
    CHECK(text.find("border_color = #202020\n") != std::string::npos);
    CHECK(text.find("music_volume = 100\n") != std::string::npos);
    Settings back;
    fe::load_settings(dir.path.string(), &back);
    Settings want = live;
    want.music_volume = 100;
    CHECK(back == want);
    CHECK_FALSE(menu.changed());
    mt_audio_set_gain(MT_AUDIO_BUS_MASTER, 1.0f);
}

TEST_CASE("settings menu: save_menu_changes replays over the file as it is") {
    TempDir dir("replay");
    {
        std::ofstream out(dir.path / "settings.ini", std::ios::binary);
        out << "# mine\npreset = enhanced\nmusic_volume = 30\nunknown_key = 1\n";
    }
    // The menu turned hd_text off (the preset becomes custom) and the volume up.
    REQUIRE(
        fe::save_menu_changes(dir.path.string(), {{"hd_text", "off"}, {"effects_volume", "80"}}));
    const std::string text = read(dir.path / "settings.ini");
    CHECK(text.rfind("# mine\npreset = custom\nmusic_volume = 30\nunknown_key = 1\n", 0) == 0);
    Settings back;
    fe::load_settings(dir.path.string(), &back);
    CHECK(back.preset == fe::Preset::kCustom);
    CHECK_FALSE(back.hd_text);
    CHECK(back.gapless_music); // Enhanced's, kept
    CHECK(back.effects_volume == 80);
    CHECK(back.music_volume == 30);
    // A preset from the menu.
    REQUIRE(fe::save_menu_changes(dir.path.string(), {{"preset", "original"}}));
    fe::load_settings(dir.path.string(), &back);
    CHECK(back.preset == fe::Preset::kOriginal);
    CHECK(back.hide_keypad == fe::KeypadMode::kNever);
    CHECK_FALSE(fe::save_menu_changes("", {{"hd_text", "on"}})); // no data directory
}

TEST_CASE("live settings: each live row reaches its object; the others wait") {
    Window win;
    fe::Presenter presenter;
    REQUIRE(presenter.start(win.renderer, 480, 320));
    fe::Input input;
    fe::Actions actions{&input};
    fe::Gamepads gamepads{&actions, &input};
    fe::Rumble rumble{&gamepads};
    fe::KeypadHider keypad(fe::KeypadMode::kAuto);
    fe::FocusMute focus_mute(true);
    fe::PerfOverlay perf(false, true);
    fe::PresentPass pass;
    fe::LiveTargets t;
    t.presenter = &presenter;
    t.filter_pass = &pass;
    t.renderer = win.renderer;
    t.input = &input;
    t.keypad = &keypad;
    t.rumble = &rumble;
    t.focus_mute = &focus_mute;
    t.perf = &perf;
    // #145's rows (the desktop, the lifecycle) and #152's render scale.
    fe::Desktop desktop;
    desktop.attach(win.window, win.renderer);
    bool vsync = true;
    fe::FrameStats stats;
    stats.start(0, 60, vsync, 0);
    fe::Lifecycle lifecycle;
    std::vector<std::uint32_t> scales;
    t.desktop = &desktop;
    t.vsync = &vsync;
    t.stats = &stats;
    t.lifecycle = &lifecycle;
    t.render_scale = [&scales](std::uint32_t n) {
        scales.push_back(n);
        return true;
    };
    // #159's key pad layout (install_keypad_layout in the game), and the rows the turn hook
    // reads from the live settings (#146, #147, #150, #151).
    std::vector<std::string> layouts;
    t.keypad_layout = [&layouts](const fe::KeypadLayout &l) {
        layouts.push_back(fe::keypad_layout_text(l));
    };
    t.turn_reads_settings = true;
    // #195's output device: a stream on SDL's dummy driver, which audio_device reopens.
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    fe::AudioDevice audio;
    REQUIRE(audio.open());
    t.audio = &audio;
    Settings s;
    s.vsync = false;
    s.audio_mono = true;
    s.audio_device = "No Such Device";
    s.pause_unfocused = true;
    s.render_scale = 3;
    s.scale_mode = fe::ScaleMode::kSharp;
    s.border_color = fe::Color{1, 2, 3};
    s.master_volume = 40;
    s.music_volume = 60;
    s.effects_volume = 80;
    s.mute_unfocused = false;
    s.right_touch = fe::RightTouch{true, 440, 290};
    s.hide_keypad = fe::KeypadMode::kNever;
    s.rumble = false;
    s.rumble_strength = 30;
    s.perf_overlay = true;
    s.filter = fe::ShaderFilter::kCrt;
    s.border = fe::BorderMode::kImage;
    s.border_image = "art.png";
    REQUIRE(fe::parse_keypad_layout("dpad=80,12,-20,70", &s.keypad_layout));
    const bool gpu = SDL_GetGPURendererDevice(win.renderer) != nullptr;
    for (const SettingInfo &row : fe::settings_registry()) {
        CAPTURE(row.key);
        const bool applied = fe::apply_live_setting(t, row, s);
        if (row.applies == fe::Applies::kAtOnce) {
            // Every row that says it applies at once has its case; a filter needs SDL's GPU
            // renderer (the dummy driver's is software).
            CHECK(applied == (std::string(row.key) != "filter" || gpu));
        } else {
            CHECK_FALSE(applied);
        }
    }
    CHECK(presenter.scale_mode() == fe::ScaleMode::kSharp);
    CHECK(presenter.border_color() == fe::Color{1, 2, 3});
    CHECK(mt_audio_get_gain(MT_AUDIO_BUS_MASTER) == doctest::Approx(0.4));
    CHECK(mt_audio_get_gain(MT_AUDIO_BUS_MUSIC) == doctest::Approx(0.6));
    CHECK(mt_audio_get_gain(MT_AUDIO_BUS_EFFECTS) == doctest::Approx(0.8));
    CHECK_FALSE(focus_mute.enabled());
    CHECK(mt_audio_get_mono());
    mt_audio_set_mono(false);
    CHECK(audio.is_open());
    CHECK(audio.fell_back()); // the name is not there: the default
    audio.close();
    SDL_ResetHint(SDL_HINT_AUDIO_DRIVER);
    CHECK(input.right_touch() == s.right_touch);
    CHECK(keypad.mode() == fe::KeypadMode::kNever);
    CHECK_FALSE(rumble.on());
    CHECK(rumble.strength() == 30);
    CHECK(perf.on());
    // vsync off reached the window and, through follow_vsync, Game::vsync and the telemetry
    // (the paced clock's plan reads them); fullscreen's case applies the same Desktop::apply.
    CHECK_FALSE(desktop.vsync());
    CHECK_FALSE(vsync);
    CHECK_FALSE(stats.vsync());
    CHECK(lifecycle.pause_unfocused());
    CHECK(scales == std::vector<std::uint32_t>{3});
    CHECK(presenter.border().mode() == fe::BorderMode::kImage); // border and border_image (#157)
    CHECK(presenter.border().image_path() == "art.png");
    CHECK(layouts == std::vector<std::string>{fe::keypad_layout_text(s.keypad_layout)});
    // Nothing to reach: the row waits.
    CHECK_FALSE(fe::apply_live_setting(fe::LiveTargets{}, *fe::find_setting("scale_mode"), s));
    for (const char *key : {"keypad_layout", "border", "menu_repeat", "game_speed", "autosave"}) {
        CAPTURE(key);
        CHECK_FALSE(fe::apply_live_setting(fe::LiveTargets{}, *fe::find_setting(key), s));
    }
    // mute_unfocused off unmutes now; under --audio-capture it stays off.
    fe::FocusMute muted(true);
    muted.on_event(SDL_EVENT_WINDOW_FOCUS_LOST);
    CHECK(muted.muted());
    t.focus_mute = &muted;
    CHECK(fe::apply_live_setting(t, *fe::find_setting("mute_unfocused"), s));
    CHECK_FALSE(muted.muted());
    t.focus_mute_allowed = false;
    s.mute_unfocused = true;
    CHECK(fe::apply_live_setting(t, *fe::find_setting("mute_unfocused"), s));
    CHECK_FALSE(muted.enabled());
    for (const mt_audio_bus b : {MT_AUDIO_BUS_MASTER, MT_AUDIO_BUS_MUSIC, MT_AUDIO_BUS_EFFECTS}) {
        mt_audio_set_gain(b, 1.0f);
    }
    mt_audio_set_muted(false);
}

TEST_CASE("settings menu: the gear's place; the mouse wheel and the list's arrows scroll") {
    // 1920 x 1080 integer: 3x at (240, 60), bars left and right: the gear in the right bar.
    {
        const fe::Placement p = fe::place(fe::ScaleMode::kInteger, 1920, 1080, 480, 320);
        const fe::GearPlace g = fe::gear_place(p, 1);
        CHECK_FALSE(g.over_image);
        CHECK(g.rect.x == 1680 + 107);
        CHECK(g.rect.y == 4);
        CHECK(g.rect.w == 26);
    }
    // 4:3 fit: bars above and below: the gear in the top bar's right end.
    {
        const fe::Placement p = fe::place(fe::ScaleMode::kFit, 1024, 768, 480, 320);
        const fe::GearPlace g = fe::gear_place(p, 1);
        CHECK_FALSE(g.over_image);
        CHECK(g.rect.x == 1024 - 26 - 4);
        CHECK(g.rect.y + g.rect.h <= p.rect.y);
    }
    // A phone with a notch on each side (insets 47 points), at 3x density: in the safe area,
    // beside the image.
    {
        const fe::Insets in{0, 47, 21, 47};
        const fe::Placement p = fe::place(fe::ScaleMode::kInteger, 2532, 1170, 480, 320, in, 3, 3);
        const fe::GearPlace g = fe::gear_place(p, 3);
        CHECK_FALSE(g.over_image);
        CHECK(g.rect.w == 78);
        CHECK(g.rect.x >= p.rect.x + p.rect.w);
        CHECK(g.rect.x + g.rect.w <= p.safe.x + p.safe.w);
        CHECK(g.rect.y >= p.safe.y);
    }
    // No bars: over the image's top right corner.
    {
        const fe::Placement p = fe::place(fe::ScaleMode::kInteger, 960, 640, 480, 320);
        const fe::GearPlace g = fe::gear_place(p, 1);
        CHECK(g.over_image);
        CHECK(g.rect.x == 960 - 26 - 4);
        CHECK(g.rect.y == 4);
    }
    // Widescreen (#86): at 1280 x 720 integer the wide picture is 1136 wide at 72, the image
    // 960 at 160: the gear goes into the 72-pixel bar beside the wide picture, not on the wings.
    // At 1920 x 1080 fit the wide picture leaves no bar: beside the 3:2 image, as before.
    // Off (the wide placement is the placement): gear_place's.
    {
        const fe::Placement wide =
            fe::place(fe::ScaleMode::kInteger, 1280, 720, 568, 320, fe::Insets{}, 1, 1, 568);
        fe::Placement image = wide;
        image.rect = fe::wide_part(wide.rect, 568, 480);
        REQUIRE(wide.rect.x == 72);
        REQUIRE(image.rect.x == 160);
        const fe::GearPlace g = fe::gear_place(image, wide, 1);
        CHECK_FALSE(g.over_image);
        CHECK(g.rect.x >= wide.rect.x + wide.rect.w);
        CHECK(g.rect.x + g.rect.w <= 1280);
    }
    {
        const fe::Placement wide =
            fe::place(fe::ScaleMode::kFit, 1920, 1080, 568, 320, fe::Insets{}, 1, 1, 568);
        fe::Placement image = wide;
        image.rect = fe::wide_part(wide.rect, 568, 480);
        CHECK(fe::gear_place(wide, 1).over_image);
        const fe::GearPlace g = fe::gear_place(image, wide, 1);
        CHECK_FALSE(g.over_image);
        CHECK(g.rect.x >= image.rect.x + image.rect.w);
        CHECK(g.rect.x == fe::gear_place(image, 1).rect.x);
    }
    {
        const fe::Placement p = fe::place(fe::ScaleMode::kInteger, 1920, 1080, 480, 320);
        CHECK(fe::gear_place(p, p, 1).rect.x == fe::gear_place(p, 1).rect.x);
    }
    // The list scrolls: a tab with more rows than fit (synthetic: the registry's 16 rows in one
    // group).
    std::vector<SettingInfo> many(fe::settings_registry().begin(), fe::settings_registry().end());
    for (SettingInfo &r : many) {
        r.group = SettingGroup::kOther;
    }
    Settings live;
    SettingsMenu menu(many, nullptr, nullptr, nullptr);
    menu.attach(&live);
    Driver drv{menu, nullptr};
    drv.key(SDLK_F1);
    while (menu.tabs()[menu.tab()] != SettingGroup::kOther) {
        drv.key(SDLK_TAB);
    }
    REQUIRE(menu.items().size() > SettingsMenu::kVisibleRows + 1);
    SDL_Event wheel{};
    wheel.type = SDL_EVENT_MOUSE_WHEEL;
    wheel.wheel.y = -1;
    CHECK(menu.handle(wheel));
    CHECK(menu.first_visible() == 1);
    wheel.wheel.y = 1;
    menu.handle(wheel);
    CHECK(menu.first_visible() == 0);
    drv.tap(Device::kMouse, scroll_down_point());
    CHECK(menu.first_visible() == 1);
    // Down past the last visible row scrolls with it; Up from the first wraps to the preset.
    for (std::size_t i = 0; i < SettingsMenu::kVisibleRows + 2; i++) {
        drv.key(SDLK_DOWN);
    }
    CHECK(menu.selected() == SettingsMenu::kVisibleRows + 3); // from row 1
    CHECK(menu.first_visible() == 3);
    drv.key(SDLK_UP);
    CHECK(menu.first_visible() == 3);
    while (menu.selected() != 1) {
        drv.key(SDLK_UP);
    }
    CHECK(menu.first_visible() == 0);
    drv.key(SDLK_W);
    CHECK(menu.selected() == 0);
    // A press on one row and a release on another does nothing.
    const Settings before = live;
    menu.pointer(arrow_point(menu, 3, 1), true, false);
    menu.pointer(arrow_point(menu, 4, 1), false, true);
    CHECK(live == before);
}

TEST_CASE("settings menu: the drawing, by hash; every label and help fits") {
    REQUIRE(SDL_Init(SDL_INIT_GAMEPAD));
    const fs::path out = fs::path(output_dir()) / "settings_menu";
    std::error_code ec;
    fs::create_directories(out, ec);
    // Labels fit beside the values; tab names fit the bar.
    for (const SettingInfo &r : fe::settings_registry()) {
        CAPTURE(r.key);
        CHECK(fe::Canvas::text_width(r.label) <= SettingsMenu::kValueX - 24);
    }
    struct Case {
        const char *name;
        bool extra; // the synthetic rows (an Assists tab)
        int tabs;   // Tab presses after opening
        int downs;
        std::uint64_t hash;
    };
    // The tab bar has an Accessibility tab since #158's rows (colorblind_filter, screen_shake).
    // #179: Video has fullscreen_mode; every tab ends with Quit.
    const Case cases[] = {
        {"video", false, 0, 2, 0x376f9f95dc01ab3dull},
        {"controls", false, 2, 4, 0x57b84f95d110e575ull},
        {"assists", true, 4, 0, 0x13f1e0d844400b8aull},
    };
    fe::RecentStats recent;
    recent.frames = 60;
    recent.fps = 60;
    recent.frontend_ms = 2.5;
    recent.max_frontend_ms = 4.25;
    for (const Case &k : cases) {
        CAPTURE(k.name);
        Settings live;
        live.preset = fe::Preset::kEnhanced;
        live.hd_text = true;
        live.gapless_music = true;
        SettingsMenu menu(k.extra ? std::span<const SettingInfo>(kExtraRows)
                                  : fe::settings_registry(),
                          nullptr, nullptr, nullptr);
        menu.attach(&live);
        menu.set_started(Settings{});
        menu.set_frame_cost(recent);
        menu.set_action("bindings", [] { return std::string(); });
        Driver drv{menu, nullptr};
        drv.key(SDLK_F1);
        for (int i = 0; i < k.tabs; i++) {
            drv.key(SDLK_TAB);
        }
        for (int i = 0; i < k.downs; i++) {
            drv.key(SDLK_DOWN);
        }
        CHECK(menu.dirty());
        std::vector<std::uint8_t> px = backdrop(480, 320);
        menu.draw(px.data(), 480, 320, 1);
        CHECK_FALSE(menu.dirty());
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
        std::vector<std::uint8_t> px2 = backdrop(960, 640);
        menu.draw(px2.data(), 960, 640, 2);
        SDL_Surface *s2 =
            SDL_CreateSurfaceFrom(960, 640, SDL_PIXELFORMAT_ABGR8888, px2.data(), 960 * 4);
        CHECK((s2 != nullptr &&
               SDL_SavePNG(s2, (out / (std::string(k.name) + "_2x.png")).string().c_str())));
        SDL_DestroySurface(s2);
    }
    // The gear.
    std::vector<std::uint8_t> gear = fe::GearButton::pixels(52);
    SDL_Surface *s = SDL_CreateSurfaceFrom(52, 52, SDL_PIXELFORMAT_ABGR8888, gear.data(), 52 * 4);
    CHECK((s != nullptr && SDL_SavePNG(s, (out / "gear.png").string().c_str())));
    SDL_DestroySurface(s);
    char hex[32];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(fnv1a(gear)));
    MESSAGE("gear hash " << hex);
    CHECK(fnv1a(gear) == 0xa2a99d07b4fa4a0cull);
    SDL_Quit();
}

TEST_CASE("settings menu: the picture adjustments' test pattern behind the rows (#187)") {
    REQUIRE(SDL_Init(SDL_INIT_GAMEPAD));
    const fs::path out = fs::path(output_dir()) / "settings_menu";
    std::error_code ec;
    fs::create_directories(out, ec);
    Settings live;
    live.preset = fe::Preset::kEnhanced;
    SettingsMenu menu(fe::settings_registry(), nullptr, nullptr, nullptr);
    menu.attach(&live);
    menu.set_started(Settings{});
    Driver drv{menu, nullptr};
    drv.key(SDLK_F1);
    REQUIRE(fe::setting_group_name(menu.tabs()[menu.tab()]) == std::string("Video"));
    // Down to Brightness, the first row with the pattern.
    const auto selected_key = [&menu] {
        const SettingsMenu::Item &it = menu.items()[menu.selected()];
        return it.kind == SettingsMenu::ItemKind::kSetting ? std::string(it.setting->key)
                                                           : std::string();
    };
    // A row without the pattern: the list as before, everything dimmed.
    const std::vector<std::uint8_t> game = backdrop(480, 320);
    std::vector<std::uint8_t> plain = game;
    menu.draw(plain.data(), 480, 320, 1);
    for (int i = 0; i < 60 && selected_key() != "brightness"; i++) {
        drv.key(SDLK_DOWN);
    }
    REQUIRE(selected_key() == "brightness");
    std::vector<std::uint8_t> px = game;
    menu.draw(px.data(), 480, 320, 1);
    const auto at = [](const std::vector<std::uint8_t> &v, int x, int y) {
        return &v[(static_cast<std::size_t>(y) * 480 + x) * 4];
    };
    // The list's slots: those of the four picture rows on the panel, the first other one the
    // grey ramp, the rest the game's picture as it was (undimmed).
    const std::size_t top = menu.first_visible();
    int ramps = 0;
    int pictures = 0;
    int rows = 0;
    for (std::size_t r = 0; r < SettingsMenu::kVisibleRows; r++) {
        const std::size_t i = top + r + 1;
        const bool row = i < menu.items().size() &&
                         menu.items()[i].kind == SettingsMenu::ItemKind::kSetting &&
                         menu.items()[i].setting->test_pattern;
        const int y = SettingsMenu::kListY + static_cast<int>(r) * SettingsMenu::kRowH;
        const int mid = y + SettingsMenu::kRowH / 2;
        CAPTURE(r);
        if (row) {
            rows++;
            CHECK(std::memcmp(at(px, SettingsMenu::kListX + 2, y + 1),
                              at(game, SettingsMenu::kListX + 2, y + 1), 3) != 0);
            continue;
        }
        if (ramps == 0) {
            ramps++;
            // 16 steps across the list: black at the left, white at the right, greys between.
            const int step = SettingsMenu::kListW / 16;
            const int x0 = SettingsMenu::kListX + (SettingsMenu::kListW - 16 * step) / 2;
            for (int k = 0; k < 16; k++) {
                const std::uint8_t *p = at(px, x0 + k * step + step / 2, mid);
                const auto v = static_cast<std::uint8_t>(k * 17);
                CHECK(p[0] == v);
                CHECK(p[1] == v);
                CHECK(p[2] == v);
            }
            continue;
        }
        pictures++;
        // The game's own picture, byte for byte, across the slot.
        for (int x = SettingsMenu::kListX; x < SettingsMenu::kListX + SettingsMenu::kListW;
             x += 7) {
            CHECK(std::memcmp(at(px, x, mid), at(game, x, mid), 4) == 0);
        }
        CHECK(std::memcmp(at(plain, 100, mid), at(game, 100, mid), 3) != 0); // dimmed before
    }
    CHECK(rows >= 1);
    CHECK(rows <= 4);
    CHECK(ramps == 1);
    CHECK(pictures >= 1);
    // Outside the list, dimmed as before.
    CHECK(std::memcmp(at(px, 2, 2), at(plain, 2, 2), 4) == 0);
    CHECK(std::memcmp(at(px, 240, 300), at(plain, 240, 300), 4) == 0);
    const std::uint64_t h = fnv1a(px);
    char hex[32];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(h));
    SDL_Surface *s = SDL_CreateSurfaceFrom(480, 320, SDL_PIXELFORMAT_ABGR8888, px.data(), 480 * 4);
    const fs::path png = out / "video_picture.png";
    CHECK((s != nullptr && SDL_SavePNG(s, png.string().c_str())));
    SDL_DestroySurface(s);
    MESSAGE("video_picture: " << png.string() << " hash " << hex);
    CHECK(h == 0xa1de2d2d220a8c5eull);
    // Every picture row keeps the pattern; the next row without it brings the list back.
    const auto undimmed = [&](const std::vector<std::uint8_t> &v) {
        int n = 0;
        for (int y = SettingsMenu::kListY; y < SettingsMenu::kListY + 160; y += 4) {
            for (int x = 20; x < 440; x += 37) {
                n += std::memcmp(at(v, x, y), at(game, x, y), 4) == 0 ? 1 : 0;
            }
        }
        return n;
    };
    CHECK(undimmed(px) > 0);
    CHECK(undimmed(plain) == 0);
    for (const char *key : {"contrast", "saturation", "gamma"}) {
        drv.key(SDLK_DOWN);
        CHECK(selected_key() == key);
        std::vector<std::uint8_t> again = game;
        menu.draw(again.data(), 480, 320, 1);
        CHECK(undimmed(again) > 0);
    }
    drv.key(SDLK_DOWN);
    CHECK(selected_key() != "gamma");
    std::vector<std::uint8_t> back = game;
    menu.draw(back.data(), 480, 320, 1);
    CHECK(undimmed(back) == 0);
    SDL_Quit();
}
