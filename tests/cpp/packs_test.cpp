// The packs screen (#370, ADR 0149, packs.h) and the pack manifest (maplethief/pack_manifest.h)
// on synthetic packs written at run time: the manifest's rules and reasons, the list's own rules,
// the start's choice and its notices, and the settings menu's packs screen driven with a
// controller alone: two packs turned on, reordered, one turned off; a pack with a bad file
// saying why. Several packs open at once (#421): the exit log's line per pack with what it
// replaced and what an earlier pack hid, the same on the packs screen, and the pack flags given
// more than once.
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "data_notice.h"
#include "maplethief/pack_manifest.h"
#include "maplethief/textpack.h"
#include "packs.h"
#include "settings.h"
#include "settings_menu.h"

namespace {

namespace fe = maplethief::frontend;
namespace fs = std::filesystem;
using fe::PackKind;
using fe::Settings;
using fe::SettingsMenu;

// A data folder of its own, removed afterwards.
struct Base {
    fs::path dir = fs::temp_directory_path() /
                   ("maplethief-packs-test-" + std::to_string(std::random_device{}()) + "-" +
                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Base() { fs::create_directories(dir); }
    ~Base() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    Base(const Base &) = delete;
    Base &operator=(const Base &) = delete;
    std::string str() const { return dir.generic_string(); }

    // A pack folder <base>/<sub>/<name> with `manifest` as pack.strings ("" : none).
    std::string pack(const std::string &sub, const std::string &name,
                     const std::string &manifest) const {
        const fs::path p = dir / sub / name;
        fs::create_directories(p);
        if (!manifest.empty()) {
            std::ofstream(p / "pack.strings", std::ios::binary) << manifest;
        }
        return p.generic_string();
    }
};

std::string manifest(const std::string &kind, const std::string &id, const std::string &name,
                     const std::string &more = "") {
    return "\"format\" = \"1\";\n\"kind\" = \"" + kind + "\";\n\"id\" = \"" + id +
           "\";\n\"name\" = \"" + name + "\";\n" + more;
}

fe::PackInfo read(const std::string &path, PackKind kind, const std::string &port = "1.4.0") {
    return fe::read_pack(path, kind, port);
}

SDL_Event pad_event(SDL_GamepadButton b, bool down) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
    e.gbutton.button = static_cast<Uint8>(b);
    e.gbutton.down = down;
    return e;
}
void press(SettingsMenu &menu, SDL_GamepadButton b) {
    menu.handle(pad_event(b, true));
    menu.handle(pad_event(b, false));
}

// The menu's list row whose label starts with `label`, or -1.
long row_of(const SettingsMenu &menu, const std::string &label) {
    for (std::size_t i = 1; i < menu.items().size(); i++) {
        if (menu.row_label(i).rfind(label, 0) == 0) {
            return static_cast<long>(i);
        }
    }
    return -1;
}

// The pad's d-pad down until item `want` is selected; false if it never is.
bool pad_to(SettingsMenu &menu, std::size_t want) {
    for (std::size_t n = 0; n <= menu.items().size(); n++) {
        if (menu.selected() == want) {
            return true;
        }
        press(menu, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    }
    return false;
}

// The settings menu as game.cpp's start_menu sets it up for the packs: each pack row opens the
// packs screen at its kind.
struct PacksMenu {
    Settings live;
    SettingsMenu menu{fe::settings_registry(), nullptr, nullptr, nullptr};
    std::shared_ptr<fe::PacksScreen> screen;

    explicit PacksMenu(const std::string &base)
        : screen(std::make_shared<fe::PacksScreen>(base, "1.4.0")) {
        menu.set_platform(fe::SettingsPlatform::kDesktop);
        menu.attach(&live);
        for (const PackKind kind : fe::kPackKinds) {
            menu.set_setting_opener(fe::pack_kind_key(kind), [this, kind] {
                screen->scan(live);
                menu.open_list(fe::packs_menu_list(
                    screen, kind, [this](const std::string &key, const std::string &value) {
                        return menu.change_setting(key, value) ? &live : nullptr;
                    }));
                return std::string();
            });
        }
    }

    // With the pad alone: opens the menu, finds the texture pack row in Video, opens the packs
    // screen on it.
    void open_texture_packs() {
        press(menu, SDL_GAMEPAD_BUTTON_GUIDE);
        REQUIRE(menu.is_open());
        std::size_t row = 0;
        for (std::size_t i = 1; i < menu.items().size(); i++) {
            const SettingsMenu::Item &it = menu.items()[i];
            if (it.kind == SettingsMenu::ItemKind::kSetting &&
                std::strcmp(it.setting->key, "texture_pack") == 0) {
                row = i;
            }
        }
        REQUIRE(row != 0);
        REQUIRE(pad_to(menu, row));
        press(menu, SDL_GAMEPAD_BUTTON_SOUTH);
        REQUIRE(menu.in_list());
    }
};

} // namespace

TEST_CASE("pack manifest: pack.strings fields, the fallbacks, and every refusal's reason") {
    Base b;
    SUBCASE("a full manifest") {
        const std::string p = b.pack(
            "texture-packs", "HD",
            manifest("texture", "hd.sprites", "HD Sprites",
                     "\"version\" = \"1.2\";\n\"author\" = \"Ana\";\n\"description\" = \"Sharp "
                     "sprites.\";\n\"game_version\" = \"1.5\";\n\"port_min\" = \"1.3.0\";\n"
                     "\"content\" = \"derived\";\n\"license\" = \"CC-BY-4.0\";\n\"x\" = \"y\";"));
        mt_pack_manifest m{};
        CHECK(mt_pack_manifest_read(p.c_str(), "texture", "1.4.0", &m) == 1);
        CHECK(std::string(m.refused).empty());
        CHECK(std::string(m.id) == "hd.sprites");
        CHECK(std::string(m.name) == "HD Sprites");
        CHECK(std::string(m.version) == "1.2");
        CHECK(std::string(m.author) == "Ana");
        CHECK(std::string(m.description) == "Sharp sprites.");
        CHECK(std::string(m.content) == "derived");
        CHECK(std::string(m.license) == "CC-BY-4.0");
        CHECK(std::string(m.source) == "pack.strings");
        CHECK(fe::pack_title(read(p, PackKind::kTexture)) == "HD Sprites 1.2");
    }
    SUBCASE("a bare texture or audio folder: its folder's name") {
        const fe::PackInfo p = read(b.pack("audio-packs", "Remix", ""), PackKind::kAudio);
        CHECK(p.refused.empty());
        CHECK(p.name == "Remix");
        CHECK(p.id == "Remix");
    }
    SUBCASE("a text pack's textpack.strings still counts; pack.strings wins over it") {
        const std::string p = b.pack("text-packs", "de", "");
        std::ofstream(fs::path(p) / "textpack.strings")
            << "\"format\" = \"1\"; \"name\" = \"Deutsch\"; \"language\" = \"de\";";
        fe::PackInfo info = read(p, PackKind::kText);
        CHECK(info.refused.empty());
        CHECK(info.name == "Deutsch");
        CHECK(info.language == "de");
        CHECK(info.id == "de");
        std::ofstream(fs::path(p) / "pack.strings")
            << manifest("text", "de.menus", "Deutsch (Menüs)", "\"language\" = \"de\";");
        info = read(p, PackKind::kText);
        CHECK(info.refused.empty());
        CHECK(info.id == "de.menus");
        CHECK(info.name == "Deutsch (Menüs)");
        // A text folder with no manifest at all.
        CHECK(read(b.pack("text-packs", "bare", ""), PackKind::kText).refused ==
              "it has no pack.strings or textpack.strings");
    }
    SUBCASE("refusals") {
        const auto why = [&](const std::string &m, PackKind kind = PackKind::kTexture) {
            static int n = 0;
            return read(b.pack("texture-packs", "p" + std::to_string(n++), m), kind).refused;
        };
        CHECK(why("\"format\" = \"2\"; \"kind\" = \"texture\"; \"id\" = \"a\"; \"name\" = "
                  "\"A\";") == "pack.strings: pack format 2 needs a newer port");
        CHECK(why("\"kind\" = \"texture\"; \"id\" = \"a\"; \"name\" = \"A\";") ==
              "pack.strings: it has no \"format\"");
        CHECK(why(manifest("audio", "a", "A")) ==
              "pack.strings: it is an audio pack, not a texture pack");
        CHECK(why(manifest("music", "a", "A")) ==
              "pack.strings: its \"kind\" is not texture, audio or text");
        CHECK(why(manifest("texture", "Bad Id", "A")) ==
              "pack.strings: its \"id\" is not 1 to 64 of a-z, 0-9, '.', '_' and '-'");
        CHECK(why(manifest("texture", "-a", "A")).find("\"id\"") != std::string::npos);
        CHECK(why(manifest("texture", "a", std::string(65, 'x'))) ==
              "pack.strings: its \"name\" is longer than 64 characters");
        CHECK(why(manifest("texture", "a", "A", "\"game_version\" = \"1.6\";")) ==
              "pack.strings: made for the game's version 1.6; this port runs 1.5");
        CHECK(why(manifest("texture", "a", "A", "\"port_min\" = \"1.5.0\";")) ==
              "pack.strings: needs port 1.5.0 or later; this is 1.4.0");
        CHECK(why(manifest("texture", "a", "A", "\"port_min\" = \"1.4\";")) ==
              "pack.strings: its \"port_min\" is not a version (X.Y.Z)");
        CHECK(why(manifest("text", "a", "A"), PackKind::kText) ==
              "pack.strings: its \"language\" is not a language tag (such as \"de\" or "
              "\"pt-BR\")");
        // A bad file: the parser's reason, with its line.
        const std::string bad = why("\"format\" = \"1\";\n\"name\" = \"\xC3\x28\";");
        CHECK(bad.rfind("pack.strings: line 2: ", 0) == 0);
        CHECK(bad.find("UTF-8") != std::string::npos);
        CHECK(why("\"format\" = \"1\" \"kind\"").rfind("pack.strings: line 1: ", 0) == 0);
        CHECK(read((b.dir / "missing").generic_string(), PackKind::kTexture).refused ==
              "the folder is missing");
    }
    SUBCASE("versions") {
        int ok = 0;
        CHECK(mt_pack_compare_versions("1.4.0", "1.4.0", &ok) == 0);
        CHECK(ok == 1);
        CHECK(mt_pack_compare_versions("1.4.0", "1.10.0", &ok) < 0);
        CHECK(mt_pack_compare_versions("1.4.0-rc.1", "1.4.0", &ok) < 0);
        CHECK(mt_pack_compare_versions("1.4.0-rc.2", "1.4.0-rc.10", &ok) < 0);
        CHECK(mt_pack_compare_versions("2.0.0", "1.99.99", &ok) > 0);
        CHECK(mt_pack_compare_versions("1.4", "1.4.0", &ok) == 0);
        CHECK(ok == 0);
        CHECK(mt_pack_compare_versions("01.4.0", "1.4.0", &ok) == 0);
        CHECK(ok == 0);
        // port_min against a release candidate of that release: too old.
        const std::string p = b.pack("texture-packs", "rc",
                                     manifest("texture", "a", "A", "\"port_min\" = \"1.4.0\";"));
        CHECK(read(p, PackKind::kTexture, "1.4.0-rc.1").refused ==
              "pack.strings: needs port 1.4.0 or later; this is 1.4.0-rc.1");
        CHECK(read(p, PackKind::kTexture, "").refused.empty());
    }
}

TEST_CASE("packs: the list's own rules; the start opens every one it takes and says why not") {
    Base b;
    CHECK(fe::split_pack_list("") == std::vector<std::string>{});
    CHECK(fe::split_pack_list("/a") == std::vector<std::string>{"/a"});
    CHECK(fe::split_pack_list("/a||/b|") == std::vector<std::string>{"/a", "/b"});
    CHECK(fe::join_pack_list({"/a", "/b"}) == "/a|/b");
    CHECK(fe::pack_list_summary("", "none") == "none");
    CHECK(fe::pack_list_summary("/p/HD/", "none") == "HD");
    CHECK(fe::pack_list_summary("/p/HD|/p/UI|/p/X", "none") == "HD + 2");

    const std::string a =
        b.pack("text-packs", "a", manifest("text", "a", "A", "\"language\" = \"de\";"));
    const std::string a2 =
        b.pack("text-packs", "a2", manifest("text", "a", "A again", "\"language\" = \"de\";"));
    const std::string fr =
        b.pack("text-packs", "fr", manifest("text", "fr", "F", "\"language\" = \"fr\";"));
    const std::string bad = b.pack("text-packs", "bad", "\"format\" = \"9\";");
    const std::vector<fe::PackInfo> list =
        fe::check_pack_list({bad, a, a2, fr}, PackKind::kText, "1.4.0");
    REQUIRE(list.size() == 4);
    CHECK(list[0].refused == "pack.strings: pack format 9 needs a newer port");
    CHECK(list[1].refused.empty());
    CHECK(list[2].refused == "an earlier pack in the list has its id, a");
    CHECK(list[3].refused == "language fr differs from the first text pack's de");

    std::vector<std::string> many;
    for (int i = 0; i < 17; i++) {
        many.push_back(b.pack("texture-packs", "t" + std::to_string(i),
                              manifest("texture", "t" + std::to_string(i), "T")));
    }
    const std::vector<fe::PackInfo> seventeen =
        fe::check_pack_list(many, PackKind::kTexture, "1.4.0");
    CHECK(seventeen[15].refused.empty());
    CHECK(seventeen[16].refused == "at most 16 packs of a kind are on at once");
    CHECK(read(b.pack("texture-packs", "a|b", ""), PackKind::kTexture).refused ==
          "its folder's path holds '|', which a list of packs cannot");

    // The start: the refused pack is a notice with its reason; every accepted one opens (#421).
    Settings s;
    s.text_pack = fe::join_pack_list({bad, a, fr});
    fe::take_data_notices();
    CHECK(fe::packs_to_open(s, PackKind::kText, "1.4.0") == std::vector<std::string>{a});
    const std::vector<std::string> notices = fe::take_data_notices();
    REQUIRE(notices.size() == 2);
    CHECK(notices[0] ==
          "The pack bad was not loaded: pack.strings: pack format 9 needs a newer port");
    CHECK(notices[1] ==
          "The pack F was not loaded: language fr differs from the first text pack's de");
    s.text_pack.clear();
    CHECK(fe::packs_to_open(s, PackKind::kText, "1.4.0").empty());
    CHECK(fe::take_data_notices().empty());
}

TEST_CASE("packs screen: with a controller alone, two packs on, reordered, one off (#370)") {
    Base b;
    const std::string alpha = b.pack(
        "texture-packs", "A", manifest("texture", "alpha", "Alpha", "\"version\" = \"1.0\";"));
    const std::string beta =
        b.pack("texture-packs", "B", manifest("texture", "beta", "Beta", "\"author\" = \"Bo\";"));
    b.pack("texture-packs", "C", manifest("texture", "gamma", "Gamma"));
    PacksMenu m(b.str());
    m.open_texture_packs();
    SettingsMenu &menu = m.menu;
    // Every kind's rows, the texture packs selected first; the others say where packs go.
    CHECK(menu.row_label(menu.selected()) == "Texture: Alpha 1.0");
    CHECK(menu.row_value(menu.selected()) == "off");
    CHECK(row_of(menu, "Audio: none found") > 0);
    CHECK(row_of(menu, "Text: none found") > 0);
    CHECK(menu.help() == alpha);

    // Alpha on, then Beta on: Alpha #1, Beta #2.
    press(menu, SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK(m.live.texture_pack == alpha);
    CHECK(menu.row_value(menu.selected()) == "on #1");
    CHECK(menu.status() == "Alpha 1.0 is on (#1), from the next start");
    const long beta_row = row_of(menu, "Texture: Beta");
    REQUIRE(beta_row > 0);
    REQUIRE(pad_to(menu, static_cast<std::size_t>(beta_row)));
    CHECK(menu.help() == "By Bo. " + beta);
    press(menu, SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK(m.live.texture_pack == alpha + "|" + beta);
    CHECK(menu.row_label(menu.selected()) == "Texture: Beta");
    CHECK(menu.row_value(menu.selected()) == "on #2");

    // Beta earlier (Left): Beta #1, Alpha #2; the selection stays on Beta.
    press(menu, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    CHECK(m.live.texture_pack == beta + "|" + alpha);
    CHECK(menu.row_label(menu.selected()) == "Texture: Beta");
    CHECK(menu.row_value(menu.selected()) == "on #1");
    CHECK(menu.status() == "Beta is now #1, from the next start");
    press(menu, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    CHECK(menu.status() == "Beta is first already");
    CHECK(m.live.texture_pack == beta + "|" + alpha);

    // Alpha off: Beta alone.
    const long alpha_row = row_of(menu, "Texture: Alpha");
    REQUIRE(alpha_row > 0);
    REQUIRE(pad_to(menu, static_cast<std::size_t>(alpha_row)));
    CHECK(menu.row_value(menu.selected()) == "on #2");
    press(menu, SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK(m.live.texture_pack == beta);
    CHECK(menu.row_label(menu.selected()) == "Texture: Alpha 1.0");
    CHECK(menu.row_value(menu.selected()) == "off");
    // An off pack has no place to move to.
    press(menu, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    CHECK(menu.status() == "Turn Alpha 1.0 on first to order it");

    // Back to the tab (East): the row shows the list's first pack; the changes are the menu's.
    press(menu, SDL_GAMEPAD_BUTTON_EAST);
    CHECK_FALSE(menu.in_list());
    CHECK(menu.row_value(menu.selected()) == "B");
    const auto changes = menu.take_changes();
    REQUIRE(changes.size() == 4);
    for (const auto &c : changes) {
        CHECK(c.first == "texture_pack");
    }
    CHECK(changes[1].second == alpha + "|" + beta);
    CHECK(changes[2].second == beta + "|" + alpha);
    CHECK(changes[3].second == beta);
    press(menu, SDL_GAMEPAD_BUTTON_EAST);
    CHECK_FALSE(menu.is_open());
}

TEST_CASE("packs screen: a pack with a bad file shows its reason on the screen (#370)") {
    Base b;
    b.pack("texture-packs", "Broken", "\"format\" = \"1\";\n\"name\" = \"\xC3\x28\";\n");
    b.pack("texture-packs", "Later",
           manifest("texture", "later", "Later", "\"port_min\" = \"9.0.0\";"));
    PacksMenu m(b.str());
    m.open_texture_packs();
    SettingsMenu &menu = m.menu;
    const long broken = row_of(menu, "Texture: Broken");
    REQUIRE(broken > 0);
    REQUIRE(pad_to(menu, static_cast<std::size_t>(broken)));
    CHECK(menu.row_value(menu.selected()) == "refused");
    CHECK(menu.help().rfind("Refused: pack.strings: line 2: ", 0) == 0);
    const std::string dump = menu.dump();
    CHECK(dump.find("> Texture: Broken | refused") != std::string::npos);
    CHECK(dump.find("help: Refused: pack.strings: line 2: ") != std::string::npos);
    // It is not turned on, and the status line says why.
    press(menu, SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK(m.live.texture_pack.empty());
    CHECK(menu.status().rfind("Broken cannot be turned on: pack.strings: line 2: ", 0) == 0);
    const long later = row_of(menu, "Texture: Later");
    REQUIRE(later > 0);
    REQUIRE(pad_to(menu, static_cast<std::size_t>(later)));
    CHECK(menu.help() == "Refused: pack.strings: needs port 9.0.0 or later; this is 1.4.0");
    // A refused pack the setting lists (settings.ini) shows as on and refused, and can be
    // turned off.
    press(menu, SDL_GAMEPAD_BUTTON_EAST);
    m.live.texture_pack = (b.dir / "texture-packs" / "Broken").generic_string();
    press(menu, SDL_GAMEPAD_BUTTON_SOUTH);
    REQUIRE(menu.in_list());
    CHECK(menu.row_label(menu.selected()) == "Texture: Broken");
    CHECK(menu.row_value(menu.selected()) == "on #1, refused");
    press(menu, SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK(m.live.texture_pack.empty());
    CHECK(menu.row_value(menu.selected()) == "refused");
}

TEST_CASE("packs: the Original preset has no pack (#370)") {
    Settings s;
    std::string why;
    REQUIRE(fe::apply_setting(&s, "preset", "original", &why));
    for (const PackKind kind : fe::kPackKinds) {
        CAPTURE(fe::pack_kind_key(kind));
        CHECK(fe::pack_kind_value(s, kind).empty());
        CHECK(fe::packs_to_open(s, kind, "1.4.0").empty());
        CHECK(fe::pack_kind_of(fe::pack_kind_key(kind)) == kind);
    }
    CHECK_FALSE(fe::pack_kind_of("border_image"));
}

TEST_CASE("packs: the exit log names each open pack's replaced and hidden counts (#421)") {
    Base b;
    const auto text_pack = [&b](const std::string &name, const std::string &more,
                                const std::string &port) {
        const std::string p = b.pack("text-packs", name,
                                     manifest("text", name == "Menus" ? "menus" : "dialogue", name,
                                              "\"language\" = \"de\";" + more));
        std::ofstream(fs::path(p) / "Port.strings", std::ios::binary) << port;
        return p;
    };
    const std::string first =
        text_pack("Menus", "\"version\" = \"1.2\";",
                  "\"hint_move\" = \"bewegen\";\n\"hint_back\" = \"zurueck\";\n");
    const std::string second =
        text_pack("Dialogue", "", "\"hint_move\" = \"gehen\";\n\"hint_page\" = \"Seite\";\n");
    Settings s;
    s.text_pack = fe::join_pack_list({first, second});
    const std::vector<std::string> paths = fe::packs_to_open(s, PackKind::kText, "1.4.0");
    REQUIRE(paths == std::vector<std::string>{first, second});
    REQUIRE(fe::open_packs(PackKind::kText, paths).empty());
    CHECK(std::strcmp(mt_textpack_lookup("Port", "hint_move"), "bewegen") == 0); // the first's
    CHECK(std::strcmp(mt_textpack_lookup("Port", "hint_back"), "zurueck") == 0);
    CHECK(std::strcmp(mt_textpack_lookup("Port", "hint_page"), "Seite") == 0); // the second's
    CHECK(std::strcmp(mt_textpack_lookup("Port", "hint_move"), "bewegen") == 0);

    const std::vector<fe::PackUse> uses = fe::open_pack_uses(PackKind::kText, "1.4.0");
    REQUIRE(uses.size() == 2);
    CHECK(fe::pack_use_line(PackKind::kText, uses[0]) ==
          "textpack: Menus 1.2: 3 replaced, 0 hidden");
    CHECK(fe::pack_use_line(PackKind::kText, uses[1]) ==
          "textpack: Dialogue: 1 replaced, 2 hidden by Menus 1.2");

    // The packs screen's help line says the same while the packs are open.
    fe::PacksScreen screen(b.str(), "1.4.0");
    screen.scan(s);
    const std::size_t row = screen.first_row(PackKind::kText);
    CHECK(screen.row_help(row + 1).find("This run: 1 replaced, 2 hidden by an earlier pack.") !=
          std::string::npos);
    screen.set_uses([](PackKind) { return std::vector<fe::PackUse>{}; });
    CHECK(screen.row_help(row + 1).find("This run") == std::string::npos);
    mt_textpack_close();
    CHECK(fe::open_pack_uses(PackKind::kText, "1.4.0").empty());

    // Several earlier packs hid entries; rejected files are named when there are some.
    fe::PackUse u;
    u.title = "HD Sprites 1.2";
    u.replaced = 812;
    u.hidden = 40;
    u.rejected = 2;
    u.hidden_by = {{"UI Redraw", 32}, {"Fonts", 8}};
    CHECK(fe::pack_use_line(PackKind::kTexture, u) ==
          "texpack: HD Sprites 1.2: 812 replaced, 40 hidden (32 by UI Redraw, 8 by Fonts), 2 "
          "rejected");
    u.hidden_by.pop_back();
    u.rejected = 0;
    CHECK(fe::pack_use_line(PackKind::kAudio, u) ==
          "audiopack: HD Sprites 1.2: 812 replaced, 40 hidden by UI Redraw");
}

TEST_CASE("packs: a pack flag given more than once lists its packs in order (#421)") {
    std::vector<std::pair<std::string, std::string>> flags;
    fe::add_setting_flag(flags, "texture_pack", "/p/HD");
    fe::add_setting_flag(flags, "music_volume", "50");
    fe::add_setting_flag(flags, "texture_pack", "/p/UI");
    fe::add_setting_flag(flags, "audio_pack", "/a/Music");
    fe::add_setting_flag(flags, "music_volume", "60"); // not a pack: the later one counts
    fe::add_setting_flag(flags, "texture_pack", "/p/X");
    REQUIRE(flags.size() == 4);
    CHECK(flags[0] == std::pair<std::string, std::string>{"texture_pack", "/p/HD|/p/UI|/p/X"});
    CHECK(flags[1] == std::pair<std::string, std::string>{"music_volume", "50"});
    CHECK(flags[2] == std::pair<std::string, std::string>{"audio_pack", "/a/Music"});
    CHECK(flags[3] == std::pair<std::string, std::string>{"music_volume", "60"});
    Settings s;
    for (const auto &[key, value] : flags) {
        REQUIRE(fe::apply_setting(&s, key, value, nullptr));
    }
    CHECK(fe::split_pack_list(s.texture_pack) ==
          std::vector<std::string>{"/p/HD", "/p/UI", "/p/X"});
    CHECK(s.music_volume == 60);
}
