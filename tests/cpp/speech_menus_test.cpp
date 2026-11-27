// The menus read aloud (#393, ADR 0190, speech_menus.h) on the log backend: the port's settings
// menu says each row and value once per move; the shell's buttons have a name for every image
// the focus reaches on a synthetic view tree of the shell's screens; the popup menu's and the
// shop's rows once per move, the description when the detail opens. Assertions use counts, ids
// and the menus' own strings, never game text. The real game's: input.speech_menus.
#include <SDL3/SDL.h>
#include <doctest/doctest.h>

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "focus.h"
#include "input.h"
#include "maplethief/input.h"
#include "settings.h"
#include "settings_menu.h"
#include "speech.h"
#include "speech_menus.h"

namespace {

namespace fe = maplethief::frontend;

SDL_Event key_event(SDL_Keycode k, bool down) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.key = k;
    e.key.down = down;
    return e;
}

struct Voice {
    fe::LogSpeechBackend *log = nullptr;
    fe::Speech speech;
    double now = 0;
    Voice() {
        auto backend = std::make_unique<fe::LogSpeechBackend>([](const std::string &) {});
        log = backend.get();
        speech.set_backend(std::move(backend));
        speech.set_enabled(true);
    }
    std::size_t count() const { return log->said().size(); }
    const std::string &last() const { return log->said().back().text; }
};

// The shell's screens as the runtime lists their buttons (mt_uikit_controls): each control by
// its action ("-[Target selector:]"), or by the image +imageNamed: was given (a name with
// ".png", in a folder or not, Korean twins included) with an action of its own. A synthetic
// tree: positions in a column, one screen after the other.
struct Screen {
    std::vector<std::string> controls;
};

std::vector<fe::FocusTarget> screen_targets(const Screen &s) {
    std::vector<fe::FocusTarget> out;
    for (std::size_t i = 0; i < s.controls.size(); i++) {
        fe::FocusTarget t;
        t.object = 0x1000 + static_cast<std::uint32_t>(i) * 0x10;
        t.order = static_cast<std::uint32_t>(i + 1);
        t.x = 20;
        t.y = 10 + 40 * static_cast<float>(i);
        t.w = 100;
        t.h = 30;
        t.px = t.cx();
        t.py = t.cy();
        const std::string &c = s.controls[i];
        t.view_class = "UIButton";
        if (c.rfind("-[", 0) == 0) { // "-[Target action]"
            const std::size_t space = c.find(' ');
            t.target = c.substr(2, space - 2);
            t.action = c.substr(space + 1, c.size() - space - 2);
        } else {
            t.target = "ShellView";
            t.action = "click" + std::to_string(i) + ":";
            t.image = c;
        }
        out.push_back(t);
    }
    return out;
}

} // namespace

TEST_CASE("speech menus: the settings menu says each row and value once per move (#393)") {
    Voice v;
    fe::Settings live;
    fe::SettingsMenu menu(fe::settings_registry(), nullptr, nullptr, nullptr);
    menu.attach(&live);
    fe::SpeechMenus sm;
    const auto key = [&](SDL_Keycode k) {
        menu.handle(key_event(k, true));
        sm.settings_turn(menu, v.speech, v.now += 0.5);
        menu.handle(key_event(k, false));
        sm.settings_turn(menu, v.speech, v.now += 0.5);
    };
    sm.settings_turn(menu, v.speech, v.now);
    CHECK(v.count() == 0); // closed: nothing
    key(SDLK_F1);
    REQUIRE(menu.is_open());
    REQUIRE(v.count() == 1); // opening says the tab and the row
    CHECK(v.last() == fe::settings_menu_line(menu, true));
    CHECK(v.last().find(fe::setting_group_name(menu.tabs()[menu.tab()])) == 0);

    // Down through the tab's rows: one utterance per move, the row's label and value.
    std::size_t moves = 0;
    for (std::size_t i = 1; i + 1 < menu.items().size(); i++) {
        const std::size_t before = menu.selected();
        const std::size_t said = v.count();
        key(SDLK_DOWN);
        if (menu.selected() == before) {
            CHECK(v.count() == said); // no move, nothing said
            continue;
        }
        moves++;
        CHECK(v.count() == said + 1);
        CHECK(v.last() == fe::settings_menu_line(menu, false));
        CHECK(v.last().find(menu.row_label(menu.selected())) == 0);
        CHECK(v.last().find(menu.row_value(menu.selected())) != std::string::npos);
    }
    CHECK(moves >= 3);
    CHECK(sm.stats().settings == moves + 1);

    // Back to the first setting row; a change of its value says the new value alone.
    while (menu.selected() > 1) {
        key(SDLK_UP);
    }
    REQUIRE(menu.items()[menu.selected()].kind == fe::SettingsMenu::ItemKind::kSetting);
    const std::string before_value = menu.row_value(menu.selected());
    std::size_t said = v.count();
    key(SDLK_RIGHT);
    if (menu.row_value(menu.selected()) != before_value) {
        CHECK(v.count() == said + 1);
        CHECK(v.last() == menu.row_value(menu.selected()));
    } else {
        CHECK(v.count() == said);
    }

    // A new tab: the tab's name, then its row.
    said = v.count();
    key(SDLK_TAB);
    CHECK(v.count() == said + 1);
    CHECK(v.last() == fe::settings_menu_line(menu, true));

    // Nothing moves: nothing is said again.
    said = v.count();
    for (int i = 0; i < 5; i++) {
        sm.settings_turn(menu, v.speech, v.now += 0.1);
    }
    CHECK(v.count() == said);

    // Closed, then opened again: the tab and row once more.
    key(SDLK_ESCAPE);
    CHECK(!menu.is_open());
    said = v.count();
    key(SDLK_F1);
    CHECK(v.count() == said + 1);

    // Speech off: nothing.
    v.speech.set_enabled(false);
    said = v.log->said().size();
    key(SDLK_DOWN);
    CHECK(v.log->said().size() == said);
}

