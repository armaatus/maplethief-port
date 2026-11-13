// The save tools (#148, ADR 0078, save_tools.h, save_zip.h): export to a folder and a zip and
// import back byte for byte; hostile sets refused with a reason and nothing written; the
// import's backup and its roll-back after a failed write; backups that rotate at N and skip an
// unchanged save; the hook on CDataMgr::SaveQuestData through MT_FN_HOOK's entry; restore;
// reading folders and zips (Finder's __MACOSX, Documents/, duplicates); the zip reader on
// hostile input; the base folder; the save_dir and save_backups settings. The same rules on
// savedata.py's fixtures, through the program: CTests saves.check_parity and
// saves.cli_round_trip (tools/mtport/savecheck.py).
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include "file_dialog.h"
#include "live_settings.h"
#include "maplethief/hooks.h"
#include "maplethief/recomp/calls.h"
#include "maplethief/saves.h"
#include "maplethief/stubs.h"
#include "save_fixtures.h"
#include "save_menu.h"
#include "save_tools.h"
#include "save_zip.h"
#include "settings.h"

namespace {

namespace fs = std::filesystem;
namespace fe = maplethief::frontend;
using Bytes = std::vector<std::uint8_t>;
using Tree = std::map<std::string, Bytes>; // a folder's files, by path below it

fs::path unique_dir(const char *what) {
    const fs::path dir =
        fs::temp_directory_path() /
        (std::string("maplethief-save-tools-") + what + "-" +
         std::to_string(std::random_device{}()) + "-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    return dir;
}

// A folder removed with everything in it at the end of the test.
struct TempDir {
    fs::path path;
    explicit TempDir(const char *what) : path(unique_dir(what)) {}
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;
};

Bytes read_bytes(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

void write_bytes(const fs::path &p, const Bytes &b) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char *>(b.data()), static_cast<std::streamsize>(b.size()));
}

// Every file below `dir`, with its bytes.
Tree tree(const fs::path &dir) {
    Tree t;
    if (!fs::exists(dir)) {
        return t;
    }
    for (const auto &e : fs::recursive_directory_iterator(dir)) {
        if (e.is_regular_file()) {
            t[fs::relative(e.path(), dir).generic_string()] = read_bytes(e.path());
        }
    }
    return t;
}

using save_fixtures::kConfig;
using save_fixtures::new_character;
using save_fixtures::new_quests;
using save_fixtures::valid_set;

void write_tree(const fs::path &dir, const Tree &t) {
    for (const auto &[name, bytes] : t) {
        write_bytes(dir / name, bytes);
    }
}

fe::SaveLocation location(const fs::path &root, std::uint32_t keep = 10) {
    fe::SaveLocation loc;
    loc.data_dir = root.string();
    loc.documents_dir = (root / "Documents").string();
    loc.backups_dir = (root / "backups").string();
    loc.keep = keep;
    fs::create_directories(loc.documents_dir);
    return loc;
}

std::vector<fe::ZipEntry> unzip(const Bytes &zip) {
    std::vector<fe::ZipEntry> entries;
    std::string why;
    REQUIRE_MESSAGE(fe::zip_read(zip, 1u << 20, &entries, &why), why);
    return entries;
}

// A zip of `entries`, each `size` zero bytes deflated (fixed Huffman codes: one literal, then
// copies of 258 at distance 1): a few kilobytes that inflate to any size.
Bytes zip_of_zeros(const std::vector<std::string> &names, std::uint32_t size) {
    Bytes deflated;
    std::uint32_t acc = 0, bits = 0;
    const auto put = [&](std::uint32_t v, std::uint32_t n) { // LSB first
        for (std::uint32_t i = 0; i < n; i++) {
            acc |= ((v >> i) & 1u) << bits;
            if (++bits == 8) {
                deflated.push_back(static_cast<std::uint8_t>(acc));
                acc = bits = 0;
            }
        }
    };
    const auto code = [&](std::uint32_t c, std::uint32_t n) { // a Huffman code, MSB first
        for (std::uint32_t i = n; i-- > 0;) {
            put((c >> i) & 1u, 1);
        }
    };
    put(1, 1);           // the last block
    put(1, 2);           // fixed codes
    code(0x30, 8);       // literal 0
    std::uint32_t left = size - 1;
    for (; left >= 258; left -= 258) {
        code(0xC5, 8);   // length 258 (code 285)
        code(0, 5);      // distance 1
    }
    for (; left > 0; left--) {
        code(0x30, 8);
    }
    code(0, 7);          // end of block
    if (bits != 0) {
        deflated.push_back(static_cast<std::uint8_t>(acc));
    }
    const std::uint32_t crc = fe::crc32(Bytes(size, 0));
    Bytes zip, central;
    const auto le = [](Bytes &b, std::uint32_t v, int n) {
        for (int i = 0; i < n; i++) {
            b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        }
    };
    for (const std::string &name : names) {
        const auto at = static_cast<std::uint32_t>(zip.size());
        le(zip, 0x04034b50u, 4);
        le(zip, 20, 2);
        le(zip, 0, 2);
        le(zip, 8, 2);
        le(zip, 0, 4); // time, date
        le(zip, crc, 4);
        le(zip, static_cast<std::uint32_t>(deflated.size()), 4);
        le(zip, size, 4);
        le(zip, static_cast<std::uint32_t>(name.size()), 2);
        le(zip, 0, 2);
        zip.insert(zip.end(), name.begin(), name.end());
        zip.insert(zip.end(), deflated.begin(), deflated.end());
        le(central, 0x02014b50u, 4);
        le(central, 20, 2);
        le(central, 20, 2);
        le(central, 0, 2);
        le(central, 8, 2);
        le(central, 0, 4);
        le(central, crc, 4);
        le(central, static_cast<std::uint32_t>(deflated.size()), 4);
        le(central, size, 4);
        le(central, static_cast<std::uint32_t>(name.size()), 2);
        le(central, 0, 2 + 2 + 2 + 2 + 4); // extra, comment, disk, internal, external
        le(central, at, 4);
        central.insert(central.end(), name.begin(), name.end());
    }
    const auto central_at = static_cast<std::uint32_t>(zip.size());
    zip.insert(zip.end(), central.begin(), central.end());
    le(zip, 0x06054b50u, 4);
    le(zip, 0, 4); // disks
    le(zip, static_cast<std::uint32_t>(names.size()), 2);
    le(zip, static_cast<std::uint32_t>(names.size()), 2);
    le(zip, static_cast<std::uint32_t>(central.size()), 4);
    le(zip, central_at, 4);
    le(zip, 0, 2);
    return zip;
}

// A zip from Python's zipfile (deflated): "Saves/" (a folder), "Saves/config" (3 1 0 1 1 1),
// "Saves/CTerms" (0 0) and "__MACOSX/Saves/._config" ("junk").
const Bytes kDeflatedZip = {
    0x50, 0x4B, 0x03, 0x04, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0x3C, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x53, 0x61,
    0x76, 0x65, 0x73, 0x2F, 0x50, 0x4B, 0x03, 0x04, 0x14, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
    0x21, 0x3C, 0x5D, 0x91, 0xE8, 0x65, 0x08, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x0C, 0x00,
    0x00, 0x00, 0x53, 0x61, 0x76, 0x65, 0x73, 0x2F, 0x63, 0x6F, 0x6E, 0x66, 0x69, 0x67, 0x63, 0x66,
    0x64, 0x60, 0x64, 0x64, 0x04, 0x00, 0x50, 0x4B, 0x03, 0x04, 0x14, 0x00, 0x00, 0x00, 0x08, 0x00,
    0x00, 0x00, 0x21, 0x3C, 0xFF, 0x12, 0xD9, 0x41, 0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x0C, 0x00, 0x00, 0x00, 0x53, 0x61, 0x76, 0x65, 0x73, 0x2F, 0x43, 0x54, 0x65, 0x72, 0x6D, 0x73,
    0x63, 0x60, 0x00, 0x00, 0x50, 0x4B, 0x03, 0x04, 0x14, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
    0x21, 0x3C, 0x39, 0x9C, 0xFB, 0x06, 0x06, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x17, 0x00,
    0x00, 0x00, 0x5F, 0x5F, 0x4D, 0x41, 0x43, 0x4F, 0x53, 0x58, 0x2F, 0x53, 0x61, 0x76, 0x65, 0x73,
    0x2F, 0x2E, 0x5F, 0x63, 0x6F, 0x6E, 0x66, 0x69, 0x67, 0xCB, 0x2A, 0xCD, 0xCB, 0x06, 0x00, 0x50,
    0x4B, 0x01, 0x02, 0x14, 0x03, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0x3C, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00, 0x00, 0x00, 0x53, 0x61, 0x76,
    0x65, 0x73, 0x2F, 0x50, 0x4B, 0x01, 0x02, 0x14, 0x03, 0x14, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00,
    0x00, 0x21, 0x3C, 0x5D, 0x91, 0xE8, 0x65, 0x08, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x0C,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x01, 0x24, 0x00, 0x00,
    0x00, 0x53, 0x61, 0x76, 0x65, 0x73, 0x2F, 0x63, 0x6F, 0x6E, 0x66, 0x69, 0x67, 0x50, 0x4B, 0x01,
    0x02, 0x14, 0x03, 0x14, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x21, 0x3C, 0xFF, 0x12, 0xD9,
    0x41, 0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x01, 0x56, 0x00, 0x00, 0x00, 0x53, 0x61, 0x76, 0x65, 0x73,
    0x2F, 0x43, 0x54, 0x65, 0x72, 0x6D, 0x73, 0x50, 0x4B, 0x01, 0x02, 0x14, 0x03, 0x14, 0x00, 0x00,
    0x00, 0x08, 0x00, 0x00, 0x00, 0x21, 0x3C, 0x39, 0x9C, 0xFB, 0x06, 0x06, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x17, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80,
    0x01, 0x84, 0x00, 0x00, 0x00, 0x5F, 0x5F, 0x4D, 0x41, 0x43, 0x4F, 0x53, 0x58, 0x2F, 0x53, 0x61,
    0x76, 0x65, 0x73, 0x2F, 0x2E, 0x5F, 0x63, 0x6F, 0x6E, 0x66, 0x69, 0x67, 0x50, 0x4B, 0x05, 0x06,
    0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x04, 0x00, 0xED, 0x00, 0x00, 0x00, 0xBF, 0x00, 0x00, 0x00,
    0x00, 0x00};

std::string stamp(int n) {
    char s[32];
    std::snprintf(s, sizeof s, "20261030-1200%02d-000", n);
    return s;
}

} // namespace

