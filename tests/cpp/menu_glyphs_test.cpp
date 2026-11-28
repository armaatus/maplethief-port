// The port's menus in other letters (#372, ADR 0206), synthetic text only:
//
//   the canvas      printable ASCII in the 5x7 font as before; accented Latin and Hangul from the
//                   bundled fonts (NanumGothic, Fira Sans), in cells (Hangul two); a character
//                   neither font has, a control and a byte that is not UTF-8 as '?'; cuts at whole
//                   characters
//   the pseudo-translation
//                   a pack whose Port.strings gives every text of the port (the table, the
//                   registry's labels and help, the menu's action rows) accents on every letter it
//                   can and Hangul syllables a third longer than the English: every overlay
//                   (settings menu tabs, a list view, the preset picker, the controls card, the
//                   remap overlay, the disconnect pause, the key pad editor, the start question,
//                   the import screen) drawn at UI scales 1 to 4 with no '?', every text inside
//                   the screen and its panel and none over another; golden images at 1x and 2x
//                   (tests/golden/data/menu_glyphs/), with a tolerance for stb_truetype's float
//                   rasterization.
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "canvas.h"
#include "controls_card.h"
#include "crash_report.h"
#include "disconnect.h"
#include "golden.h"
#include "import_screen.h"
#include "input.h"
#include "keypad_editor.h"
#include "keypad_layout.h"
#include "maplethief/textpack.h"
#include "port_text.h"
#include "preset_picker.h"
#include "prompts.h"
#include "recovery.h"
#include "remap.h"
#include "settings.h"
#include "settings_menu.h"
#include "start_question.h"

namespace fe = maplethief::frontend;
namespace golden = maplethief::golden;
namespace fs = std::filesystem;
using fe::Canvas;