TEST_CASE("speech menus: a change of a speech row leaves its sample alone (#390, #393)") {
    Voice v;
    fe::Settings live;
    fe::SettingsMenu menu(fe::settings_registry(), nullptr, nullptr, nullptr);
    menu.attach(&live);
    fe::SpeechMenus sm;
    const auto key = [&](SDL_Keycode k) {
        menu.handle(key_event(k, true));
        menu.handle(key_event(k, false));
        sm.settings_turn(menu, v.speech, v.now += 0.5);
    };
    key(SDLK_F1);
    // Find the speech_rate row (Accessibility).
    bool found = false;
    for (std::size_t t = 0; t < menu.tabs().size() && !found; t++) {
        for (std::size_t i = 0; i < menu.items().size() * 2 && !found; i++) {
            const fe::SettingInfo *s = menu.items()[menu.selected()].setting;
            found = s != nullptr && std::string(s->key) == "speech_rate";
            if (!found) {
                key(SDLK_DOWN);
            }
        }
        if (!found) {
            key(SDLK_TAB);
        }
    }
    REQUIRE(found);
    const std::size_t said = v.count();
    key(SDLK_RIGHT);
    CHECK(v.count() == said); // the frontend's apply says the sample (live_settings.cpp)
}

TEST_CASE("speech menus: shell button names; every control the focus reaches has one (#393)") {
    // The table's keys: an image's file name without folder, extension or Korean suffix, or an
    // action as the uikit trace writes it.
    CHECK(fe::shell_button_key("AlertYesButton.png") == "AlertYesButton");
    CHECK(fe::shell_button_key("/png/npc/npcAcceptButton_kor.png") == "npcAcceptButton");
    CHECK(fe::shell_button_key("") == "");
    CHECK(fe::shell_button_name("NoSuchImage.png") == nullptr);
    CHECK(fe::shell_button_name("-[NoView noAction:]") == nullptr);
    fe::FocusTarget plain;
    plain.target = "MainMenuView";
    plain.action = "clickPlay:";
    CHECK(fe::shell_button_lookup(plain) == "-[MainMenuView clickPlay:]");
    plain.image = "/png/TouchCloseButton_kor.png";
    CHECK(fe::shell_button_lookup(plain) == "TouchCloseButton"); // the image first
    std::set<std::string> keys;
    for (const fe::ShellButton &b : fe::shell_buttons()) {
        INFO("key " << b.key);
        CHECK(keys.insert(b.key).second); // once each
        const char *name = fe::shell_button_name(b.key);
        REQUIRE(name != nullptr);
        CHECK(*name != 0);
        if (b.key[0] != '-') {
            CHECK(fe::shell_button_key(b.key) == b.key);
            CHECK(fe::shell_button_name(std::string(b.key) + "_kor.png") == name);
        }
    }

    // The shell's screens (main menu, Help, About, the terms, the character select and their
    // alerts) as the focus lists them: a synthetic tree of every control the input.menu_* flows
    // focus (with MT_SPEECH_MENUS=1 they list the real ones and check each against the table).
    const std::vector<Screen> screens = {
        {{"-[MainMenuView clickPlay:]", "-[MainMenuView clickHelp:]", "-[MainMenuView clickAbout:]",
          "-[MainMenuView clickGameCenter:]", "-[MainMenuView clickMoreGame:]",
          "-[MainMenuView clickADButton:]", "-[MainMenuView clickADClose:]"}},
        {{"-[MainMenuView closeHelp:]"}},
        {{"TouchCloseButton.png", "-[MainMenuView clickLinkURL1:]",
          "-[MainMenuView clickLinkURL2:]", "-[MainMenuView clickLinkURL3:]",
          "-[MainMenuView clickLinkURL4:]", "-[MainMenuView clickAboutBuild:]",
          "-[MainMenuView closeAbout:]"}},
        {{"TermsButton_kor.png", "-[TermsView releaseAgreementButton:]"}},
        {{"-[ChooseCharacterView chooseCharacterOne:]", "-[ChooseCharacterView newCharacterTwo:]",
          "-[ChooseCharacterView clickStart:]", "-[ChooseCharacterView clickNew:]",
          "-[ChooseCharacterView clickDelete:]", "-[ChooseCharacterView clickBack:]",
          "ChooseCharacterBackButton.png"}},
        {{"-[ChooseCharacterView newCharacterOne:]", "-[ChooseCharacterView chooseCharacterTwo:]",
          "-[ChooseCharacterView chooseCharacterThree:]",
          "-[ChooseCharacterView newCharacterThree:]"}},
        {{"-[ChooseCharacterView cancelNewAlert:]"}},
        {{"cash_meso_3000.png", "cash_meso_10000.png", "cash_meso_22000.png", "cash_meso_50000.png",
          "cash_meso_120000.png", "cash_meso_300000.png", "cash_exp_40.png", "cash_exp_80.png",
          "-[TouchKeyPadView releaseClearButton:]"}},
        {{"-[ChooseCharacterView okDeleteAlert:]", "-[ChooseCharacterView cancelAlert:]"}},
        {{"-[ChooseCharacterView confirmAlert:]"}},
        {{"-[ChooseCharacterView okNewAlert:]"}},
        {{"-[TouchKeyPadView releaseOkButton:]"}},
        {{"AlertYesButton.png", "AlertNoButton.png"}},
        {{"/png/AlertOKButton.png"}},
        {{"map.png"}}, // the key pad's map button, reachable while a game loads (panel -1)
    };
    Voice v;
    fe::Input input;
    fe::Focus focus(&input);
    std::vector<fe::FocusTarget> targets;
    focus.set_panel_source([](void *) { return std::int32_t{-1}; }, nullptr);
    focus.set_target_source(
        [](void *user, mt_cpu *, std::vector<fe::FocusTarget> *out) {
            *out = *static_cast<std::vector<fe::FocusTarget> *>(user);
        },
        &targets);
    fe::SpeechMenus sm;
    double now = 0;
    const auto turn = [&] {
        now += 1.0 / 60;
        focus.turn(nullptr, now);
        sm.shell_turn(focus, {}, v.speech, now);
    };
    // Each change of the focused control (a screen opening, a move up or down) is said once;
    // a press that moves nothing says nothing.
    std::size_t controls = 0;
    std::uint32_t last = 0;
    const auto seen = [&] {
        const fe::FocusTarget *f = focus.focused();
        if (f != nullptr && f->object != last) {
            controls++;
        }
        last = f != nullptr ? f->object : 0;
        CHECK(v.count() == controls);
    };
    const auto press = [&](std::uint32_t action) {
        focus.action(action, true);
        turn();
        seen();
        focus.action(action, false);
        turn();
        seen();
        turn();
        seen();
    };
    for (const Screen &s : screens) {
        targets = screen_targets(s);
        for (const fe::FocusTarget &t : targets) {
            INFO("control " << fe::shell_button_lookup(t));
            CHECK(fe::shell_button_name(t) != nullptr);
        }
        turn(); // the screen opens: the focus on one of its controls, said once
        seen();
        REQUIRE(focus.focused() != nullptr);
        // Up to the top, then down over every control to the bottom and once past it.
        for (std::size_t i = 0; i < targets.size(); i++) {
            press(MT_ACTION_UP);
        }
        for (std::size_t i = 0; i <= targets.size(); i++) {
            press(MT_ACTION_DOWN);
        }
        targets.clear();
        turn(); // the screen closes
        seen();
        last = 0;
    }
    CHECK(controls >= 34);
    CHECK(sm.stats().controls == controls);
    CHECK(sm.stats().unnamed == 0);
}

