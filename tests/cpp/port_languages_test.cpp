// The port's shipped menu languages (#373, ADR 0222, port_text.h): platform/lang/<tag>/ holds a
// text pack of our own text for each, embedded at build time. Every port text (port_text.def,
// the registry's labels and help, the menu actions' fields) has a translation with the
// English's places in each; the setting's names are the shipped folders; menu_language = auto
// follows the system's preferred languages (SDL_PREFERRED_LOCALES stands in for the system
// here), so a first start without settings.ini is in the system's language from the import
// screen on. The layout of each language over every overlay: menu_glyphs_test.cpp.
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "canvas.h"
#include "controls_card.h"
#include "game.h"
#include "import_screen.h"
#include "input.h"
#include "port_text.h"
#include "preset_picker.h"
#include "settings.h"

namespace fe = maplethief::frontend;
using fe::Txt;

namespace {

namespace fs = std::filesystem;

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

// The system's preferred languages for a test: SDL's hint, which SDL_GetPreferredLocales reads
// before the system's; cleared afterwards, and the menu back in English.
struct SystemLanguages {
    explicit SystemLanguages(const char *csv) {
        REQUIRE(SDL_SetHint(SDL_HINT_PREFERRED_LOCALES, csv));
    }
    ~SystemLanguages() {
        SDL_ResetHint(SDL_HINT_PREFERRED_LOCALES);
        fe::set_menu_language("en");
    }
    SystemLanguages(const SystemLanguages &) = delete;
    SystemLanguages &operator=(const SystemLanguages &) = delete;
};

// The texts an overlay drew (the Canvas's observer).
struct Drawn {
    std::vector<std::string> texts;
    Drawn() {
        fe::set_text_observer(
            [](void *user, const fe::DrawnText &t) {
                static_cast<Drawn *>(user)->texts.emplace_back(t.text);
            },
            this);
    }
    ~Drawn() { fe::set_text_observer(nullptr, nullptr); }
    Drawn(const Drawn &) = delete;
    Drawn &operator=(const Drawn &) = delete;
    bool has(const std::string &s) const {
        return std::find(texts.begin(), texts.end(), s) != texts.end();
    }
};

// What `draw` puts on a 480x320 screen at 1x.
template <class Draw> std::vector<std::string> texts_of(Draw draw) {
    std::vector<std::uint8_t> px(std::size_t{480} * 320 * 4, 0);
    const Drawn d;
    draw(px.data(), 480u, 320u, 1);
    return d.texts;
}

// The texts of the port whose translation in the menu language now differs from the English:
// none of them may be drawn in English.
std::set<std::string> english_with_other_text() {
    std::set<std::string> out;
    for (std::size_t i = 0; i < static_cast<std::size_t>(Txt::kCount); i++) {
        const auto id = static_cast<Txt>(i);
        if (std::string(fe::tr(id)) != fe::port_text_english(id) &&
            std::string(fe::port_text_english(id)).find('{') == std::string::npos) {
            out.insert(fe::port_text_english(id));
        }
    }
    return out;
}

} // namespace

TEST_CASE("menu languages: every port text has a translation in each shipped language, with "
          "the English's places (#373)") {
    const auto &rows = fe::port_text_rows();
    REQUIRE(rows.size() > 600);
    const auto &languages = fe::shipped_port_languages();
    // The setting's names are auto, English and the shipped folders, in order.
    std::vector<std::string> names(std::begin(fe::kMenuLanguageNames) + 1,
                                   std::end(fe::kMenuLanguageNames));
    CHECK(names == fe::menu_languages());
    CHECK(std::string(fe::kMenuLanguageNames[0]) == "auto");
    REQUIRE(languages.size() >= 2);
    for (const fe::ShippedLanguage &l : languages) {
        CAPTURE(l.tag);
        CHECK(l.error.empty());
        CHECK_FALSE(l.name.empty());
        std::map<std::string, std::string> texts(l.texts.begin(), l.texts.end());
        // Missing texts, listed in .strings syntax with their English to translate.
        std::string missing;
        for (const auto &[key, english] : rows) {
            const auto it = texts.find(key);
            if (it == texts.end()) {
                missing += quoted(key) + " = " + quoted(english) + ";\n";
            } else {
                CAPTURE(key);
                CHECK_FALSE(it->second.empty());
            }
        }
        CHECK_MESSAGE(missing.empty(), "platform/lang/" << l.tag << "/Port.strings lacks:\n"
                                                        << missing);
        std::string wrong;
        for (const std::string &k : l.wrong_places) {
            wrong += k + "\n";
        }
        CHECK_MESSAGE(l.wrong_places.empty(), "places ({0}, ...) not the English's:\n" << wrong);
        std::string unknown;
        for (const std::string &k : l.unknown) {
            unknown += k + "\n";
        }
        CHECK_MESSAGE(l.unknown.empty(), "keys the port has no text for:\n" << unknown);
        CHECK(l.texts.size() == rows.size());
    }
    // A language is a draft until a second speaker reviews it (#375,
    // docs/build/how-to/translation.md): the Dutch is reviewed, the Korean and a new language
    // are drafts. Taking a language off this list, or onto it, is a deliberate change.
    const std::set<std::string> reviewed = {"nl"};
    for (const fe::ShippedLanguage &l : languages) {
        CAPTURE(l.tag);
        CHECK((l.description.rfind("Draft", 0) == 0) == (reviewed.count(l.tag) == 0));
    }
    CHECK(std::any_of(languages.begin(), languages.end(),
                      [](const fe::ShippedLanguage &l) { return l.tag == "ko"; }));
}