namespace {

// A text the overlay drew, with the draw it belongs to.
struct Seen {
    int x, y, w, h;
    std::string text;
    int missing;
};

struct Observer {
    std::vector<Seen> seen;
    Observer() {
        fe::set_text_observer(
            [](void *user, const fe::DrawnText &t) {
                static_cast<Observer *>(user)->seen.push_back(
                    {t.x, t.y, t.w, t.h, std::string(t.text), t.missing});
            },
            this);
    }
    ~Observer() { fe::set_text_observer(nullptr, nullptr); }
    Observer(const Observer &) = delete;
    Observer &operator=(const Observer &) = delete;
};

// A flat stand-in for the game's picture (the goldens stay small).
std::vector<std::uint8_t> backdrop(std::uint32_t w, std::uint32_t h) {
    std::vector<std::uint8_t> px(std::size_t{w} * h * 4);
    for (std::size_t i = 0; i < px.size(); i += 4) {
        px[i] = 0x50, px[i + 1] = 0x78, px[i + 2] = 0x5A, px[i + 3] = 255;
    }
    return px;
}

golden::Image image_of(const std::vector<std::uint8_t> &px, int w, int h) {
    golden::Image img(w, h);
    img.bytes() = px;
    return img;
}

// --- the pseudo-translation --------------------------------------------------------------------

// Each ASCII letter that has an accented form gets one; places ({0}) and escapes stay; then
// Hangul syllables, two cells each, until the text is a third wider than the English.
std::string pseudo(const std::string &english) {
    static const char *const kLower[26] = {
        "\xC3\xA1", "b", "\xC3\xA7", "d", "\xC3\xA9", "f",        "g",        "h",       "\xC3\xAD",
        "j",        "k", "l",        "m", "\xC3\xB1", "\xC3\xB6", "p",        "q",       "r",
        "\xC5\xA1", "t", "\xC3\xBC", "v", "w",        "x",        "\xC3\xBD", "\xC5\xBE"};
    static const char *const kUpper[26] = {
        "\xC3\x81", "B", "\xC3\x87", "D", "\xC3\x89", "F",        "G",        "H",       "\xC3\x8E",
        "J",        "K", "L",        "M", "\xC3\x91", "\xC3\x98", "P",        "Q",       "R",
        "\xC5\xA0", "T", "\xC3\x9C", "V", "W",        "X",        "\xC3\x9D", "\xC5\xBD"};
    static const char *const kHangul[] = {"\xEA\xB0\x80", "\xEB\x82\x98", "\xEB\x8B\xA4",
                                          "\xEB\x9D\xBC", "\xEB\xA7\x88", "\xEB\xB0\x94",
                                          "\xEC\x82\xAC", "\xED\x95\x9C", "\xEA\xB8\x80"};
    std::string out;
    int depth = 0; // inside a place
    for (const char ch : english) {
        depth += ch == '{' ? 1 : ch == '}' ? -1 : 0;
        if (depth == 0 && ch >= 'a' && ch <= 'z') {
            out += kLower[ch - 'a'];
        } else if (depth == 0 && ch >= 'A' && ch <= 'Z') {
            out += kUpper[ch - 'A'];
        } else {
            out.push_back(ch);
        }
    }
    const int extra = (Canvas::columns(english) + 2) / 3;
    if (extra > 0) {
        out += ' ';
        for (int i = 0; i + 1 < extra; i += 2) {
            out += kHangul[(i / 2) % (sizeof kHangul / sizeof kHangul[0])];
        }
    }
    return out;
}

// A .strings value: quotes, backslashes and line breaks escaped.
std::string quoted(const std::string &s) {
    std::string out = "\"";
    for (const char ch : s) {
        if (ch == '"' || ch == '\\') {
            out.push_back('\\');
            out.push_back(ch);
        } else if (ch == '\n') {
            out += "\\n";
        } else {
            out.push_back(ch);
        }
    }
    return out + "\"";
}

struct PseudoPack {
    fs::path dir;
    PseudoPack() {
        dir = golden::test_output_dir() / "menu_glyphs_pack";
        fs::remove_all(dir);
        fs::create_directories(dir);
        std::ostringstream port;
        const auto entry = [&port](const std::string &key, const char *english) {
            if (english != nullptr && english[0] != '\0') {
                port << quoted(key) << " = " << quoted(pseudo(english)) << ";\n";
            }
        };
        for (std::size_t i = 0; i < static_cast<std::size_t>(fe::Txt::kCount); i++) {
            const auto id = static_cast<fe::Txt>(i);
            entry(fe::port_text_key(id), fe::port_text_english(id));
        }
        for (const fe::SettingInfo &r : fe::settings_registry()) {
            entry(std::string("setting.") + r.key + ".label", r.label);
            entry(std::string("setting.") + r.key + ".help", r.help);
        }
        for (const fe::MenuActionInfo &a : fe::menu_actions()) {
            const std::string k = std::string("action.") + a.key + ".";
            entry(k + "label", a.label);
            entry(k + "help", a.help);
            entry(k + "help_pad", a.help_pad);
            entry(k + "button", a.button);
            entry(k + "confirm", a.confirm);
        }
        std::ofstream(dir / "textpack.strings", std::ios::binary)
            << "\"format\" = \"1\";\n\"name\" = \"Pseudo\";\n\"language\" = \"x-pseudo\";\n";
        std::ofstream(dir / "Port.strings", std::ios::binary) << port.str();
        REQUIRE(mt_textpack_open(dir.string().c_str()) >= 0);
        fe::reload_port_text();
    }
    ~PseudoPack() {
        mt_textpack_close();
        fe::reload_port_text();
        fs::remove_all(dir);
    }
    PseudoPack(const PseudoPack &) = delete;
    PseudoPack &operator=(const PseudoPack &) = delete;
};

// --- the checks --------------------------------------------------------------------------------

struct Box {
    int x, y, w, h;
};

bool overlaps(const Seen &a, const Seen &b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

// Which of an overlay's draws have a golden image.
enum class Goldens { kNone, k1x, k1x2x };
// Off: no draw is checked against a golden (the shipped languages' runs, #373).
bool g_goldens = true;

// One overlay's draw at scales 1 to 4: no '?', every text inside `panel` and none over another;
// the same texts at every scale; 1x (and 2x) checked against their goldens.
void check_overlay(
    const std::string &name, Box panel,
    const std::function<void(std::uint8_t *, std::uint32_t, std::uint32_t, int)> &draw,
    Goldens goldens = Goldens::k1x, int max_scale = 4) {
    CAPTURE(name);
    std::vector<Seen> first;
    for (int scale = 1; scale <= max_scale; scale++) {
        CAPTURE(scale);
        const auto w = static_cast<std::uint32_t>(480 * scale);
        const auto h = static_cast<std::uint32_t>(320 * scale);
        std::vector<std::uint8_t> px = backdrop(w, h);
        Observer obs;
        draw(px.data(), w, h, scale);
        REQUIRE_FALSE(obs.seen.empty());
        for (std::size_t i = 0; i < obs.seen.size(); i++) {
            const Seen &t = obs.seen[i];
            CAPTURE(t.text);
            CAPTURE(t.x);
            CAPTURE(t.y);
            CAPTURE(t.w);
            CHECK(t.missing == 0);
            CHECK(t.x >= panel.x);
            CHECK(t.y >= panel.y);
            CHECK(t.x + t.w <= panel.x + panel.w);
            CHECK(t.y + t.h <= panel.y + panel.h);
            for (std::size_t k = i + 1; k < obs.seen.size(); k++) {
                if (obs.seen[k].w > 0 && t.w > 0) {
                    CAPTURE(obs.seen[k].text);
                    CHECK_FALSE(overlaps(t, obs.seen[k]));
                }
            }
        }
        if (scale == 1) {
            first = obs.seen;
        } else {
            CHECK(obs.seen.size() == first.size());
        }
        if (g_goldens && ((scale == 1 && goldens != Goldens::kNone) ||
                          (scale == 2 && goldens == Goldens::k1x2x))) {
            const std::string file = name + (scale == 1 ? "" : "_2x") + ".png";
            const auto r =
                golden::check_golden(image_of(px, static_cast<int>(w), static_cast<int>(h)),
                                     fs::path(MT_GOLDEN_DATA_DIR) / "menu_glyphs" / file,
                                     std::string("menu_glyphs/") + file, {24, 0.002});
            CHECK_MESSAGE(r.passed, r.message);
        }
    }
}

SDL_Event key_event(SDL_Keycode k, bool down) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.key = k;
    e.key.down = down;
    return e;
}

// Every overlay (#372): each settings tab, a list view, the preset picker, the controls card
// for touch, keyboard and gamepad, the remap overlay, the disconnect pause, the key pad editor,
// both start questions, the import screen waiting and refused; `text` makes the synthetic texts
// (a list's rows, a refusal's reason) as the pack under test would.
void every_overlay(const std::function<std::string(const std::string &)> &text) {
    const Box screen{0, 0, 480, 320};
    const Box menu_panel{8, 6, 464, 308};
    // The settings menu: each tab, and a list view.
    {
        fe::Settings live;
        live.preset = fe::Preset::kEnhanced;
        fe::SettingsMenu menu(fe::settings_registry(), nullptr, nullptr, nullptr);
        menu.attach(&live);
        menu.set_started(fe::Settings{});
        menu.open();
        const std::size_t tabs = menu.tabs().size();
        for (std::size_t t = 0; t < tabs; t++) {
            const std::string name = "settings_" + std::to_string(t);
            check_overlay(
                name, menu_panel,
                [&menu](std::uint8_t *px, std::uint32_t w, std::uint32_t h, int s) {
                    menu.draw(px, w, h, s);
                },
                t == 0 ? Goldens::k1x2x : Goldens::kNone); // the other tabs: checked, no image
            menu.handle(key_event(SDLK_TAB, true));
            menu.handle(key_event(SDLK_TAB, false));
        }
        // A list view (the Saves tab's kind): its title, rows and help come from elsewhere,
        // here synthetic and long.
        fe::MenuList list;
        list.title = text("Slot 1 of 3: the saves on this computer, newest first");
        list.help = text("Choose a save to load it; the game asks before it overwrites "
                         "anything, and keeps a copy of what it replaces");
        for (int i = 0; i < 12; i++) {
            list.rows.push_back({text("Quick save " + std::to_string(i) + " in Henesys"),
                                 text("26 Nov 2026 10:0" + std::to_string(i % 10))});
        }
        menu.open_list(list);
        check_overlay("settings_list", menu_panel,
                      [&menu](std::uint8_t *px, std::uint32_t w, std::uint32_t h, int s) {
                          menu.draw(px, w, h, s);
                      });
    }
    {
        fe::PresetPicker picker(false, true);
        check_overlay("preset_picker", screen,
                      [&picker](std::uint8_t *px, std::uint32_t w, std::uint32_t h, int s) {
                          picker.draw(px, w, h, s);
                      });
    }
    {
        fe::Input input;
        fe::Actions actions(&input);
        for (const auto device :
             {fe::PromptDevice::kNone, fe::PromptDevice::kKeyboard, fe::PromptDevice::kGamepad}) {
            fe::ControlsCard card(&actions, nullptr);
            card.seed_device(device);
            card.open();
            check_overlay(std::string("controls_card_") + fe::prompt_device_name(device), screen,
                          [&card](std::uint8_t *px, std::uint32_t w, std::uint32_t h, int s) {
                              card.draw(px, w, h, s);
                          });
        }
    }
    {
        fe::Remap remap(nullptr, nullptr);
        remap.open(0);
        check_overlay("remap", screen,
                      [&remap](std::uint8_t *px, std::uint32_t w, std::uint32_t h, int s) {
                          remap.draw(px, w, h, s);
                      });
    }
    {
        fe::DisconnectPause pause(nullptr, nullptr);
        check_overlay("disconnect", screen,
                      [&pause](std::uint8_t *px, std::uint32_t w, std::uint32_t h, int s) {
                          pause.draw(px, w, h, s);
                      });
    }
    {
        fe::KeypadEditor editor;
        editor.open(fe::KeypadLayout{});
        check_overlay("keypad_editor", screen,
                      [&editor](std::uint8_t *px, std::uint32_t w, std::uint32_t h, int s) {
                          editor.draw(px, w, h, s);
                      });
    }
    {
        fe::StartQuestion q(fe::safe_start_question_text("/data/MapleThief/crashes/a.txt"),
                            {fe::start_choice_label(fe::SafeStartChoice::kDefaults),
                             fe::start_choice_label(fe::SafeStartChoice::kNormal)});
        check_overlay("start_question", screen,
                      [&q](std::uint8_t *px, std::uint32_t w, std::uint32_t h, int s) {
                          q.draw(px, w, h, s);
                      });
        std::vector<std::string> buttons;
        for (const auto c :
             {fe::RecoveryChoice::kResume, fe::RecoveryChoice::kResumeDefaults,
              fe::RecoveryChoice::kStartDefaults, fe::RecoveryChoice::kStartNormal}) {
            buttons.emplace_back(fe::start_choice_label(c));
        }
        fe::RecoveryOffer offer;
        offer.path = "states/recovery-1.state";
        offer.slot = 1;
        offer.minutes_before = 3;
        fe::StartQuestion four(fe::recovery_question_text("/data/MapleThief/crashes/a.txt", offer),
                               std::move(buttons));
        check_overlay("start_question_recovery", screen,
                      [&four](std::uint8_t *px, std::uint32_t w, std::uint32_t h, int s) {
                          four.draw(px, w, h, s);
                      });
    }
    {
        fe::ImportScreen import("/data/MapleThief", nullptr);
        check_overlay("import_screen", screen,
                      [&import](std::uint8_t *px, std::uint32_t w, std::uint32_t h, int s) {
                          import.draw(px, w, h, s);
                      });
        // Refused: a synthetic importer's reason, then the SD-card route's question.
        fe::ImportScreen refused(
            "/data/MapleThief", [text](const std::string &, const fe::IpaProgress &) {
                fe::IpaResult r;
                r.problem = fe::IpaProblem::kNotIpa;
                r.why = text("This file is not the iPhone game's version 1.5: its "
                             "executable has another size and another checksum");
                return r;
            });
        REQUIRE(refused.start("/Users/player/Downloads/MapleThief-1.5.ipa"));
        refused.wait();
        check_overlay("import_screen_refused", screen,
                      [&refused](std::uint8_t *px, std::uint32_t w, std::uint32_t h, int s) {
                          refused.draw(px, w, h, s);
                      });
    }
}

} // namespace