TEST_CASE("save_zip: CRC-32, a stored round trip and a deflated zip from Python") {
    const std::string check = "123456789";
    CHECK(fe::crc32({reinterpret_cast<const std::uint8_t *>(check.data()), check.size()}) ==
          0xCBF43926u);
    const std::vector<fe::ZipEntry> entries = {
        {"theif0", new_character()}, {"empty", {}}, {"Documents/config", kConfig}};
    const Bytes zip = fe::zip_store(entries, fe::dos_time(2026, 10, 30, 12, 34, 56));
    const std::vector<fe::ZipEntry> back = unzip(zip);
    REQUIRE(back.size() == entries.size());
    for (std::size_t i = 0; i < back.size(); i++) {
        CHECK(back[i].name == entries[i].name);
        CHECK(back[i].bytes == entries[i].bytes);
    }
    const std::vector<fe::ZipEntry> py = unzip(kDeflatedZip); // the folder is left out
    REQUIRE(py.size() == 3);
    CHECK(py[0].name == "Saves/config");
    CHECK(py[0].bytes == kConfig);
    CHECK(py[1].bytes == Bytes(2, 0));
    CHECK(py[2].bytes == Bytes({'j', 'u', 'n', 'k'}));
}

TEST_CASE("save_zip: damaged, hostile and unsupported zips are refused with a reason") {
    const Bytes good = fe::zip_store(std::vector<fe::ZipEntry>{{"theif0", new_character()}});
    std::vector<fe::ZipEntry> out;
    std::string why;
    const auto refused = [&](Bytes z, std::size_t limit = 1u << 20) {
        why.clear();
        const bool ok = fe::zip_read(z, limit, &out, &why);
        CHECK_FALSE(why.empty());
        return !ok;
    };
    CHECK(refused({}));
    CHECK(refused(Bytes(100, 0)));                        // no end record
    CHECK(refused(Bytes(good.begin(), good.end() - 1)));  // cut short
    CHECK(refused(Bytes(good.begin() + 10, good.end()))); // its start missing
    Bytes crc = good;
    crc[30 + 6 + 100] ^= 1; // a byte of theif0's data
    CHECK(refused(crc));
    CHECK(why.find("CRC") != std::string::npos);
    Bytes encrypted = good;
    encrypted[good.size() - 22 - 46 - 6 + 8] |= 1; // the central entry's flags
    CHECK(refused(encrypted));
    CHECK(why.find("encrypted") != std::string::npos);
    Bytes method = good;
    method[good.size() - 22 - 46 - 6 + 10] = 12; // bzip2
    CHECK(refused(method));
    CHECK(refused(good, 100)); // larger than the caller takes
    Bytes zip64 = good;
    zip64[good.size() - 22 + 16] = 0xFF, zip64[good.size() - 22 + 17] = 0xFF;
    zip64[good.size() - 22 + 18] = 0xFF, zip64[good.size() - 22 + 19] = 0xFF;
    CHECK(refused(zip64));
    const Bytes twice = fe::zip_store(
        std::vector<fe::ZipEntry>{{"theif0", new_character()}, {"theif0", new_character()}});
    CHECK(refused(twice));
    Bytes inflated = kDeflatedZip; // config's deflated data damaged
    inflated[0x54] ^= 0xFF;
    CHECK(refused(inflated));
}

TEST_CASE("save tools: export to a folder and a zip, and import back byte for byte") {
    const TempDir tmp("round-trip");
    const fs::path &root = tmp.path;
    const fe::SaveLocation from = location(root / "from");
    const Tree set = valid_set();
    write_tree(from.documents_dir, set);
    write_bytes(fs::path(from.documents_dir) / ("theif0" MT_SAVES_TMP_SUFFIX), {1, 2}); // skipped
    write_bytes(fs::path(from.documents_dir) / "notes.txt", {'x'});                     // not ours

    std::vector<std::string> exported;
    std::string why;
    REQUIRE_MESSAGE(fe::export_saves(from, (root / "out").string(), &exported, &why), why);
    CHECK(exported.size() == set.size());
    CHECK(tree(root / "out") == set);
    REQUIRE_MESSAGE(fe::export_saves(from, (root / "saves.ZIP").string(), nullptr, &why), why);
    std::map<std::string, Bytes> zipped;
    for (const fe::ZipEntry &e : unzip(read_bytes(root / "saves.ZIP"))) {
        zipped[e.name] = e.bytes;
    }
    CHECK(zipped == set);
    // Into the Documents/ folder itself: refused.
    CHECK_FALSE(fe::export_saves(from, from.documents_dir, nullptr, &why));

    for (const char *source : {"out", "saves.ZIP"}) {
        CAPTURE(source);
        const fe::SaveLocation to = location(root / (std::string("to-") + source));
        fe::ImportResult result;
        REQUIRE_MESSAGE(fe::import_saves(to, (root / source).string(), stamp(1), &result, &why),
                        why);
        CHECK(tree(to.documents_dir) == set);
        CHECK(result.imported.size() == set.size());
        CHECK(result.backup.empty()); // nothing was replaced
        CHECK_FALSE(fs::exists(to.backups_dir));
    }
}

