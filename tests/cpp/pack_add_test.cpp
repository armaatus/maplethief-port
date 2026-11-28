// Adding a pack on a desktop (#422, ADR 0149, pack_add.h) on synthetic packs and zips written at
// run time: a zip with a '..' entry, an absolute path, a link, an encrypted entry, a file over
// its kind's limit or more than the whole's limit is refused whole and writes nothing; a good
// zip lands in its kind's pack folder and shows on the packs screen; a folder dropped on the
// window (an SDL_EVENT_DROP_FILE) while the packs screen is open is added by its path; a refused
// one says why on the status line; the kind comes from pack.strings, textpack.strings or the
// files.
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

#include "maplethief/pack_manifest.h"
#include "maplethief/textpack.h"
#include "pack_add.h"
#include "packs.h"
#include "save_zip.h"
#include "settings.h"
#include "settings_menu.h"

namespace {

namespace fe = maplethief::frontend;
namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;
using fe::PackKind;
using fe::Settings;
using fe::SettingsMenu;

// A folder of its own, removed afterwards.
struct Dir {
    fs::path dir = fs::temp_directory_path() /
                   ("maplethief-pack-add-test-" + std::to_string(std::random_device{}()) + "-" +
                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Dir() { fs::create_directories(dir); }
    ~Dir() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    Dir(const Dir &) = delete;
    Dir &operator=(const Dir &) = delete;
    std::string str() const { return dir.generic_string(); }

    // Every path below, '/' separated, sorted: what a refusal must leave as it was.
    std::vector<std::string> tree() const {
        std::vector<std::string> out;
        for (const auto &e : fs::recursive_directory_iterator(dir)) {
            out.push_back(fs::relative(e.path(), dir).generic_string());
        }
        std::sort(out.begin(), out.end());
        return out;
    }
};

Bytes bytes_of(const std::string &s) { return Bytes(s.begin(), s.end()); }

std::string manifest(const std::string &kind, const std::string &id, const std::string &name,
                     const std::string &more = "") {
    return "\"format\" = \"1\";\n\"kind\" = \"" + kind + "\";\n\"id\" = \"" + id +
           "\";\n\"name\" = \"" + name + "\";\n" + more;
}

fe::ZipEntry entry(const std::string &name, const std::string &text) {
    return fe::ZipEntry{name, bytes_of(text)};
}

Bytes zip_of(const std::vector<fe::ZipEntry> &entries) { return fe::zip_store(entries); }

// The offsets of the central entries of a zip_store zip (no comment).
std::vector<std::size_t> central_entries(const Bytes &zip) {
    const std::size_t end = zip.size() - 22;
    const auto get16 = [&](std::size_t at) {
        return std::size_t{zip[at]} | std::size_t{zip[at + 1]} << 8;
    };
    const auto get32 = [&](std::size_t at) { return get16(at) | get16(at + 2) << 16; };
    std::vector<std::size_t> out;
    std::size_t at = get32(end + 16);
    for (std::size_t i = 0, n = get16(end + 10); i < n; i++) {
        out.push_back(at);
        at += 46 + get16(at + 28) + get16(at + 30) + get16(at + 32);
    }
    return out;
}

void put_le(Bytes &f, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; i++) {
        f[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
    }
}

// A texture pack's zip: one top folder `top` with its pack.strings and a picture.
std::vector<fe::ZipEntry> texture_pack(const std::string &top) {
    return {entry(top + "/pack.strings",
                  manifest("texture", "hd", "HD Sprites", "\"version\" = \"1.2\";")),
            entry(top + "/0011223344556677_64x64.png", "not decoded until the game asks")};
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

// A drop on the window, as SDL sends it: begin, the file, complete.
void drop(SettingsMenu &menu, const std::string &path) {
    SDL_Event e{};
    e.type = SDL_EVENT_DROP_BEGIN;
    CHECK(menu.handle(e));
    e = SDL_Event{};
    e.type = SDL_EVENT_DROP_FILE;
    e.drop.data = path.c_str();
    CHECK(menu.handle(e));
    e = SDL_Event{};
    e.type = SDL_EVENT_DROP_COMPLETE;
    CHECK(menu.handle(e));
}

// The settings menu with the packs screen as game.cpp sets it up, the "Add a pack..." row on
// (its picker answers with `picked`, as the dialog's answer would).
struct PacksMenu {
    Settings live;
    SettingsMenu menu{fe::settings_registry(), nullptr, nullptr, nullptr};
    std::shared_ptr<fe::PacksScreen> screen;
    std::string picked;
    unsigned picks = 0;

    explicit PacksMenu(const std::string &base)
        : screen(std::make_shared<fe::PacksScreen>(base, "1.4.0")) {
        menu.set_platform(fe::SettingsPlatform::kDesktop);
        menu.attach(&live);
        for (const PackKind kind : fe::kPackKinds) {
            menu.set_setting_opener(fe::pack_kind_key(kind), [this, kind] {
                screen->set_add_row(true);
                screen->scan(live);
                menu.open_list(fe::packs_menu_list(
                    screen, kind,
                    [this](const std::string &key, const std::string &value) {
                        return menu.change_setting(key, value) ? &live : nullptr;
                    },
                    [this]() -> const Settings & { return live; },
                    [this] {
                        picks++;
                        return std::string();
                    }));
                return std::string();
            });
        }
    }

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
        for (std::size_t n = 0; n <= menu.items().size() && menu.selected() != row; n++) {
            press(menu, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        }
        REQUIRE(menu.selected() == row);
        press(menu, SDL_GAMEPAD_BUTTON_SOUTH);
        REQUIRE(menu.in_list());
    }
};

} // namespace

TEST_CASE("pack add: the kind from pack.strings bytes (mt_pack_manifest_kind)") {
    char kind[8] = {'x'};
    const std::string audio = manifest("audio", "a", "A");
    CHECK(mt_pack_manifest_kind(reinterpret_cast<const std::uint8_t *>(audio.data()), audio.size(),
                                kind) == 1);
    CHECK(std::string(kind) == "audio");
    const std::string other = manifest("video", "a", "A");
    CHECK(mt_pack_manifest_kind(reinterpret_cast<const std::uint8_t *>(other.data()), other.size(),
                                kind) == 0);
    CHECK(std::string(kind).empty());
    const std::string broken = "\"kind\" = \"text\"";
    CHECK(mt_pack_manifest_kind(reinterpret_cast<const std::uint8_t *>(broken.data()),
                                broken.size(), kind) == 0);
    CHECK(mt_pack_manifest_kind(nullptr, 0, kind) == 0);
}

TEST_CASE("pack add: a folder's kind from pack.strings, textpack.strings or its files") {
    Dir d;
    std::string why;
    fs::create_directories(d.dir / "a");
    std::ofstream(d.dir / "a" / "pack.strings") << manifest("audio", "a", "A");
    CHECK(fe::pack_folder_kind((d.dir / "a").generic_string(), &why) == PackKind::kAudio);
    fs::create_directories(d.dir / "t");
    std::ofstream(d.dir / "t" / "textpack.strings") << "\"format\" = \"1\";";
    CHECK(fe::pack_folder_kind((d.dir / "t").generic_string(), &why) == PackKind::kText);
    fs::create_directories(d.dir / "png" / "ui");
    std::ofstream(d.dir / "png" / "ui" / "Title.png") << "x";
    CHECK(fe::pack_folder_kind((d.dir / "png").generic_string(), &why) == PackKind::kTexture);
    fs::create_directories(d.dir / "wav");
    std::ofstream(d.dir / "wav" / "Hit.WAV") << "x";
    CHECK(fe::pack_folder_kind((d.dir / "wav").generic_string(), &why) == PackKind::kAudio);
    fs::create_directories(d.dir / "none");
    std::ofstream(d.dir / "none" / "readme.txt") << "x";
    CHECK_FALSE(fe::pack_folder_kind((d.dir / "none").generic_string(), &why));
    CHECK(why == "its kind is not known: give it a pack.strings with its kind");
    CHECK_FALSE(fe::pack_folder_kind((d.dir / "missing").generic_string(), &why));
    CHECK(why == "it is not a folder");
}

TEST_CASE("pack add: a bad zip is refused whole and nothing is written (#422)") {
    Dir base;
    fs::create_directories(base.dir / "texture-packs");
    fs::create_directories(base.dir / "text-packs");
    const std::vector<std::string> before = base.tree();

    struct Bad {
        const char *what;
        Bytes zip;
        std::string why;
        std::uint64_t max_total = fe::kMaxPackZipBytes;
    };
    std::vector<Bad> bad;
    {
        auto e = texture_pack("HD");
        e.push_back(entry("HD/../../escape.png", "x"));
        bad.push_back({"a '..' entry", zip_of(e),
                       "its entry \"HD/../../escape.png\" is not safe (an empty, '.' or '..' "
                       "path component)"});
    }
    {
        auto e = texture_pack("HD");
        e.push_back(entry("/etc/escape.png", "x"));
        bad.push_back({"an absolute path", zip_of(e),
                       "its entry \"/etc/escape.png\" is not safe (an absolute path)"});
    }
    {
        // The link last, so the files before it would be written by a reader that went on.
        auto e = texture_pack("HD");
        e.push_back(entry("HD/link.png", "/etc/passwd"));
        Bytes z = zip_of(e);
        put_le(z, central_entries(z).back() + 38, 0120777u << 16);
        bad.push_back({"a link", z, "its entry \"HD/link.png\" is a link"});
    }
    {
        Bytes z = zip_of(texture_pack("HD"));
        z[central_entries(z).back() + 8] |= 1;
        bad.push_back(
            {"an encrypted entry", z, "its entry \"HD/0011223344556677_64x64.png\" is encrypted"});
    }
    {
        // A text pack's file over MT_TEXTPACK_MAX_FILE.
        const std::vector<fe::ZipEntry> e = {
            entry("De/pack.strings", manifest("text", "de", "Deutsch", "\"language\" = \"de\";")),
            fe::ZipEntry{"De/Big.strings", Bytes(MT_TEXTPACK_MAX_FILE + 1, ' ')}};
        bad.push_back({"a file over its kind's limit", zip_of(e),
                       "its file \"Big.strings\" is larger than 4 MiB"});
    }
    {
        // More than 1 GiB in all: 14 pictures that say they hold 79 MiB each (deflated, they
        // would fit a small zip), each under the texture limit.
        auto e = texture_pack("HD");
        for (char c = 'a'; c < 'a' + 14; c++) {
            e.push_back(entry("HD/" + std::string(16, c) + "_64x64.png", "x"));
        }
        Bytes z = zip_of(e);
        const std::vector<std::size_t> central = central_entries(z);
        for (std::size_t i = 2; i < central.size(); i++) {
            put_le(z, central[i] + 24, 79u << 20);
        }
        bad.push_back({"more than 1 GiB in all", z, "its files hold more than 1 GiB"});
    }
    {
        Bytes z = zip_of(texture_pack("HD"));
        bad.push_back({"a zip file over the limit", z, "the zip is larger than 64 bytes", 64});
    }
    {
        auto e = texture_pack("HD");
        e.push_back(entry("hd/PACK.strings", "x"));
        bad.push_back({"two names for one file", zip_of(e),
                       "its entries \"HD/pack.strings\" and \"hd/PACK.strings\" name one file"});
    }
    bad.push_back({"no zip", bytes_of("PK but not a zip at all, long enough to be looked at"),
                   "it is not a zip file (no end of central directory)"});
    for (const Bad &b : bad) {
        CAPTURE(b.what);
        const fe::PackAdd a =
            fe::unpack_pack_zip(b.zip, "pack.zip", base.str(), "1.4.0", b.max_total);
        CHECK_FALSE(a.ok);
        CHECK(a.why == b.why);
        CHECK(base.tree() == before);
    }
    // Through the file as the drop gives it: the same.
    const fs::path file = base.dir.parent_path() / (base.dir.filename().string() + "-link.zip");
    {
        Bytes z = zip_of(texture_pack("HD"));
        put_le(z, central_entries(z)[1] + 38, 0120777u << 16);
        std::ofstream(file, std::ios::binary)
            .write(reinterpret_cast<const char *>(z.data()),
                   static_cast<std::streamsize>(z.size()));
    }
    const fe::PackAdd a = fe::unpack_pack_zip_file(file.generic_string(), base.str(), "1.4.0");
    fs::remove(file);
    CHECK_FALSE(a.ok);
    CHECK(a.why == "its entry \"HD/0011223344556677_64x64.png\" is a link");
    CHECK(base.tree() == before);
}

TEST_CASE("pack add: a zip whose manifest is refused leaves nothing behind") {
    Dir base;
    const std::vector<fe::ZipEntry> e = {
        entry("Old/pack.strings", manifest("texture", "old", "Old", "\"port_min\" = \"9.0.0\";")),
        entry("Old/0011223344556677_64x64.png", "x")};
    const fe::PackAdd a = fe::unpack_pack_zip(zip_of(e), "Old.zip", base.str(), "1.4.0");
    CHECK_FALSE(a.ok);
    CHECK(a.why == "pack.strings: needs port 9.0.0 or later; this is 1.4.0");
    CHECK(base.tree() == std::vector<std::string>{"texture-packs"});
}

TEST_CASE("pack add: a good zip lands in its pack folder and shows on the packs screen (#422)") {
    Dir base;
    // One top folder: its name. Finder's extras left out.
    std::vector<fe::ZipEntry> e = texture_pack("HD");
    e.push_back(entry("HD/ui/Title.png", "x"));
    e.push_back(entry("__MACOSX/HD/._pack.strings", "x"));
    e.push_back(entry("HD/.DS_Store", "x"));
    fe::PackAdd a = fe::unpack_pack_zip(zip_of(e), "Download (1).zip", base.str(), "1.4.0");
    REQUIRE(a.ok);
    CHECK(a.kind == PackKind::kTexture);
    CHECK(a.path == base.str() + "/texture-packs/HD");
    CHECK(a.title == "HD Sprites 1.2");
    CHECK(base.tree() == std::vector<std::string>{"texture-packs", "texture-packs/HD",
                                                  "texture-packs/HD/0011223344556677_64x64.png",
                                                  "texture-packs/HD/pack.strings",
                                                  "texture-packs/HD/ui",
                                                  "texture-packs/HD/ui/Title.png"});
    // A second time: the folder is there.
    a = fe::unpack_pack_zip(zip_of(e), "HD.zip", base.str(), "1.4.0");
    CHECK_FALSE(a.ok);
    CHECK(a.why == "a pack folder named \"HD\" is there already");

    // Files at the top: the zip's name; a text pack by textpack.strings.
    const std::vector<fe::ZipEntry> text = {
        entry("textpack.strings",
              "\"format\" = \"1\";\n\"name\" = \"Nederlands\";\n\"language\" = \"nl\";"),
        entry("Port.strings", "\"hint_move\" = \"bewegen\";")};
    a = fe::unpack_pack_zip(zip_of(text), "nl-menus.zip", base.str(), "1.4.0");
    REQUIRE(a.ok);
    CHECK(a.kind == PackKind::kText);
    CHECK(a.path == base.str() + "/text-packs/nl-menus");
    CHECK(fs::is_regular_file(base.dir / "text-packs" / "nl-menus" / "Port.strings"));

    // Dropped on the packs screen as a file: unpacked, listed (off), selected.
    const fs::path file = base.dir.parent_path() / (base.dir.filename().string() + "-Retro.zip");
    {
        const Bytes z = zip_of(
            {entry("pack.strings", manifest("texture", "retro", "Retro")), entry("a.png", "x")});
        std::ofstream(file, std::ios::binary)
            .write(reinterpret_cast<const char *>(z.data()),
                   static_cast<std::streamsize>(z.size()));
    }
    PacksMenu m(base.str());
    m.open_texture_packs();
    drop(m.menu, file.generic_string());
    fs::remove(file);
    const std::string retro = base.str() + "/texture-packs/" + file.stem().string();
    CHECK(fs::is_regular_file(fs::path(retro) / "a.png"));
    CHECK(m.menu.row_label(m.menu.selected()) == "Texture: Retro");
    CHECK(m.menu.row_value(m.menu.selected()) == "off");
    CHECK(m.menu.status() == "Retro unpacked into texture-packs/: choose it to turn it on");
    CHECK(m.live.texture_pack.empty());
    // HD from the first zip is listed too.
    bool hd = false;
    for (std::size_t i = 1; i < m.menu.items().size(); i++) {
        hd = hd || m.menu.row_label(i) == "Texture: HD Sprites 1.2";
    }
    CHECK(hd);
}

TEST_CASE("pack add: a folder dropped on the packs screen is added by its path (#422)") {
    Dir base, elsewhere;
    const fs::path pack = elsewhere.dir / "Redraw";
    fs::create_directories(pack);
    std::ofstream(pack / "pack.strings")
        << manifest("texture", "redraw", "UI Redraw", "\"author\" = \"Kim\";");
    PacksMenu m(base.str());
    m.open_texture_packs();
    // The last row adds through the picker.
    const std::size_t last = m.menu.items().size() - 1;
    CHECK(m.menu.row_label(last) == "Add a pack...");
    CHECK(m.menu.row_value(last) == "choose a folder");
    for (std::size_t n = 0; n < last && m.menu.selected() != last; n++) {
        press(m.menu, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    }
    REQUIRE(m.menu.selected() == last);
    CHECK(m.menu.help() ==
          "Choose a pack's folder to add it by its path. Or drop a pack's folder or .zip on the "
          "window.");
    press(m.menu, SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK(m.picks == 1);
    CHECK(m.live.texture_pack.empty());

    // The drop: on, by its path, selected; nothing written into it or the data folder.
    drop(m.menu, pack.generic_string());
    CHECK(m.live.texture_pack == pack.generic_string());
    CHECK(m.menu.row_label(m.menu.selected()) == "Texture: UI Redraw");
    CHECK(m.menu.row_value(m.menu.selected()) == "on #1");
    CHECK(m.menu.status() == "UI Redraw added, on (#1), from the next start");
    CHECK(elsewhere.tree() == std::vector<std::string>{"Redraw", "Redraw/pack.strings"});
    // Again: listed already.
    drop(m.menu, pack.generic_string() + "/");
    CHECK(m.menu.status() == "UI Redraw is on the list already");
    CHECK(m.live.texture_pack == pack.generic_string());
    const auto changes = m.menu.take_changes();
    REQUIRE(changes.size() == 1);
    CHECK(changes[0].first == "texture_pack");
    CHECK(changes[0].second == pack.generic_string());

    // A refused folder says why on the status line and changes nothing.
    const fs::path old = elsewhere.dir / "Old";
    fs::create_directories(old);
    std::ofstream(old / "pack.strings")
        << manifest("audio", "old", "Old Songs", "\"port_min\" = \"9.0.0\";");
    drop(m.menu, old.generic_string());
    CHECK(m.menu.status() ==
          "Old Songs not added: pack.strings: needs port 9.0.0 or later; this is 1.4.0");
    CHECK(m.live.audio_pack.empty());
    const fs::path what = elsewhere.dir / "What";
    fs::create_directories(what);
    drop(m.menu, what.generic_string());
    CHECK(m.menu.status() ==
          "What not added: its kind is not known: give it a pack.strings with its kind");
    // A second pack with the same id: the list's own rule.
    const fs::path twin = elsewhere.dir / "Twin";
    fs::create_directories(twin);
    std::ofstream(twin / "pack.strings") << manifest("texture", "redraw", "Twin");
    drop(m.menu, twin.generic_string());
    CHECK(m.menu.status() == "Twin not added: an earlier pack in the list has its id, redraw");
    CHECK(m.live.texture_pack == pack.generic_string());
    CHECK_FALSE(m.menu.changed());

    // Closed list: a drop does nothing.
    press(m.menu, SDL_GAMEPAD_BUTTON_EAST);
    CHECK_FALSE(m.menu.in_list());
    CHECK_FALSE(m.menu.drop_on_list(twin.generic_string()));
}