TEST_CASE("menu languages: auto follows the system's preferred languages; a set one stays") {
    {
        const SystemLanguages sys("nl_NL");
        CHECK(fe::system_menu_language() == "nl");
        CHECK(fe::apply_menu_language(fe::MenuLanguage::kAuto) == "nl");
        CHECK(fe::menu_language() == "nl");
        CHECK(fe::apply_menu_language(fe::MenuLanguage::kEn) == "en");
        CHECK(std::string(fe::tr(Txt::menu_title)) == "SETTINGS");
        CHECK(fe::apply_menu_language(fe::MenuLanguage::kKo) == "ko");
    }
    {
        const SystemLanguages sys("fr_FR,ko_KR,nl");
        CHECK(fe::system_menu_language() == "ko"); // the first the port has
    }
    {
        const SystemLanguages sys("en_US,nl_NL");
        CHECK(fe::system_menu_language() == "en");
    }
    {
        const SystemLanguages sys("nl_BE");
        CHECK(fe::system_menu_language() == "nl"); // by the language subtag
    }
    {
        const SystemLanguages sys("fr_CA,de");
        CHECK(fe::system_menu_language() == "en"); // none shipped: English
    }
    CHECK_FALSE(fe::set_menu_language("fr"));
    CHECK(fe::menu_language() == "en");
}

TEST_CASE("menu languages: a text drawn before a change of language stays valid; a pack's text "
          "wins over the menu language's") {
    const SystemLanguages sys("en");
    REQUIRE(fe::set_menu_language("nl"));
    const char *nl = fe::tr(Txt::menu_title);
    const std::string nl_copy = nl;
    CHECK(nl_copy != "SETTINGS");
    CHECK(std::string(fe::tr_key("setting.menu_language.label", "Menu language")) !=
          "Menu language");
    REQUIRE(fe::set_menu_language("ko"));
    CHECK(std::string(nl) == nl_copy); // the Dutch text still alive
    CHECK(std::string(fe::tr(Txt::menu_title)) != nl_copy);
    REQUIRE(fe::set_menu_language("en"));
    CHECK(fe::tr(Txt::menu_title) == fe::port_text_english(Txt::menu_title));
    CHECK(fe::menu_language_name("en") == "English");
    CHECK(fe::menu_language_name("nl") == "Nederlands");
    CHECK(fe::menu_language_name("auto") == "auto (English)");
}

TEST_CASE("menu languages: with the system in Dutch, the first start is Dutch from the import "
          "screen on (#373)") {
    REQUIRE(SDL_Init(SDL_INIT_GAMEPAD));
    const SystemLanguages sys("nl_NL");
    // A first start: no settings.ini in the data folder.
    const fs::path dir = fs::path(MT_TEST_OUTPUT_DIR) / "port_languages_first_start";
    fs::remove_all(dir);
    fs::create_directories(dir);
    fe::Settings settings;
    bool exists = true;
    fe::load_settings(dir.string(), &settings, &exists);
    CHECK_FALSE(exists);
    CHECK(settings.menu_language == fe::MenuLanguage::kAuto);
    // main.cpp's step after the settings are read, before any screen.
    CHECK(fe::apply_menu_language(settings.menu_language) == "nl");
    const std::set<std::string> english = english_with_other_text();
    REQUIRE(english.size() > 200);
    // A keyboard key's name ("Up", "Tab") stays as SDL names the key.
    const auto none_english = [&english](const std::vector<std::string> &texts) {
        for (const std::string &t : texts) {
            CAPTURE(t);
            CHECK((english.count(t) == 0 || SDL_GetKeyFromName(t.c_str()) != SDLK_UNKNOWN));
        }
    };
    // The import screen.
    {
        fe::ImportScreen import(dir.string(), nullptr);
        const auto texts = texts_of([&](std::uint8_t *px, std::uint32_t w, std::uint32_t h, int s) {
            import.draw(px, w, h, s);
        });
        REQUIRE_FALSE(texts.empty());
        CHECK(std::find(texts.begin(), texts.end(), fe::tr(Txt::import_title)) != texts.end());
        CHECK(std::string(fe::tr(Txt::import_title)) != "IMPORT YOUR GAME");
        none_english(texts);
    }
    // The preset picker, its controls step after a preset.
    {
        fe::PresetPicker picker(false, true);
        const auto texts = texts_of([&](std::uint8_t *px, std::uint32_t w, std::uint32_t h, int s) {
            picker.draw(px, w, h, s);
        });
        CHECK(std::find(texts.begin(), texts.end(), fe::tr(Txt::picker_title)) != texts.end());
        CHECK(std::string(fe::tr(Txt::picker_title)) != "HOW DO YOU WANT TO PLAY?");
        none_english(texts);
    }
    // The controls card the picker offers.
    {
        fe::Input input;
        fe::Actions actions(&input);
        fe::ControlsCard card(&actions, nullptr);
        card.seed_device(fe::PromptDevice::kKeyboard);
        card.open();
        const auto texts = texts_of([&](std::uint8_t *px, std::uint32_t w, std::uint32_t h, int s) {
            card.draw(px, w, h, s);
        });
        REQUIRE_FALSE(texts.empty());
        none_english(texts);
    }
    fs::remove_all(dir);
    SDL_Quit();
}