TEST_CASE("menu glyphs: cells, whole-character cuts, UTF-8 decoding (#372)") {
    CHECK(Canvas::columns("Settings") == 8);
    CHECK(Canvas::text_width("Settings") == 47); // as before: 6 a character less the last gap
    CHECK(Canvas::columns("P\xC3\xB6tion") == 6);
    CHECK(Canvas::columns("\xED\x95\x9C\xEA\xB8\x80") == 4); // two Hangul syllables
    CHECK(Canvas::columns("a\xFF") == 2);                    // a stray byte: one cell
    CHECK(Canvas::fit_columns("P\xC3\xB6tion", 2) == 3);     // "Pö", never half of "ö"
    CHECK(Canvas::fit_columns("\xED\x95\x9C\xEA\xB8\x80", 3) == 3);
    CHECK(Canvas::fit_columns("abc", 9) == 3);
    std::size_t at = 0;
    CHECK(fe::next_code_point("\xC0\xAF", &at) == -1); // overlong
    CHECK(at == 1);
    at = 0;
    CHECK(fe::next_code_point("\xED\xA0\x80", &at) == -1); // a surrogate
    at = 0;
    CHECK(fe::next_code_point("\xF0\x9F\x98\x80", &at) == 0x1F600);
    CHECK(at == 4);
}

