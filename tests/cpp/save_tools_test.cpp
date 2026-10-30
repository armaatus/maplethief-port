// The save tools (#148, ADR 0078, save_tools.h, save_zip.h): export to a folder and a zip and
// import back byte for byte; hostile sets refused with a reason and nothing written; the
// import's backup and its roll-back after a failed write; backups that rotate at N and skip an
// unchanged save; the hook on CDataMgr::SaveQuestData through MT_FN_HOOK's entry; restore;
// reading folders and zips (Finder's __MACOSX, Documents/, duplicates); the zip reader on
// hostile input; portable mode; the save_dir and save_backups settings. The same rules on
// savedata.py's fixtures, through the program: CTests saves.check_parity and
// saves.cli_round_trip (tools/mtport/savecheck.py).
#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include "maplethief/hooks.h"
#include "maplethief/recomp/calls.h"
#include "maplethief/saves.h"
#include "maplethief/stubs.h"
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

void poke16(Bytes &b, std::size_t at, int v) {
    b[at] = static_cast<std::uint8_t>(v);
    b[at + 1] = static_cast<std::uint8_t>(v >> 8);
}

// A new character as CDataMgr::LoadSaveData 0x39989 builds it (docs/formats/save.md), with
// `seed` in the bytes the game never writes.
Bytes new_character(std::uint8_t seed = 0) {
    Bytes b(0x464, 0);
    b[2] = 1;                                   // level
    poke16(b, 0x009, 10), poke16(b, 0x00B, 10); // max HP / MP of a new character
    poke16(b, 0x00D, 10), poke16(b, 0x00F, 10); // hp, mp
    b[0x011] = 100;                             // meso
    for (std::size_t i = 0; i < 48; i++) {
        b[0x015 + 16 * i] = 0xFF; // equipment inventory: empty
        b[0x325 + 3 * i] = 0xFF;  // items: empty
    }
    for (std::size_t i = 0; i < 16; i++) {
        b[0x315 + i] = static_cast<std::uint8_t>(seed + i); // never written: heap bytes
    }
    b[0x325] = 3, b[0x326] = 5; // 5 Red Potions
    b[0x328] = 6, b[0x329] = 5; // 5 Blue Potions
    for (std::size_t i = 0; i < 5; i++) {
        b[0x3C2 + 16 * i] = 0xFF;
    }
    b[0x3C2 + 16] = 0x48, b[0x3C2 + 16 + 2] = 5;            // the Wooden Club, 5 slots
    b[0x414] = 1, b[0x415] = 3, b[0x416] = 1, b[0x417] = 6; // shortcuts: the potions
    b[0x41C] = 2, b[0x41D] = 8;                             // 28.map
    poke16(b, 0x41E, -1), poke16(b, 0x420, -1);
    b[0x423] = 2;
    const char caption[] = "How it All Began";
    std::memcpy(&b[0x424], caption, sizeof caption - 1);
    return b;
}

// The quest state of a new slot (CDataMgr::LoadQuestData 0x37D7D): every record empty.
Bytes new_quests(std::uint8_t chapter = 0) {
    Bytes b(0x12AD, 0);
    b[0] = chapter;
    for (std::size_t r = 0; r < 10; r++) {
        const std::size_t base = 0xC9 + 0x1C9 * r;
        for (std::size_t k = 0; k < 5; k++) {
            b[base + 0x143 + 14 * k] = 0xFF;
            b[base + 0x189 + 13 * k] = 0xFF;
        }
    }
    for (std::size_t r = 0; r < 10; r++) {
        b[0xC9 + 0x1C9 * r] = 0xFF; // after the rewards: each record's key_a wins the overlap
    }
    return b;
}

const Bytes kConfig = {3, 1, 0, 1, 1, 1};

// Two characters and the other files.
Tree valid_set() {
    return {{"theif0", new_character(1)},
            {"quest0", new_quests(1)},
            {"theif2", new_character(7)},
            {"quest2", new_quests(3)},
            {"config", kConfig},
            {"CTerms", Bytes(2, 0)},
            {"crestore", {1, 0, 0, 3, 0, 'a', 'b', 'c'}}};
}

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
    const fe::SaveLocation loc = fe::configure_save_location(settings, root.string(), "");
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
    fe::configure_save_location(settings, root.string(), "");
    fe::install_save_backups();
    CHECK(mt_hook_enabled(MT_HOOK_FN_SAVE_QUEST_DATA) == 0);
    save_quest_data(&cpu);
    CHECK(g_saves == 4);
    CHECK(fe::list_backups(loc).size() == 2);

    mt_hook_reset();
    mt_saves_set_documents_dir(nullptr);
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

TEST_CASE("save tools: portable mode keeps the data folder beside the executable") {
    const TempDir tmp("portable");
    const fs::path &root = tmp.path;
    CHECK(fe::portable_data_dir(root.string()).empty());
    write_bytes(root / fe::kPortableFileName, {});
    CHECK(fe::portable_data_dir(root.string()) == (root / "data").string());
    CHECK(fe::portable_data_dir("").empty());
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