TEST_CASE("save tools: hostile sets are refused with a reason and nothing is overwritten") {
    const TempDir tmp("hostile");
    const fs::path &root = tmp.path;
    const fe::SaveLocation loc = location(root);
    const Tree before = {
        {"theif0", new_character(9)}, {"quest0", new_quests()}, {"config", kConfig}};
    write_tree(loc.documents_dir, before);

    struct Case {
        const char *name;
        const char *file;
        const char *code;
        Tree set;
    };
    std::vector<Case> cases;
    const auto with = [](const char *file, Bytes bytes) {
        Tree t = valid_set();
        t[file] = std::move(bytes);
        return t;
    };
    Bytes cut = new_character();
    cut.resize(1000);
    cases.push_back({"truncated", "theif0", "bad_size", with("theif0", cut)});
    Bytes big = new_quests();
    big.push_back(0);
    cases.push_back({"oversized", "quest0", "bad_size", with("quest0", big)});
    cases.push_back({"oversized receipts", "crestore", "bad_size",
                     with("crestore", Bytes(MT_SAVES_MAX_RECEIPTS_BYTES + 1, 0))});
    Bytes id = new_character();
    id[0x015 + 16 * 3] = MT_SAVES_EQUIP_IDS; // an equipment id past item_equip
    cases.push_back({"equipment id", "theif0", "bad_item", with("theif0", id)});
    Bytes etc = new_character();
    etc[0x325 + 3 * 2] = 200;
    cases.push_back({"item id", "theif0", "bad_item", with("theif0", etc)});
    cases.push_back({"speed byte 3", "config", "bad_config", with("config", {3, 3, 0, 1, 1, 1})});
    Bytes map = new_character();
    map[0x41C] = 11;
    cases.push_back({"area", "theif0", "bad_map", with("theif0", map)});
    Tree pair = valid_set();
    pair.erase("quest2");
    cases.push_back({"missing quest", "set", "bad_pair", pair});

    for (const Case &c : cases) {
        CAPTURE(c.name);
        const fs::path src = root / "sources" / c.name;
        write_tree(src, c.set);
        fe::ImportResult result;
        std::string why;
        CHECK_FALSE(fe::import_saves(loc, src.string(), stamp(2), &result, &why));
        REQUIRE(result.problems.size() == 1);
        CHECK(result.problems[0].file == c.file);
        CHECK(result.problems[0].code == c.code);
        CHECK_FALSE(result.problems[0].why.empty());
        CHECK(why.find("nothing was imported") != std::string::npos);
        CHECK(result.imported.empty());
        CHECK(tree(loc.documents_dir) == before); // nothing overwritten
        CHECK_FALSE(fs::exists(loc.backups_dir)); // and no backup taken
        // The same as a zip.
        std::vector<fe::ZipEntry> entries;
        for (const auto &[name, bytes] : c.set) {
            entries.push_back({"Documents/" + name, bytes});
        }
        write_bytes(src.string() + ".zip", fe::zip_store(entries));
        CHECK_FALSE(fe::import_saves(loc, src.string() + ".zip", stamp(2), &result, &why));
        REQUIRE(result.problems.size() == 1);
        CHECK(result.problems[0].code == c.code);
        CHECK(tree(loc.documents_dir) == before);
    }
    // Not a save source at all.
    std::string why;
    CHECK_FALSE(fe::import_saves(loc, (root / "missing").string(), stamp(2), nullptr, &why));
    write_bytes(root / "not-a-zip.zip", {'h', 'i'});
    CHECK_FALSE(fe::import_saves(loc, (root / "not-a-zip.zip").string(), stamp(2), nullptr, &why));
    CHECK(why.find("zip") != std::string::npos);
    fs::create_directories(root / "empty");
    CHECK_FALSE(fe::import_saves(loc, (root / "empty").string(), stamp(2), nullptr, &why));
    CHECK(tree(loc.documents_dir) == before);
}

TEST_CASE("save tools: an import backs up what it replaces, and a failed write puts it back") {
    const TempDir tmp("import-backup");
    const fs::path &root = tmp.path;
    const fe::SaveLocation loc = location(root, 2);
    const Tree before = {{"theif0", new_character(9)},
                         {"quest0", new_quests()},
                         {"config", {3, 0, 0, 0, 0, 0}},
                         {"cupon", Bytes(0x174A, 0)}};
    write_tree(loc.documents_dir, before);
    const fs::path src = root / "src";
    write_tree(src, valid_set());

    fe::ImportResult result;
    std::string why;
    REQUIRE_MESSAGE(fe::import_saves(loc, src.string(), stamp(3), &result, &why), why);
    Tree want = valid_set();
    want["cupon"] = before.at("cupon"); // not in the import: kept
    CHECK(tree(loc.documents_dir) == want);
    // The replaced files, as they were.
    CHECK(result.backup == (fs::path(loc.backups_dir) / "import" / stamp(3)).string());
    CHECK(tree(result.backup) == Tree{{"theif0", before.at("theif0")},
                                      {"quest0", before.at("quest0")},
                                      {"config", before.at("config")}});
    // Imports keep max(keep, 1) backups: two more imports, the oldest goes.
    REQUIRE(fe::import_saves(loc, src.string(), stamp(4), &result, &why));
    REQUIRE(fe::import_saves(loc, src.string(), stamp(5), &result, &why));
    const std::vector<fe::BackupInfo> backups = fe::list_backups(loc);
    REQUIRE(backups.size() == 2);
    CHECK(backups[0].stamp == stamp(5));
    CHECK(backups[1].stamp == stamp(4));
    CHECK(backups[0].kind == "import");

    // A write that fails part way (theif2's temporary file cannot be made: a folder is in the
    // way): every file is as it was, theif0 and config included, and no new file is left.
    const fs::path fresh = root / "fresh";
    const fe::SaveLocation f = location(fresh);
    write_tree(f.documents_dir, before);
    fs::create_directories(fs::path(f.documents_dir) / ("theif2" MT_SAVES_TMP_SUFFIX));
    CHECK_FALSE(fe::import_saves(f, src.string(), stamp(6), &result, &why));
    CHECK(why.find("as they were") != std::string::npos);
    Tree now = tree(f.documents_dir);
    CHECK(now == before);
}

TEST_CASE("save tools: backups rotate at N and skip an unchanged save") {
    const TempDir tmp("rotate");
    const fs::path &root = tmp.path;
    fe::SaveLocation loc = location(root, 3);
    const fs::path docs = loc.documents_dir;
    std::string why;
    // No character in the slot: nothing.
    CHECK(fe::backup_slot(loc, 1, stamp(0), &why).empty());
    // A folder of the player's own in the slot's backups: never rotated away.
    fs::create_directories(fs::path(loc.backups_dir) / "1" / "keep me");
    for (int i = 1; i <= 5; i++) {
        write_bytes(docs / "theif1", new_character(static_cast<std::uint8_t>(i)));
        write_bytes(docs / "quest1", new_quests(static_cast<std::uint8_t>(i)));
        const std::string made = fe::backup_slot(loc, 1, stamp(i), &why);
        CHECK(made == (fs::path(loc.backups_dir) / "1" / stamp(i)).string());
        // The same bytes again: no new backup.
        CHECK(fe::backup_slot(loc, 1, stamp(i + 10), &why).empty());
    }
    std::vector<fe::BackupInfo> b = fe::list_backups(loc);
    REQUIRE(b.size() == 3); // N = 3: the newest three
    CHECK(b[0].stamp == stamp(5));
    CHECK(b[1].stamp == stamp(4));
    CHECK(b[2].stamp == stamp(3));
    CHECK(b[0].kind == "1");
    CHECK(b[0].files == std::vector<std::string>{"quest1", "theif1"});
    CHECK(tree(b[0].path) == Tree{{"quest1", new_quests(5)}, {"theif1", new_character(5)}});
    CHECK(fs::is_directory(fs::path(loc.backups_dir) / "1" / "keep me"));
    // Two in one millisecond: the second gets "-1", and it is the newer.
    write_bytes(docs / "theif1", new_character(42));
    CHECK(fe::backup_slot(loc, 1, stamp(5), &why) ==
          (fs::path(loc.backups_dir) / "1" / (stamp(5) + "-1")).string());
    b = fe::list_backups(loc);
    REQUIRE(b.size() == 3);
    CHECK(b[0].stamp == stamp(5) + "-1");
    // N = 0: no backups at all, the old ones stay.
    loc.keep = 0;
    write_bytes(docs / "theif1", new_character(43));
    CHECK(fe::backup_slot(loc, 1, stamp(9), &why).empty());
    CHECK(fe::list_backups(loc).size() == 3);
    // A smaller N rotates at the next backup.
    loc.keep = 1;
    CHECK_FALSE(fe::backup_slot(loc, 1, stamp(9), &why).empty());
    CHECK(fe::list_backups(loc).size() == 1);
}