TEST_CASE("speech menus: a control's label, a screen's title, nothing while the focus is off") {
    Voice v;
    fe::Input input;
    fe::Focus focus(&input);
    std::vector<fe::FocusTarget> targets = screen_targets(Screen{{"Unknown.png", "Other.png"}});
    std::int32_t panel = -1;
    focus.set_panel_source([](void *user) { return *static_cast<std::int32_t *>(user); }, &panel);
    focus.set_target_source(
        [](void *user, mt_cpu *, std::vector<fe::FocusTarget> *out) {
            *out = *static_cast<std::vector<fe::FocusTarget> *>(user);
        },
        &targets);
    // A title above the buttons, and a label inside the first button.
    std::vector<fe::ScreenLabel> labels = {
        {0x900, 1, 20, 0, 200, 8, "Title"},
        {0x910, 5, 30, 15, 50, 20, "Inside"},
    };
    fe::SpeechMenus sm;
    double now = 0;
    focus.turn(nullptr, now += 0.1);
    sm.shell_turn(focus, labels, v.speech, now);
    REQUIRE(v.count() == 1);
    CHECK(v.last() == "Title. Inside"); // the title first, then the focused control
    CHECK(sm.stats().titles == 1);
    // A move to the second button, which has neither a table name nor a label: nothing said.
    focus.action(MT_ACTION_DOWN, true);
    focus.turn(nullptr, now += 0.1);
    sm.shell_turn(focus, labels, v.speech, now);
    CHECK(v.count() == 1);
    CHECK(sm.stats().unnamed == 1);
    focus.action(MT_ACTION_DOWN, false);
    // An alert's label appears: its title alone.
    labels.push_back({0x920, 9, 100, 100, 100, 20, "Alert"});
    focus.turn(nullptr, now += 0.1);
    sm.shell_turn(focus, labels, v.speech, now);
    CHECK(v.count() == 2);
    CHECK(v.last() == "Alert");
    // The focus goes off (a key pad panel): nothing, and coming back says the screen again.
    panel = 0;
    focus.turn(nullptr, now += 0.1);
    sm.shell_turn(focus, labels, v.speech, now);
    CHECK(v.count() == 2);
    panel = -1;
    focus.turn(nullptr, now += 0.1);
    sm.shell_turn(focus, labels, v.speech, now);
    CHECK(v.count() == 3);
}