TEST_CASE("menu glyphs: accents and Hangul from the bundled fonts, '?' for what neither has") {
    Observer obs;
    const auto draw = [](const char *s, int scale) {
        std::vector<std::uint8_t> px(std::size_t{60} * 20 * 4 * scale * scale, 0);
        Canvas c(px.data(), 60u * scale, 20u * scale, scale);
        c.text(2, 4, s, {255, 255, 255, 255});
        return px;
    };
    // An accented letter is not its base letter, nor '?'.
    CHECK(draw("\xC3\xA9", 1) != draw("e", 1));
    CHECK(draw("\xC3\xA9", 1) != draw("?", 1));
    CHECK(draw("\xED\x95\x9C", 2) != draw("??", 2));
    obs.seen.clear();
    draw("H\xC3\xA9n\xC3\xA9sys \xED\x95\x9C\xEA\xB8\x80", 1);
    REQUIRE(obs.seen.size() == 1);
    CHECK(obs.seen[0].missing == 0);
    CHECK(obs.seen[0].w == (7 + 1 + 4) * 6 - 1);
    obs.seen.clear();
    draw("\xF0\x9F\x98\x80 a\xFF", 1); // an emoji neither font has, a stray byte
    CHECK(obs.seen[0].missing == 2);
    // The line at 1x to 3x: a golden of the canvas alone.
    const char *line = "Einstellungen f\xC3\xBCr Gr\xC3\xB6\xC3\x9F"
                       "e \xC3\x85\xC3\x86\xC5\x93 "
                       "\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4 \xE2\x80\x9Cq\xE2\x80\x9D\xE2\x80\xA6";
    for (int scale = 1; scale <= 3; scale++) {
        CAPTURE(scale);
        const int w = 240 * scale, h = 14 * scale;
        std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4, 0);
        for (std::size_t i = 3; i < px.size(); i += 4) {
            px[i - 3] = 0x20, px[i - 2] = 0x24, px[i - 1] = 0x2C, px[i] = 255;
        }
        Canvas c(px.data(), static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), scale);
        c.text(2, 3, line, {0xF0, 0xF2, 0xF5, 255});
        const std::string file = "line_" + std::to_string(scale) + "x.png";
        const auto r = golden::check_golden(image_of(px, w, h),
                                            fs::path(MT_GOLDEN_DATA_DIR) / "menu_glyphs" / file,
                                            "menu_glyphs/" + file, {24, 0.005});
        CHECK_MESSAGE(r.passed, r.message);
    }
}

