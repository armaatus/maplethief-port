// The port's text table (#189, ADR 0108, port_text.h): without a text pack every text is its
// English literal (the overlays' drawing tests hold it byte for byte); with a synthetic pack's
// Port.strings a row is the pack's, folded to the overlays' ASCII, a key the pack lacks keeps
// its English, a translation whose places ({0}, ...) are not the English's is not used, and a
// registry row's derived key (tr_key) answers too.
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include "maplethief/textpack.h"
#include "port_text.h"
#include "settings.h"

namespace fe = maplethief::frontend;
using fe::Txt;

namespace {

namespace fs = std::filesystem;

void write(const fs::path &p, const std::string &s) {
    std::ofstream f(p, std::ios::binary);
    f << s;
}

struct Pack {
    fs::path dir;
    // Each case its own folder: CTest runs the cases side by side.
    Pack(const char *name, const std::string &port) {
        dir = fs::path(MT_TEST_OUTPUT_DIR) / (std::string("port_text_") + name);
        fs::remove_all(dir);
        fs::create_directories(dir);
        write(dir / "textpack.strings",
              "\"format\" = \"1\";\n\"name\" = \"Port test\";\n\"language\" = \"de\";\n");
        write(dir / "Port.strings", port);
        REQUIRE(mt_textpack_open(dir.string().c_str()) >= 0);
        fe::reload_port_text();
    }
    ~Pack() {
        mt_textpack_close();
        fe::reload_port_text();
        fs::remove_all(dir);
    }
};

} // namespace

TEST_CASE("Port text: without a pack every text is the English literal; keys are unique ASCII") {
    REQUIRE(mt_textpack_is_open() == 0);
    std::set<std::string> keys;
    for (std::size_t i = 0; i < static_cast<std::size_t>(Txt::kCount); i++) {
        const Txt id = static_cast<Txt>(i);
        CHECK(fe::tr(id) == fe::port_text_english(id)); // the same pointer: the literal
        CHECK(keys.insert(fe::port_text_key(id)).second);
        for (const char *p = fe::port_text_english(id); *p != 0; p++) {
            CHECK(static_cast<unsigned char>(*p) < 0x80);
        }
    }
    CHECK(std::string(fe::tr(Txt::menu_title)) == "SETTINGS");
    CHECK(fe::tr(Txt::menu_set_in_ini, {"vsync", "on or off"}) ==
          "Set vsync in settings.ini: on or off");
    CHECK(fe::tr(Txt::keypad_status, {"D-pad", "100", "80", "0", "-4"}) ==
          "D-pad: size 100%, opacity 80%, moved 0,-4");
    const char *english = "Bindings";
    CHECK(fe::tr_key("setting.bindings.label", english) == english);
}

TEST_CASE("Port text: a pack's Port.strings, folded to ASCII; missing keys and bad places keep "
          "the English") {
    const Pack pack("folded", "/* synthetic */\n"
                              "\"menu_title\" = \"EINSTELLUNGEN\";\n"
                              "\"menu_preset\" = \"Voreinstellung für Größe\";\n"
                              "\"menu_set_in_ini\" = \"{1} – setze {0} in settings.ini\";\n"
                              "\"menu_not_in_build\" = \"nicht verfügbar\";\n" // drops {0}: refused
                              "\"group_video\" = \"Bild…\";\n"
                              "\"setting.vsync.label\" = \"Vertikale Sync\";\n"
                              "\"action.quit.button\" = \"Beenden…\";\n");
    CHECK(std::string(fe::tr(Txt::menu_title)) == "EINSTELLUNGEN");
    CHECK(std::string(fe::tr(Txt::menu_preset)) == "Voreinstellung fur Grosse");
    CHECK(fe::tr(Txt::menu_set_in_ini, {"vsync", "on or off"}) ==
          "on or off - setze vsync in settings.ini");
    CHECK(fe::tr(Txt::menu_not_in_build, {"Quit"}) == "Quit is not available in this build");
    CHECK(std::string(fe::setting_group_name(fe::SettingGroup::kVideo)) == "Bild...");
    CHECK(std::string(fe::tr(Txt::menu_yes)) == "Yes"); // not in the pack
    CHECK(std::string(fe::tr_key("setting.vsync.label", "VSync")) == "Vertikale Sync");
    CHECK(std::string(fe::tr_key("action.quit.button", "Quit...")) == "Beenden...");
    CHECK(std::string(fe::tr_key("setting.fps.label", "Frame rate")) == "Frame rate");
    // The raw value stays UTF-8 in the pack; only tr folds it.
    CHECK(std::string(mt_textpack_lookup("Port", "menu_preset")) ==
          "Voreinstellung f\xC3\xBCr Gr\xC3\xB6\xC3\x9F"
          "e");
}

TEST_CASE("Port text: closing the pack brings the English back") {
    {
        const Pack pack("closing", "\"menu_title\" = \"REGLAGES\";\n");
        CHECK(std::string(fe::tr(Txt::menu_title)) == "REGLAGES");
    }
    CHECK(fe::tr(Txt::menu_title) == fe::port_text_english(Txt::menu_title));
}

TEST_CASE("Port text: ascii_fold keeps ASCII and folds Latin letters and punctuation") {
    CHECK(fe::ascii_fold("Plain text 123 {0}") == "Plain text 123 {0}");
    CHECK(fe::ascii_fold("P\xC3\xB6tion H\xC3\xA9n\xC3\xA9sys") == "Potion Henesys");
    CHECK(fe::ascii_fold("\xC3\x9F \xC3\x86 \xC5\x93 \xC5\x81\xC3\xB3"
                         "d\xC5\xBA") == "ss AE oe Lodz");
    CHECK(fe::ascii_fold("\xE2\x80\x9Cq\xE2\x80\x9D \xE2\x80\x98s\xE2\x80\x99 a\xE2\x80\x94"
                         "b \xE2\x80\xA6") == "\"q\" 's' a-b ...");
    CHECK(fe::ascii_fold("\xD0\x9C\xD0\xB8\xD1\x80") == "???"); // Cyrillic: no ASCII form
    CHECK(fe::ascii_fold("\xE3\x83\xA1") == "?");               // one character, one '?'
    CHECK(fe::ascii_fold("a\xFF"
                         "b\xC3") == "a?b?");  // not UTF-8: '?' per byte
    CHECK(fe::ascii_fold("\xC0\xAF") == "??"); // an overlong form
}