TEST_CASE("save tools: a restore puts a backup back and keeps what it replaced") {
    const TempDir tmp("restore");
    const fs::path &root = tmp.path;
    const fe::SaveLocation loc = location(root);
    const fs::path docs = loc.documents_dir;
    write_bytes(docs / "theif0", new_character(1));
    write_bytes(docs / "quest0", new_quests(1));
    std::string why;
    REQUIRE_FALSE(fe::backup_slot(loc, 0, stamp(1), &why).empty());
    write_bytes(docs / "theif0", new_character(2)); // a later save
    write_bytes(docs / "quest0", new_quests(2));
    const std::vector<fe::BackupInfo> b = fe::list_backups(loc);
    REQUIRE(b.size() == 1);
    fe::ImportResult result;
    REQUIRE_MESSAGE(fe::restore_backup(loc, b[0], stamp(2), &result, &why), why);
    CHECK(read_bytes(docs / "theif0") == new_character(1));
    CHECK(read_bytes(docs / "quest0") == new_quests(1));
    CHECK(tree(result.backup) == Tree{{"quest0", new_quests(2)}, {"theif0", new_character(2)}});
}

TEST_CASE("save tools: the cash shop's counters go with their slot (#156)") {
    const TempDir tmp("counters");
    const fs::path &root = tmp.path;
    const fe::SaveLocation loc = location(root);
    const fs::path docs = loc.documents_dir;
    const std::string text = "# MapleThief: the cash shop's counters\nlevel = 12\ndeaths = 3\n";
    const Bytes counters(text.begin(), text.end());
    write_bytes(docs / "theif0", new_character(1));
    write_bytes(docs / "quest0", new_quests(1));
    write_bytes(docs / "maplethief-cashshop0", counters);
    std::string why;
    // A backup holds them; an export too.
    const std::string made = fe::backup_slot(loc, 0, stamp(1), &why);
    REQUIRE_FALSE(made.empty());
    CHECK(tree(made) == Tree{{"maplethief-cashshop0", counters},
                             {"quest0", new_quests(1)},
                             {"theif0", new_character(1)}});
    CHECK(fe::backup_slot(loc, 0, stamp(2), &why).empty()); // unchanged, counters included
    const fs::path out = root / "out";
    REQUIRE(fe::export_saves(loc, out.string(), nullptr, &why));
    CHECK(tree(out) == tree(docs));

    // A character imported without counters: the old character's counters go (backed up).
    const fs::path src = root / "src";
    write_tree(src, {{"theif0", new_character(5)}, {"quest0", new_quests(5)}});
    fe::ImportResult result;
    REQUIRE_MESSAGE(fe::import_saves(loc, src.string(), stamp(3), &result, &why), why);
    CHECK(result.removed == std::vector<std::string>{"maplethief-cashshop0"});
    CHECK_FALSE(fs::exists(docs / "maplethief-cashshop0"));
    CHECK(read_bytes(fs::path(result.backup) / "maplethief-cashshop0") == counters);
    // With its counters: they come in. Damaged counters: refused, nothing written.
    write_bytes(src / "maplethief-cashshop0", counters);
    REQUIRE_MESSAGE(fe::import_saves(loc, src.string(), stamp(4), &result, &why), why);
    CHECK(result.removed.empty());
    CHECK(read_bytes(docs / "maplethief-cashshop0") == counters);
    write_bytes(src / "maplethief-cashshop0", Bytes{0x00, 0xFF});
    write_bytes(src / "theif0", new_character(6));
    CHECK_FALSE(fe::import_saves(loc, src.string(), stamp(5), &result, &why));
    REQUIRE(result.problems.size() == 1);
    CHECK(result.problems[0].code == "bad_counters");
    CHECK(read_bytes(docs / "theif0") == new_character(5));
    // Another slot's counters are left alone by an import of slot 0.
    write_bytes(docs / "maplethief-cashshop1", counters);
    write_bytes(src / "maplethief-cashshop0", counters);
    REQUIRE(fe::import_saves(loc, src.string(), stamp(6), &result, &why));
    CHECK(read_bytes(docs / "maplethief-cashshop1") == counters);
}

namespace {

fs::path g_hook_docs;
int g_saves = 0;

// What the game's save pair leaves behind: the slot's two files written (here, slot 2).
void fake_save_quest_data(mt_cpu *MT_RESTRICT cpu) {
    g_saves++;
    write_bytes(g_hook_docs / "theif2", new_character(static_cast<std::uint8_t>(g_saves)));
    write_bytes(g_hook_docs / "quest2", new_quests(static_cast<std::uint8_t>(g_saves)));
    cpu->r[0] = 0; // void; the hook leaves the registers to the original
}

// SaveQuestData as the recompiler writes a hooked function (recomp/lift/core.py).
void save_quest_data(mt_cpu *MT_RESTRICT cpu) {
    MT_FN_HOOK(MT_HOOK_FN_SAVE_QUEST_DATA, fake_save_quest_data);
}

} // namespace

TEST_CASE("save tools: the hook on CDataMgr::SaveQuestData backs up the saved slot after it") {
    const TempDir tmp("hook");
    const fs::path &root = tmp.path;
    REQUIRE(mt_hook_hookable(MT_HOOK_FN_SAVE_QUEST_DATA) == 1);
    mt_hook_reset();
    REQUIRE(mt_stubs_set_data_dir(root.string().c_str()) == 0);
    fe::Settings settings;
    settings.save_dir = (root / "cloud" / "MapleThief").string();
    settings.save_backups = 2;
    const fe::SaveLocation loc =
        fe::configure_save_location(settings, fe::data_location_at(root.string()));
    CHECK(loc.documents_dir == settings.save_dir);
    CHECK(std::string(mt_saves_documents_dir()) == settings.save_dir);
    CHECK(loc.backups_dir == (root / "backups").string());
    CHECK(fe::save_location() == loc);
    g_hook_docs = loc.documents_dir;
    fe::install_save_backups();
    CHECK(mt_hook_enabled(MT_HOOK_FN_SAVE_QUEST_DATA) == 1);

    mt_cpu cpu{};
    for (int i = 1; i <= 3; i++) {
        cpu.r[MT_LR] = 0x1235u;
        save_quest_data(&cpu);
        CHECK(cpu.r[MT_LR] == 0x1235u);
    }
    CHECK(g_saves == 3); // the original ran each time, once
    const mt_hook_stats st = mt_hook_get_stats(MT_HOOK_FN_SAVE_QUEST_DATA);
    CHECK(st.handled == 3);
    const std::vector<fe::BackupInfo> b = fe::list_backups(loc);
    REQUIRE(b.size() == 2); // save_backups = 2
    CHECK(b[0].kind == "2");
    CHECK(tree(b[0].path) == Tree{{"quest2", new_quests(3)}, {"theif2", new_character(3)}});
    CHECK(tree(b[1].path) == Tree{{"quest2", new_quests(2)}, {"theif2", new_character(2)}});

    // save_backups = 0: registered off, and the save runs as without it.
    settings.save_backups = 0;
    fe::configure_save_location(settings, fe::data_location_at(root.string()));
    fe::install_save_backups();
    CHECK(mt_hook_enabled(MT_HOOK_FN_SAVE_QUEST_DATA) == 0);
    save_quest_data(&cpu);
    CHECK(g_saves == 4);
    CHECK(fe::list_backups(loc).size() == 2);

    mt_hook_reset();
    mt_saves_set_documents_dir(nullptr);
    mt_stubs_set_gamecenter_path(nullptr);
    mt_stubs_set_data_dir(nullptr);
}