TEST_CASE("menu glyphs: a pseudo-translation draws every overlay without '?' or clipping") {
    REQUIRE(SDL_Init(SDL_INIT_GAMEPAD));
    const PseudoPack pack;
    // The pack's texts are what the overlays get.
    CHECK(std::string(fe::tr(fe::Txt::menu_title)).find("\xC3\x89") != std::string::npos);

    every_overlay(pseudo);
    SDL_Quit();
}

namespace {

// One shipped language over every overlay (at UI scales 1 to 4), then every row of every settings
// tab selected so that its help line draws too (at 1x: the layout counts the same cells at every
// scale, and the sweep above checks the first rows at all four).
void shipped_language_overlays(const char *tag) {
    REQUIRE(SDL_Init(SDL_INIT_GAMEPAD));
    g_goldens = false;
    REQUIRE(fe::set_menu_language(tag));
    CHECK(std::string(fe::tr(fe::Txt::menu_title)) != "SETTINGS");
    every_overlay([](const std::string &s) { return s; });
    fe::Settings live;
    live.preset = fe::Preset::kEnhanced;
    fe::SettingsMenu menu(fe::settings_registry(), nullptr, nullptr, nullptr);
    menu.attach(&live);
    menu.set_started(fe::Settings{});
    menu.open();
    for (std::size_t t = 0; t < menu.tabs().size(); t++) {
        for (std::size_t row = 0; row < menu.items().size(); row++) {
            check_overlay(
                "settings_" + std::to_string(t) + "_" + std::to_string(row), {8, 6, 464, 308},
                [&menu](std::uint8_t *px, std::uint32_t w, std::uint32_t h, int s) {
                    menu.draw(px, w, h, s);
                },
                Goldens::kNone, 1);
            menu.handle(key_event(SDLK_DOWN, true));
            menu.handle(key_event(SDLK_DOWN, false));
        }
        menu.handle(key_event(SDLK_TAB, true));
        menu.handle(key_event(SDLK_TAB, false));
    }
    fe::set_menu_language("en");
    g_goldens = true;
    SDL_Quit();
}

} // namespace

TEST_CASE("menu glyphs: the Dutch menus draw every overlay and settings row without '?' or "
          "clipping (#373)") {
    shipped_language_overlays("nl");
}

TEST_CASE("menu glyphs: the Korean menus draw every overlay and settings row without '?' or "
          "clipping (#373)") {
    shipped_language_overlays("ko");
}
