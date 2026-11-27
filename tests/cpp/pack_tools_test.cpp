// The pack tools' frontend side (#371, ADR 0205, pack_tools.h) on synthetic folders: where the
// tools write, the packs Check packs looks at, the report's words, the port's own text as
// Port.strings (which its own check passes whole), and the menu's actions. The checks
// themselves are packcheck_test.cpp's.
#include <doctest/doctest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "maplethief/packcheck.h"
#include "maplethief/texpack.h"
#include "pack_tools.h"
#include "port_text.h"
#include "settings.h"

namespace {

namespace fs = std::filesystem;
namespace fe = maplethief::frontend;

struct Dir {
    fs::path path;
    explicit Dir(const char *name) {
        path = fs::temp_directory_path() / (std::string("maplethief-pack-tools-test-") + name);
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~Dir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    Dir(const Dir &) = delete;
    Dir &operator=(const Dir &) = delete;
    std::string str() const { return path.string(); }
};

void write(const fs::path &p, const std::string &bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << bytes;
}

std::string generic(const std::string &p) { return fs::path(p).generic_string(); }

} // namespace

TEST_CASE("pack tools: the folders beside game/ the tools write to (#371)") {
    const fe::PackToolPlaces p = fe::pack_tool_places("/games/mt", fe::Language::kKo);
    CHECK(generic(p.dump) == "/games/mt/texture-dumps/dump");
    CHECK(generic(p.built_pack) == "/games/mt/texture-packs/dump-2x");
    CHECK(generic(p.text_export) == "/games/mt/text-packs/text-ko");
    const fe::PackToolPlaces none = fe::pack_tool_places("", fe::Language::kEn);
    CHECK(none.dump.empty());
    CHECK(none.built_pack.empty());
    CHECK(none.text_export.empty());
}

TEST_CASE("pack tools: Check packs looks at the rows' folders, then the pack folders, each once "
          "(#371)") {
    Dir d("to-check");
    const fs::path parent = d.path;
    fs::create_directories(parent / "texture-packs" / "HD");
    fs::create_directories(parent / "text-packs" / "fr");
    fs::create_directories(parent / "audio-packs" / ".hidden");
    write(parent / "borders" / "leaf.png", "x"); // not a pack
    fe::Settings s;
    const std::string hd = (parent / "texture-packs" / "HD").string();
    s.texture_pack = "/elsewhere/A|" + hd + "|/elsewhere/A"; // ADR 0149's list
    s.text_pack = "/elsewhere/T";
    const std::vector<fe::PackToCheck> got = fe::packs_to_check(s, parent.string());
    REQUIRE(got.size() == 4);
    CHECK(got[0].kind == "texture");
    CHECK(got[0].dir == "/elsewhere/A");
    CHECK(got[1].dir == hd);
    CHECK(got[2].kind == "text");
    CHECK(got[2].dir == "/elsewhere/T");
    CHECK(got[3].kind == "text");
    CHECK(generic(got[3].dir) == generic((parent / "text-packs" / "fr").string()));
    CHECK(fe::packs_to_check(fe::Settings{}, "").empty());
}

TEST_CASE("pack tools: the report's words and the verdict (#371)") {
    Dir d("report");
    write(d.path / "pack.strings", "\"format\" = \"2\";\n");
    write(d.path / "0000000000000001_2x2.png", "nope");
    const fe::PackToolResult refused = fe::check_pack(d.str(), "", "texture");
    CHECK(fe::pack_check_verdict(refused) == "refused");
    const std::string text = fe::pack_tool_text(d.str(), refused);
    CHECK(text.find("refused: pack.strings: pack format 2 needs a newer port") !=
          std::string::npos);
    CHECK(text.find("rejected: 0000000000000001_2x2.png is no PNG") != std::string::npos);
    CHECK(text.find(d.str() + ": texture pack: refused\n") != std::string::npos);
    fs::remove(d.path / "pack.strings");
    const fe::PackToolResult problems = fe::check_pack(d.str(), "");
    CHECK(fe::pack_check_verdict(problems) == "opens, 0 usable, 1 rejected");
    fs::remove(d.path / "0000000000000001_2x2.png");
    CHECK(fe::pack_check_verdict(fe::check_pack(d.str(), "", "texture")) == "opens, 0 usable");
}

TEST_CASE("pack tools: Port.strings holds every text of the port, and its check passes it whole "
          "(#371)") {
    const std::string port = fe::port_strings_text();
    for (std::size_t i = 0; i < static_cast<std::size_t>(fe::Txt::kCount); ++i) {
        const std::string key =
            std::string("\"") + fe::port_text_key(static_cast<fe::Txt>(i)) + "\" = ";
        CHECK_MESSAGE(port.find(key) != std::string::npos, key);
    }
    CHECK(port.find("\"setting.vsync.label\" = ") != std::string::npos);
    CHECK(port.find("\"action.check_packs.button\" = \"Check...\";") != std::string::npos);
    CHECK(port.find("\"action.quit.confirm\" = \"Quit MapleThief?\";") != std::string::npos);
    Dir d("port");
    write(d.path / "pack.strings", "\"format\" = \"1\";\n\"kind\" = \"text\";\n\"id\" = "
                                   "\"port-en\";\n\"name\" = \"Port\";\n\"language\" = \"en\";\n");
    write(d.path / "Port.strings", port);
    const fe::PackToolResult r = fe::check_pack(d.str(), "");
    CHECK(r.report.opens == 1);
    CHECK(r.report.ignored == 0);
    CHECK(r.report.rejected == 0);
    CHECK(r.report.good > static_cast<std::uint32_t>(fe::Txt::kCount));
}

TEST_CASE("pack tools: the menu's dump starts and stops, the others say what is missing (#371)") {
    Dir d("menu");
    CHECK(fe::dump_textures_action("") == "No folder for packs on this device");
    const std::string dump = (d.path / "texture-dumps" / "dump").string();
    CHECK_FALSE(fe::dumping_textures());
    CHECK(fe::dump_textures_action(dump).rfind("Dumping textures to ", 0) == 0);
    CHECK(fe::dumping_textures());
    CHECK(fs::is_regular_file(fs::path(dump) / "manifest.jsonl"));
    CHECK(fe::dump_textures_action(dump) == "Dump stopped: 0 pictures new in " + dump);
    CHECK_FALSE(fe::dumping_textures());

    const std::string pack = (d.path / "texture-packs" / "dump-2x").string();
    CHECK(fe::build_texture_pack_action((d.path / "none").string(), pack) ==
          "No dump yet: start Dump textures and play first");
    // The dump's (empty) manifest: a pack of nothing, which says so.
    CHECK(fe::build_texture_pack_action(dump, pack) ==
          "0 pictures at 2x in " + pack + ": choose it as the texture pack");
    CHECK(fe::export_text_action((d.path / "no-game").string(), fe::Language::kEn,
                                 (d.path / "text").string())
              .rfind("Not exported: the game's folder ", 0) == 0);

    // Check packs: one row per pack with its verdict; choosing one shows its first problem.
    fe::Settings s;
    s.texture_pack = pack;
    write(fs::path(pack) / "0000000000000002_2x2.png", "nope");
    const fe::MenuList list = fe::check_packs_list(s, d.str(), "");
    REQUIRE(list.rows.size() == 1); // the row's folder is also texture-packs/dump-2x: once
    CHECK(list.rows[0].label == "Texture: dump-2x");
    CHECK(list.rows[0].value == "opens, 0 usable, 1 rejected");
    CHECK(list.choose(0).find("0000000000000002_2x2.png is no PNG") != std::string::npos);
    CHECK(fe::check_packs_list(fe::Settings{}, "", "").rows.empty());
}