TEST_CASE("save tools: reading folders and zips of saves") {
    const TempDir tmp("read");
    const fs::path &root = tmp.path;
    fe::SaveSet files;
    std::vector<std::string> ignored;
    std::string why;
    // A copy of a device's or the data folder's tree: the files are in Documents/.
    write_tree(root / "copy" / "Documents", {{"config", kConfig}});
    write_bytes(root / "copy" / "settings.ini", {'x'});
    REQUIRE(fe::read_save_source((root / "copy").string(), &files, &ignored, &why));
    REQUIRE(files.size() == 1);
    CHECK(files[0].name == "config");
    // Dot files and temporary files are left out; other names are listed.
    write_tree(root / "flat", {{"config", kConfig},
                               {".DS_Store", {1}},
                               {"Thumbs.db", {1}},
                               {"quest0" MT_SAVES_TMP_SUFFIX, {1}},
                               {"theif3", {1}}});
    REQUIRE(fe::read_save_source((root / "flat").string(), &files, &ignored, &why));
    CHECK(files.size() == 1);
    CHECK(ignored == std::vector<std::string>{"Thumbs.db", "theif3"});
    // Python's deflated zip: the folder, the __MACOSX junk left out.
    write_bytes(root / "py.zip", kDeflatedZip);
    REQUIRE_MESSAGE(fe::read_save_source((root / "py.zip").string(), &files, &ignored, &why), why);
    REQUIRE(files.size() == 2);
    CHECK(files[0].name == "CTerms");
    CHECK(files[1].name == "config");
    CHECK(files[1].bytes == kConfig);
    CHECK(ignored.empty());
    // One name in two folders of a zip: which one is meant is not clear.
    write_bytes(root / "twice.zip", fe::zip_store(std::vector<fe::ZipEntry>{
                                        {"a/config", kConfig}, {"b/config", kConfig}}));
    CHECK_FALSE(fe::read_save_source((root / "twice.zip").string(), &files, &ignored, &why));
    CHECK(why.find("twice") != std::string::npos);
    // The checks on a whole set: a pair, and each file.
    CHECK(fe::check_save_set({{"theif0", new_character()}, {"quest0", new_quests()}}).empty());
    const std::vector<fe::SaveProblem> p = fe::check_save_set({{"quest1", new_quests()}});
    REQUIRE(p.size() == 1);
    CHECK(p[0].line() == "set: bad_pair: quest1 comes without theif1; a character is both files");
}

TEST_CASE("save tools: a zip's names are filtered before anything is inflated, and a set is "
          "capped in total (#324)") {
    const TempDir tmp("caps");
    const fs::path &root = tmp.path;
    fe::SaveSet files;
    std::vector<std::string> ignored;
    std::string why;
    // Five saves of 16 MiB each, a few kilobytes deflated: each within its cap, over the set's.
    const std::vector<std::string> names = {"theif0", "quest0", "theif1", "quest1", "theif2"};
    const Bytes big = zip_of_zeros(names, static_cast<std::uint32_t>(fe::kMaxSaveSourceFileBytes));
    REQUIRE(big.size() < 1u << 20);
    std::vector<fe::ZipEntry> entries;
    REQUIRE_MESSAGE(fe::zip_read(zip_of_zeros({"theif0"}, 1000), 1000, &entries, &why), why);
    CHECK(entries.at(0).bytes == Bytes(1000, 0));
    write_bytes(root / "big.zip", big);
    files.clear();
    CHECK_FALSE(fe::read_save_source((root / "big.zip").string(), &files, &ignored, &why));
    CHECK(why.find("more than 67108864 bytes in all") != std::string::npos);
    CHECK(files.empty());
    // An entry that is no save is never inflated: its damaged data does not refuse the zip.
    Bytes junk = fe::zip_store(std::vector<fe::ZipEntry>{
        {"config", kConfig}, {"notes/junk.bin", Bytes{'J', 'U', 'N', 'K', 'J', 'U', 'N', 'K'}}});
    const auto at = std::search(junk.begin(), junk.end(), std::begin("JUNKJUNK"),
                                std::begin("JUNKJUNK") + 8);
    REQUIRE(at != junk.end());
    *at ^= 1; // its CRC fails
    write_bytes(root / "junk.zip", junk);
    REQUIRE_MESSAGE(fe::read_save_source((root / "junk.zip").string(), &files, &ignored, &why),
                    why);
    REQUIRE(files.size() == 1);
    CHECK(files[0].name == "config");
    CHECK(ignored == std::vector<std::string>{"notes/junk.bin"});
}

TEST_CASE("save tools: the base folder around the executable or the app (ADR 0127)") {
    CHECK(fe::portable_base_dir("/Games/MapleThief/") == "/Games/MapleThief");
    CHECK(fe::portable_base_dir("C:\\Games\\MapleThief\\") == "C:\\Games\\MapleThief");
    CHECK(fe::portable_base_dir("/Applications/Games/MapleThief.app/Contents/Resources/") ==
          "/Applications/Games");
    CHECK(fe::portable_base_dir("/Users/a/MapleThief.app/Contents/MacOS") == "/Users/a");
}

TEST_CASE("settings: save_dir is an absolute folder or empty; save_backups 0 to 100 (#148)") {
    fe::Settings s;
    CHECK(s.save_dir.empty());
    CHECK(s.save_backups == 10u);
    CHECK(fe::apply_setting(&s, "save_dir", "/Users/me/Dropbox/MapleThief", nullptr));
    CHECK(s.save_dir == "/Users/me/Dropbox/MapleThief");
    CHECK(fe::apply_setting(&s, "save_dir", "C:\\Users\\me\\OneDrive\\Saves", nullptr));
    CHECK(fe::apply_setting(&s, "save_dir", "D:/Saves", nullptr));
    std::string why;
    CHECK_FALSE(fe::apply_setting(&s, "save_dir", "relative/folder", &why));
    CHECK(why.find("absolute") != std::string::npos);
    CHECK_FALSE(fe::apply_setting(&s, "save_dir", "/a\tb", nullptr));
    CHECK(s.save_dir == "D:/Saves");
    CHECK(fe::apply_setting(&s, "save_dir", "", nullptr));
    CHECK(s.save_dir.empty());
    CHECK(fe::apply_setting(&s, "save_backups", "0", nullptr));
    CHECK_FALSE(fe::apply_setting(&s, "save_backups", "101", nullptr));
    // The file: an empty save_dir is "save_dir =" and reads back as empty.
    fe::Settings t;
    t.save_backups = 25;
    const std::string text = fe::serialise_settings(t);
    CHECK(text.find("\nsave_dir =\n") != std::string::npos);
    CHECK(text.find("\nsave_backups = 25\n") != std::string::npos);
    fe::Settings u;
    u.save_dir = "/x";
    CHECK(fe::parse_settings(text, &u).empty());
    CHECK(u == t);
    const fe::SettingInfo *row = fe::find_setting_flag("--save-dir");
    REQUIRE(row != nullptr);
    CHECK(row->group == fe::SettingGroup::kSaves);
    CHECK_FALSE(row->presets.in_presets());
}

TEST_CASE("settings menu: the Saves actions export to exports/, import from import/, count "
          "backups (#89)") {
    const TempDir tmp("menu-actions");
    const fs::path &root = tmp.path;
    const fe::SaveLocation loc = location(root);
    const Tree set = valid_set();
    write_tree(loc.documents_dir, set);
    CHECK(fe::backups_action(loc) == "No backups yet: one is kept after each save");
    CHECK(fe::backups_action(location(root, 0)) == "No backups (Backups per character is 0)");

    // Export: a zip named by the stamp in <data dir>/exports/.
    CHECK(fe::export_saves_action(loc, stamp(1)) ==
          "Exported to exports/MapleThief-saves-" + stamp(1) + ".zip");
    const fs::path zip = root / "exports" / ("MapleThief-saves-" + stamp(1) + ".zip");
    std::map<std::string, Bytes> zipped;
    for (const fe::ZipEntry &e : unzip(read_bytes(zip))) {
        zipped[e.name] = e.bytes;
    }
    CHECK(zipped == set);

    // Import: never with a character loaded (the key pad's panel is 0 in the field).
    CHECK(fe::import_saves_action(loc, 0, stamp(2)) ==
          "Import from the title or the main menu (a character is loaded)");
    // An empty import/ folder (made by the action): nothing to import.
    CHECK(fe::import_saves_action(loc, -1, stamp(2)).rfind("Not imported: ", 0) == 0);
    CHECK(fs::is_directory(root / "import"));
    // The newest zip by name: the export, after the player's other changes.
    fs::copy_file(zip, root / "import" / zip.filename());
    write_bytes(fs::path(loc.documents_dir) / "theif0", new_character(9));
    CHECK(fe::import_saves_action(loc, -1, stamp(3)) == "Imported " + std::to_string(set.size()) +
                                                            " files from import/" +
                                                            zip.filename().string());
    CHECK(tree(loc.documents_dir) == set);
    CHECK(fe::backups_action(loc) == "Backups: imports: 1 (in backups/)");
    // A broken zip that sorts last is refused, and nothing changes.
    write_bytes(root / "import" / "zz.zip", {'P', 'K', 3, 4, 0});
    CHECK(fe::import_saves_action(loc, -1, stamp(4)).rfind("Not imported: ", 0) == 0);
    CHECK(tree(loc.documents_dir) == set);
    // A slot's backups count too.
    REQUIRE_FALSE(fe::backup_slot(loc, 0, stamp(5), nullptr).empty());
    CHECK(fe::backups_action(loc) == "Backups: slot 0: 1, imports: 1 (in backups/)");
}