TEST_CASE("speech menus: the popup menu's and the shop's rows once per move; a description on "
          "the detail") {
    Voice v;
    fe::SpeechMenus sm;
    using K = fe::MenuSpot::Kind;
    const auto spot = [](K kind, int row, int id, bool detail = false, int price = -1) {
        fe::MenuSpot s;
        s.kind = kind;
        s.row = row;
        s.id = id;
        s.detail = detail;
        s.price = price;
        return s;
    };
    const fe::SpotText text{"Name", "Description"};
    double now = 0;
    sm.spot_turn(spot(K::kItem, 0, 3), text, v.speech, now += 0.1);
    CHECK(v.count() == 1);
    sm.spot_turn(spot(K::kItem, 0, 3), text, v.speech, now += 0.1); // the same row
    CHECK(v.count() == 1);
    sm.spot_turn(spot(K::kItem, 1, 6), text, v.speech, now += 0.1);
    CHECK(v.count() == 2);
    sm.spot_turn(spot(K::kItem, 1, 6, true), text, v.speech, now += 0.1); // the detail opens
    CHECK(v.count() == 3);
    CHECK(v.last() == text.description);
    sm.spot_turn(spot(K::kItem, 1, 6, true), text, v.speech, now += 0.1);
    CHECK(v.count() == 3);
    sm.spot_turn(spot(K::kSkill, 1, 6), text, v.speech, now += 0.1); // another tab, same row
    CHECK(v.count() == 4);
    sm.spot_turn(fe::MenuSpot{}, text, v.speech, now += 0.1); // the menu closes
    CHECK(v.count() == 4);
    sm.spot_turn(spot(K::kShopBuy, 2, 32, false, 120), text, v.speech, now += 0.1);
    CHECK(v.count() == 5);
    CHECK(v.last() == fe::menu_spot_line(spot(K::kShopBuy, 2, 32, false, 120), text));
    CHECK(v.last().find("120") != std::string::npos);
    CHECK(sm.stats().spots == 4);
    CHECK(sm.stats().details == 1);
    // A row without a name in the tables says nothing.
    sm.spot_turn(spot(K::kShopBuy, 3, 99), fe::SpotText{}, v.speech, now += 0.1);
    CHECK(v.count() == 5);
    // Speech off: nothing, and nothing kept.
    v.speech.set_enabled(false);
    sm.spot_turn(spot(K::kShopBuy, 4, 1), text, v.speech, now += 0.1);
    CHECK(v.log->said().size() == 5);
}

TEST_CASE("speech menus: no guest, no spot") {
    CHECK(fe::read_menu_spot(nullptr).kind == fe::MenuSpot::Kind::kNone);
    CHECK(fe::spot_text(nullptr, fe::MenuSpot{}).name.empty());
    CHECK(fe::runtime_labels(nullptr).empty());
    CHECK(std::string(fe::menu_spot_kind_name(fe::MenuSpot::Kind::kSkill)) == "skill");
}