// --- The Saves tab (#192, save_menu.h) ---------------------------------------------------------

namespace {

// A character with this job and level (still one the checks pass).
Bytes hero(int branch, int tier, int level, std::uint8_t seed = 0) {
    Bytes b = new_character(seed);
    b[0] = static_cast<std::uint8_t>(branch);
    b[1] = static_cast<std::uint8_t>(tier);
    b[2] = static_cast<std::uint8_t>(level);
    return b;
}

// A file dialog the test answers: what it was asked, and the answer to give.
struct FakeDialog {
    int shown = 0;
    fe::FileDialogRequest request;
    std::function<void(fe::FileDialogResult)> answer;

    void install(fe::FileDialogs &d) {
        d.set_backend([this](SDL_Window *, const fe::FileDialogRequest &r,
                             std::function<void(fe::FileDialogResult)> a) {
            shown++;
            request = r;
            answer = std::move(a);
            return true;
        });
    }
    void reply(fe::FileDialogResult::Outcome o, const std::string &path = "",
               const std::string &why = "") {
        REQUIRE(answer);
        answer(fe::FileDialogResult{o, path, why});
        answer = nullptr;
    }
};

} // namespace

TEST_CASE("save menu: the job's name and the character from theif's first bytes") {
    CHECK(fe::job_name(0, 0) == "Beginner");
    CHECK(fe::job_name(0, 1) == "Rogue");
    CHECK(fe::job_name(1, 2) == "Assassin");
    CHECK(fe::job_name(1, 3) == "Hermit");
    CHECK(fe::job_name(1, 4) == "Night Lord");
    CHECK(fe::job_name(2, 2) == "Bandit");
    CHECK(fe::job_name(2, 3) == "Chief Bandit");
    CHECK(fe::job_name(2, 4) == "Shadower");
    CHECK(fe::job_name(0, 3) == "Rogue"); // no branch past the first job
    CHECK(fe::character_text(hero(1, 3, 47)) == "Lv 47 Hermit");
    CHECK(fe::character_text(new_character()) == "Lv 1 Beginner");
    CHECK(fe::character_text({}) == "");
    CHECK(fe::character_text(hero(3, 2, 10)) == "");  // bad_job
    CHECK(fe::character_text(hero(1, 2, 0)) == "");   // bad_level
    CHECK(fe::character_text(hero(1, 2, 101)) == ""); // bad_level
}

TEST_CASE("save menu: a backup's time from its folder's name") {
    CHECK(fe::backup_time_text("20261030-120001-000", false) == "2026-10-30 12:00:01");
    CHECK(fe::backup_time_text("20261231-235959-999-2", false) == "2026-12-31 23:59:59");
    CHECK(fe::backup_time_text("not-a-stamp", false) == "not-a-stamp");
    CHECK(fe::backup_time_text("20261030-12", false) == "20261030-12");
    // Local time: the same moment (SDL's local offset), in the same form.
    const std::string local = fe::backup_time_text("20261030-120001-000", true);
    CHECK(local.size() == 19);
    CHECK(local.substr(14) == "00:01");
}

TEST_CASE("save menu: the backup list's rows from synthetic backups, newest first") {
    const TempDir tmp("rows");
    const fe::SaveLocation loc = location(tmp.path);
    const fs::path docs = loc.documents_dir;
    std::string why;
    write_bytes(docs / "theif0", hero(1, 3, 23));
    write_bytes(docs / "quest0", new_quests(1));
    REQUIRE_FALSE(fe::backup_slot(loc, 0, stamp(1), &why).empty());
    write_bytes(docs / "theif2", hero(2, 2, 31, 7));
    write_bytes(docs / "quest2", new_quests(3));
    REQUIRE_FALSE(fe::backup_slot(loc, 2, stamp(2), &why).empty());
    write_bytes(docs / "theif0", hero(1, 3, 24));
    REQUIRE_FALSE(fe::backup_slot(loc, 0, stamp(3), &why).empty());
    // An import's backup: the files a restore replaced (slot 1's level 24 hero).
    fe::BackupInfo first;
    for (const fe::BackupInfo &b : fe::list_backups(loc)) {
        if (b.kind == "0" && b.stamp == stamp(1)) {
            first = b;
        }
    }
    REQUIRE_FALSE(first.path.empty());
    fe::ImportResult result;
    REQUIRE_MESSAGE(fe::restore_backup(loc, first, stamp(4), &result, &why), why);

    const std::vector<fe::BackupRow> rows = fe::backup_rows(loc, false);
    REQUIRE(rows.size() == 4);
    CHECK(rows[0].backup.stamp == stamp(4));
    CHECK(rows[0].slot == -1);
    CHECK(rows[0].label == "Replaced: slot 1, Lv 24 Hermit");
    CHECK(rows[1].backup.stamp == stamp(3));
    CHECK(rows[1].label == "Slot 1  Lv 24 Hermit");
    CHECK(rows[1].time == "2026-10-30 12:00:03");
    CHECK(rows[2].backup.stamp == stamp(2));
    CHECK(rows[2].slot == 2);
    CHECK(rows[2].label == "Slot 3  Lv 31 Bandit");
    CHECK(rows[3].label == "Slot 1  Lv 23 Hermit");
    CHECK(rows[3].character == "Lv 23 Hermit");
    CHECK(fe::restore_question(rows[3]) ==
          "Restore slot 1 from 2026-10-30 12:00:01? The current save is backed up first.");
    CHECK(fe::restore_question(rows[0]) ==
          "Put back the files replaced at 2026-10-30 12:00:04? The current save is backed up "
          "first.");

    // The menu's list: the same rows; no question with a character loaded.
    std::int32_t panel = -1;
    int stamps = 10;
    const fe::MenuList list = fe::backups_list(
        loc, [&panel] { return panel; }, [&stamps] { return stamp(stamps++); }, false);
    REQUIRE(list.rows.size() == 4);
    CHECK(list.rows[3].label == "Slot 1  Lv 23 Hermit");
    CHECK(list.rows[3].value == "2026-10-30 12:00:01");
    CHECK(list.question(3) == fe::restore_question(rows[3]));
    panel = 0;
    CHECK(list.question(3).empty());
    CHECK(list.choose(3) == "Restore from the title or the main menu (a character is loaded)");
    // Nothing to list: no rows (the menu then shows backups_action's line).
    const TempDir empty("rows-empty");
    CHECK(fe::backups_list(location(empty.path), nullptr, nullptr).rows.empty());
}

TEST_CASE("save menu: restoring a backup replaces the slot and first backs up the current files; "
          "refused outside the title") {
    const TempDir tmp("restore-action");
    const fe::SaveLocation loc = location(tmp.path);
    const fs::path docs = loc.documents_dir;
    std::string why;
    // The first save, then the hero played on and saved again.
    write_bytes(docs / "theif1", hero(1, 2, 12));
    write_bytes(docs / "quest1", new_quests(1));
    write_bytes(docs / "config", kConfig);
    REQUIRE_FALSE(fe::backup_slot(loc, 1, stamp(1), &why).empty());
    Bytes later = hero(1, 3, 30);
    later[0x41C] = 4, later[0x41D] = 2; // another map
    write_bytes(docs / "theif1", later);
    write_bytes(docs / "quest1", new_quests(2));
    REQUIRE_FALSE(fe::backup_slot(loc, 1, stamp(2), &why).empty());
    const std::vector<fe::BackupRow> rows = fe::backup_rows(loc, false);
    REQUIRE(rows.size() == 2);
    const fe::BackupRow &first = rows[1];
    REQUIRE(first.backup.stamp == stamp(1));

    // With a character loaded (the key pad's panel 0): refused, nothing written or backed up.
    const Tree docs_before = tree(docs);
    CHECK(fe::restore_backup_action(loc, first, 0, stamp(3)) ==
          "Restore from the title or the main menu (a character is loaded)");
    CHECK(tree(docs) == docs_before);
    CHECK_FALSE(fs::exists(fs::path(loc.backups_dir) / "import"));

    // At the title: the slot is the first save's again; the replaced files are in backups/import.
    CHECK(fe::restore_backup_action(loc, first, -1, stamp(3)) ==
          "Restored slot 2 from 2026-10-30 12:00:01 (the save it replaced is in backups/import)");
    CHECK(read_bytes(docs / "theif1") == hero(1, 2, 12));
    CHECK(read_bytes(docs / "quest1") == new_quests(1));
    CHECK(read_bytes(docs / "config") == kConfig); // not in the backup: left alone
    CHECK(tree(fs::path(loc.backups_dir) / "import" / stamp(3)) ==
          Tree{{"quest1", new_quests(2)}, {"theif1", later}});
    // That backup is in the list now, newest, and puts the later save back.
    const std::vector<fe::BackupRow> after = fe::backup_rows(loc, false);
    REQUIRE(after.size() == 3);
    CHECK(after[0].label == "Replaced: slot 2, Lv 30 Hermit");
    CHECK(fe::restore_backup_action(loc, after[0], -1, stamp(4)).rfind("Restored the files", 0) ==
          0);
    CHECK(read_bytes(docs / "theif1") == later);

    // A backup broken on the disk is refused by the checks, and nothing changes.
    write_bytes(fs::path(first.backup.path) / "theif1", Bytes(10, 0));
    const Tree now = tree(docs);
    CHECK(fe::restore_backup_action(loc, first, -1, stamp(5)).rfind("Not restored: theif1: ", 0) ==
          0);
    CHECK(tree(docs) == now);
}

TEST_CASE("save menu: Export and Import through the file dialog's seam; the folders as the "
          "fallback") {
    const TempDir tmp("dialogs");
    const fs::path &root = tmp.path;
    const fe::SaveLocation loc = location(root);
    const Tree set = valid_set();
    write_tree(loc.documents_dir, set);
    fe::FileDialogs dialogs;
    FakeDialog fake;
    fake.install(dialogs);
    std::vector<std::string> said;
    fe::SavesDialogs d{&dialogs, nullptr, fe::DialogRoute::kDialog,
                       [&said](std::string s) { said.push_back(std::move(s)); }};

    // Export: the save dialog at exports/<name>.zip; the chosen name gets ".zip".
    CHECK(fe::export_saves_dialog(d, loc, stamp(1)) == "Choose where to save the zip...");
    CHECK(fake.shown == 1);
    CHECK(fake.request.kind == fe::FileDialogRequest::Kind::kSaveFile);
    CHECK(fake.request.default_location ==
          (root / "exports" / ("MapleThief-saves-" + stamp(1) + ".zip")).string());
    REQUIRE(fake.request.filters.size() == 1);
    CHECK(fake.request.filters[0].pattern == "zip");
    CHECK(dialogs.busy());
    CHECK(fe::export_saves_dialog(d, loc, stamp(1)) == "A file dialog is open: finish it first");
    CHECK(fake.shown == 1);
    const fs::path chosen = root / "elsewhere" / "my saves";
    fs::create_directories(chosen.parent_path());
    fake.reply(fe::FileDialogResult::Outcome::kChosen, chosen.string());
    CHECK(said.empty()); // the answer waits for the frontend's poll
    CHECK(dialogs.poll() == 1);
    CHECK(said == std::vector<std::string>{"Exported to my saves.zip"});
    std::map<std::string, Bytes> zipped;
    for (const fe::ZipEntry &e : unzip(read_bytes(root / "elsewhere" / "my saves.zip"))) {
        zipped[e.name] = e.bytes;
    }
    CHECK(zipped == set);
    CHECK_FALSE(dialogs.busy());

    // Cancelled: nothing is written. Failed (no dialog here): exports/ as before.
    said.clear();
    fe::export_saves_dialog(d, loc, stamp(2));
    fake.reply(fe::FileDialogResult::Outcome::kCancelled);
    dialogs.poll();
    CHECK(said == std::vector<std::string>{"Export cancelled"});
    CHECK_FALSE(fs::exists(root / "exports" / ("MapleThief-saves-" + stamp(2) + ".zip")));
    said.clear();
    fe::export_saves_dialog(d, loc, stamp(3));
    fake.reply(fe::FileDialogResult::Outcome::kFailed, "", "no portal");
    dialogs.poll();
    CHECK(said == std::vector<std::string>{"Exported to exports/MapleThief-saves-" + stamp(3) +
                                           ".zip (no file dialog)"});
    // The folder route (iOS, a fullscreen Linux window, the Deck): no dialog at all.
    fe::SavesDialogs folders = d;
    folders.route = fe::DialogRoute::kFolder;
    CHECK(fe::export_saves_dialog(folders, loc, stamp(4)) ==
          "Exported to exports/MapleThief-saves-" + stamp(4) + ".zip");
    CHECK(fake.shown == 3);

    // Import: never with a character loaded (asked before the dialog, and again at its answer).
    std::int32_t panel = 0;
    int n = 10;
    const auto panel_fn = [&panel] { return panel; };
    const auto stamp_fn = [&n] { return stamp(n++); };
    CHECK(fe::import_saves_dialog(d, loc, panel_fn, stamp_fn) ==
          "Import from the title or the main menu (a character is loaded)");
    CHECK(fake.shown == 3);
    panel = -1;
    CHECK(fe::import_saves_dialog(d, loc, panel_fn, stamp_fn) ==
          "Choose a zip of saves to import...");
    CHECK(fake.request.kind == fe::FileDialogRequest::Kind::kOpenFile);
    CHECK(fake.request.default_location == (root / "import").string());
    CHECK(fs::is_directory(root / "import"));
    // The player changed a save meanwhile; the zip puts the exported set back, backed up first.
    write_bytes(fs::path(loc.documents_dir) / "theif0", hero(1, 4, 90));
    said.clear();
    fake.reply(fe::FileDialogResult::Outcome::kChosen,
               (root / "elsewhere" / "my saves.zip").string());
    dialogs.poll();
    CHECK(said == std::vector<std::string>{"Imported " + std::to_string(set.size()) +
                                           " files from my saves.zip"});
    CHECK(tree(loc.documents_dir) == set);
    CHECK(read_bytes(fs::path(loc.backups_dir) / "import" / stamp(11) / "theif0") ==
          hero(1, 4, 90));
    // A character loaded by the time it answers: refused.
    fe::import_saves_dialog(d, loc, panel_fn, stamp_fn);
    panel = 0;
    said.clear();
    fake.reply(fe::FileDialogResult::Outcome::kChosen,
               (root / "elsewhere" / "my saves.zip").string());
    dialogs.poll();
    CHECK(said == std::vector<std::string>{
                      "Import from the title or the main menu (a character is loaded)"});
    // A broken zip is refused by the checks.
    panel = -1;
    write_bytes(root / "broken.zip", {'P', 'K', 3, 4, 0});
    fe::import_saves_dialog(d, loc, panel_fn, stamp_fn);
    said.clear();
    fake.reply(fe::FileDialogResult::Outcome::kChosen, (root / "broken.zip").string());
    dialogs.poll();
    REQUIRE(said.size() == 1);
    CHECK(said[0].rfind("Not imported: ", 0) == 0);
    CHECK(tree(loc.documents_dir) == set);
    // Failed: the import folder's newest zip, as before.
    fs::copy_file(root / "elsewhere" / "my saves.zip", root / "import" / "a.zip");
    fe::import_saves_dialog(d, loc, panel_fn, stamp_fn);
    said.clear();
    fake.reply(fe::FileDialogResult::Outcome::kFailed, "", "no portal");
    dialogs.poll();
    CHECK(said == std::vector<std::string>{"Imported " + std::to_string(set.size()) +
                                           " files from import/a.zip (no file dialog)"});
    // A backend that cannot even ask: the folder at once.
    dialogs.set_backend([](SDL_Window *, const fe::FileDialogRequest &,
                           std::function<void(fe::FileDialogResult)>) { return false; });
    CHECK(fe::import_saves_dialog(d, loc, panel_fn, stamp_fn) ==
          "Imported " + std::to_string(set.size()) + " files from import/a.zip (no file dialog)");
}

TEST_CASE("save menu: Open data folder hands the folder's file:// URL to the opener") {
    const TempDir tmp("open folder");
    const fe::SaveLocation loc = location(tmp.path);
    fe::FileDialogs dialogs;
    std::vector<std::string> urls;
    bool works = true;
    dialogs.set_url_opener([&](const std::string &url, std::string *why) {
        urls.push_back(url);
        if (!works) {
            *why = "no file manager";
        }
        return works;
    });
    CHECK(fe::open_data_folder_action(dialogs, loc) == "Opened " + loc.data_dir);
    REQUIRE(urls.size() == 1);
    CHECK(urls[0] == fe::file_url(loc.data_dir));
    CHECK(urls[0].find("open%20folder") != std::string::npos);
    works = false;
    CHECK(fe::open_data_folder_action(dialogs, loc) ==
          "Cannot open the data folder: no file manager");
    fe::SaveLocation none;
    CHECK(fe::open_data_folder_action(dialogs, none) == "No data folder");
    CHECK(urls.size() == 2);
}

// --- The Nintendo Switch's Saves tab (#241, ADR 0142) -----------------------------------------

namespace {

SDL_Event pad_press(SDL_GamepadButton b, bool down) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
    e.gbutton.button = static_cast<Uint8>(b);
    e.gbutton.down = down;
    return e;
}
void press(fe::SettingsMenu &menu, SDL_GamepadButton b) {
    menu.handle(pad_press(b, true));
    menu.handle(pad_press(b, false));
}

} // namespace

TEST_CASE("save menu: on the Switch, export then import on a fresh data folder restores the "
          "saves, with the controller (#241)") {
    const TempDir from("switch-from");
    const TempDir fresh("switch-fresh");
    const fe::SaveLocation a = location(from.path);
    const Tree set = valid_set();
    write_tree(a.documents_dir, set);

    // Export: the console has no dialogs (platform_caps.h), so its route is the folder's, and the
    // zip lands in <data>/exports/ without asking.
    fe::DialogPlace place;
    place.no_dialogs = true;
    REQUIRE(fe::dialog_route(place) == fe::DialogRoute::kFolder);
    fe::FileDialogs dialogs;
    FakeDialog fake;
    fake.install(dialogs);
    const fe::SavesDialogs d{&dialogs, nullptr, fe::dialog_route(place), {}};
    const std::string name = "MapleThief-saves-" + stamp(1) + ".zip";
    CHECK(fe::export_saves_dialog(d, a, stamp(1)) == "Exported to exports/" + name);
    CHECK(fake.shown == 0);
    const fs::path zip = from.path / "exports" / name;
    REQUIRE(fs::is_regular_file(zip));

    // A fresh data folder; the player copies the zip into its import/ on the SD card.
    const fe::SaveLocation b = location(fresh.path);
    REQUIRE(tree(b.documents_dir).empty());
    std::int32_t panel = -1;
    const auto panel_fn = [&panel] { return panel; };
    const auto stamp_fn = [] { return stamp(2); };
    // Nothing there yet: where a zip goes (import/ is made).
    CHECK(fe::import_saves_list_action(b, panel_fn, stamp_fn, nullptr) ==
          "Nothing to import: put a zip of saves in the data folder's import/");
    CHECK(fs::is_directory(fresh.path / "import"));
    fs::copy_file(zip, fresh.path / "import" / name);

    // The Switch's menu, with the pad: Saves -> Import saves... -> the list -> the zip -> Yes.
    fe::Settings live;
    fe::SettingsMenu menu(fe::settings_registry(), nullptr, nullptr, nullptr);
    menu.set_platform(fe::SettingsPlatform::kSwitch);
    menu.attach(&live);
    menu.set_action("import_saves", [&] {
        return fe::import_saves_list_action(
            b, panel_fn, stamp_fn, [&menu](fe::MenuList l) { menu.open_list(std::move(l)); });
    });
    press(menu, SDL_GAMEPAD_BUTTON_GUIDE);
    REQUIRE(menu.is_open());
    for (std::size_t n = 0;
         n < menu.tabs().size() && menu.tabs()[menu.tab()] != fe::SettingGroup::kSaves; n++) {
        press(menu, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    }
    REQUIRE(menu.tabs()[menu.tab()] == fe::SettingGroup::kSaves);
    const auto on_import = [&menu] {
        const fe::SettingsMenu::Item &it = menu.items()[menu.selected()];
        return it.kind == fe::SettingsMenu::ItemKind::kAction &&
               std::strcmp(it.action->key, "import_saves") == 0;
    };
    for (std::size_t n = 0; n < menu.items().size() && !on_import(); n++) {
        press(menu, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    }
    REQUIRE(on_import());
    // With a character loaded it is refused at once, with no list.
    panel = 0;
    press(menu, SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK_FALSE(menu.in_list());
    CHECK(menu.status() == "Import from the title or the main menu (a character is loaded)");
    panel = -1;
    press(menu, SDL_GAMEPAD_BUTTON_SOUTH);
    REQUIRE(menu.in_list());
    REQUIRE(menu.list().rows.size() == 1);
    CHECK(menu.list().rows[0].label == name);
    CHECK(menu.list().rows[0].value == "import/");
    press(menu, SDL_GAMEPAD_BUTTON_SOUTH);
    REQUIRE(menu.confirming());
    CHECK(menu.help() == "Import " + name + "? The files it replaces are backed up first.");
    CHECK(tree(b.documents_dir).empty()); // No is focused: nothing yet
    press(menu, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    press(menu, SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK(menu.status() == "Imported " + std::to_string(set.size()) + " files from " + name);
    CHECK_FALSE(menu.in_list());
    // Restored byte for byte.
    CHECK(tree(b.documents_dir) == set);
}

TEST_CASE("save menu: the Switch's import list holds import/'s zips and folders and exports/' "
          "zips, newest first (#241)") {
    const TempDir tmp("switch-sources");
    const fe::SaveLocation loc = location(tmp.path);
    CHECK(fe::import_sources(loc).empty());
    write_tree(tmp.path / "import" / "folder set", valid_set());
    write_bytes(tmp.path / "import" / "old.ZIP", {'P', 'K'});
    write_bytes(tmp.path / "import" / "notes.txt", {'x'});
    write_bytes(tmp.path / "import" / ".hidden.zip", {'x'});
    write_bytes(tmp.path / "exports" / "MapleThief-saves-1.zip", {'P', 'K'});
    fs::create_directories(tmp.path / "exports" / "not a source");
    const auto now = fs::file_time_type::clock::now();
    fs::last_write_time(tmp.path / "import" / "old.ZIP", now - std::chrono::hours(48));
    fs::last_write_time(tmp.path / "exports" / "MapleThief-saves-1.zip",
                        now - std::chrono::hours(24));
    fs::last_write_time(tmp.path / "import" / "folder set", now);
    const std::vector<fe::ImportSource> sources = fe::import_sources(loc);
    REQUIRE(sources.size() == 3);
    CHECK(sources[0].name == "folder set");
    CHECK(sources[0].folder == "import");
    CHECK(sources[1].name == "MapleThief-saves-1.zip");
    CHECK(sources[1].folder == "exports");
    CHECK(sources[2].name == "old.ZIP");
    // A folder imports as a zip does; a broken zip is refused with its reason.
    const fe::MenuList list = fe::import_list(loc, sources, nullptr, [] { return stamp(3); });
    REQUIRE(list.rows.size() == 3);
    CHECK(list.question(0) == "Import folder set? The files it replaces are backed up first.");
    CHECK(list.choose(0) ==
          "Imported " + std::to_string(valid_set().size()) + " files from folder set");
    CHECK(tree(loc.documents_dir) == valid_set());
    CHECK(list.choose(2).rfind("Not imported: ", 0) == 0);
    CHECK(tree(loc.documents_dir) == valid_set());
    // No data folder.
    CHECK(fe::import_saves_list_action(fe::SaveLocation{}, nullptr, nullptr, nullptr) ==
          "No data folder: use --import-saves");
}
