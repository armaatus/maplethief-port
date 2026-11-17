// The first-start copy and --migrate-from (#262, ADR 0127's *Migration* (b), data_copy.h) and
// the data folder's notices (data_notice.h): a synthetic old per-user folder (old and new
// layout) copied byte for byte into the new layout; a destination with data left alone, or
// with --force kept but for what the source replaces, which is backed up; ADR 0078's hostile
// save sets refused with nothing copied; the source unchanged in every case; a second start
// copying nothing; an interrupted copy's leftovers; the bundle and its executable's SHA-256;
// the notice through take_data_notices().
#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include "data_copy.h"
#include "data_location.h"
#include "data_notice.h"
#include "ipa_import.h"
#include "maplethief/saves.h"
#include "save_fixtures.h"
#include "save_tools.h"

namespace {

namespace fs = std::filesystem;
namespace fe = maplethief::frontend;
using save_fixtures::Bytes;
using save_fixtures::kConfig;
using save_fixtures::new_character;
using save_fixtures::new_quests;
using save_fixtures::Tree;
using save_fixtures::valid_set;

struct TempDir {
    fs::path path;
    TempDir()
        : path(fs::temp_directory_path() /
               ("maplethief-data-copy-" + std::to_string(std::random_device{}()) + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;
};

Bytes text(const std::string &s) { return Bytes(s.begin(), s.end()); }

Bytes read_bytes(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

void write_bytes(const fs::path &p, const Bytes &b) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char *>(b.data()), static_cast<std::streamsize>(b.size()));
}

void write_tree(const fs::path &dir, const Tree &t) {
    for (const auto &[name, bytes] : t) {
        write_bytes(dir / name, bytes);
    }
}

// Every file below `dir` ("a/b" -> bytes), and every folder (as "a/" -> empty).
Tree tree(const fs::path &dir) {
    Tree t;
    if (!fs::exists(dir)) {
        return t;
    }
    for (const auto &e : fs::recursive_directory_iterator(dir)) {
        const std::string rel = fs::relative(e.path(), dir).generic_string();
        if (e.is_regular_file()) {
            t[rel] = read_bytes(e.path());
        } else if (e.is_directory()) {
            t[rel + "/"] = {};
        }
    }
    return t;
}

// The files only.
Tree files(const Tree &t) {
    Tree f;
    for (const auto &[k, v] : t) {
        if (k.back() != '/') {
            f[k] = v;
        }
    }
    return f;
}

// `t`'s entries with every path prefixed.
Tree under(const std::string &prefix, const Tree &t) {
    Tree out;
    for (const auto &[k, v] : t) {
        out[prefix + k] = v;
    }
    return out;
}

// An old per-user folder as a build before ADR 0127 left it: Documents/, gamecenter.json at the
// top, the settings files, backups, states, screenshots, exports, import; and what isn't copied.
Tree old_folder() {
    Tree t = under("Documents/", valid_set());
    t["gamecenter.json"] = text("{\"achievements\": [1]}");
    t["settings.ini"] = text("[game]\nmaster_volume = 80\n");
    t["bindings.ini"] = text("jump = key:space\n");
    t["window.ini"] = text("x = 10\n");
    t["backups/0/20261030-120000-000/theif0"] = new_character(3);
    t["backups/import/20261030-120001-000/config"] = kConfig;
    t["states/0.state"] = Bytes(300, 7);
    t["screenshots/shot.png"] = Bytes(40, 9);
    t["exports/saves.zip"] = Bytes(10, 1);
    t["import/x"] = Bytes(3, 2);
    // Not copied:
    t["Documents/notes.txt"] = text("mine");
    t["Documents/theif0.mt-tmp"] = Bytes(5, 0);
    t["states/half.mt-tmp"] = Bytes(5, 0);
    t["logs/run.log"] = text("log");
    t["unknown.txt"] = text("?");
    return t;
}

// What the new folder holds after old_folder() is copied: the new layout.
Tree copied_folder() {
    Tree t = under("saves/", valid_set());
    t["saves/gamecenter.json"] = text("{\"achievements\": [1]}");
    t["settings.ini"] = text("[game]\nmaster_volume = 80\n");
    t["bindings.ini"] = text("jump = key:space\n");
    t["window.ini"] = text("x = 10\n");
    t["backups/0/20261030-120000-000/theif0"] = new_character(3);
    t["backups/import/20261030-120001-000/config"] = kConfig;
    t["states/0.state"] = Bytes(300, 7);
    t["screenshots/shot.png"] = Bytes(40, 9);
    t["exports/saves.zip"] = Bytes(10, 1);
    t["import/x"] = Bytes(3, 2);
    return t;
}

// The game folder <root>/kit: the location game, <kit>/data made by the probe (empty).
fe::DataLocation kit_location(const fs::path &root) {
    const fs::path kit = root / "kit";
    fs::create_directories(kit / "data");
    return fe::data_location_at((kit / "data").string(), fe::DataPlace::kGame,
                                fe::DataReason::kDefault, kit.string());
}

// The siblings of `dir` whose names start with its own and `suffix` (".tmp-", ".old-").
std::vector<std::string> leftovers(const fs::path &dir, const std::string &suffix) {
    std::vector<std::string> out;
    const std::string prefix = dir.filename().string() + suffix;
    for (const auto &e : fs::directory_iterator(dir.parent_path())) {
        if (e.path().filename().string().rfind(prefix, 0) == 0) {
            out.push_back(e.path().filename().string());
        }
    }
    return out;
}

} // namespace

TEST_CASE("data copy: an old per-user folder is copied byte for byte into the new layout on the "
          "first start, with a notice; a second start copies nothing") {
    const TempDir tmp;
    const fs::path old = tmp.path / "Application Support" / "MapleThief";
    write_tree(old, old_folder());
    const Tree old_before = tree(old);
    const fe::DataLocation loc = kit_location(tmp.path);
    fe::take_data_notices(); // nothing from before

    fe::DataCopyResult r;
    REQUIRE(fe::first_start_copy(loc, old.string(), &r));
    CHECK_MESSAGE(r.ok, r.error);
    CHECK(files(tree(loc.dir)) == copied_folder());
    CHECK(tree(old) == old_before); // the old folder as it was
    CHECK(r.replaced.empty());
    CHECK(r.backup.empty());
    CHECK(r.copied.size() == copied_folder().size());
    const std::vector<std::string> skipped = r.skipped;
    const auto skips = [&skipped](const std::string &what) {
        for (const std::string &s : skipped) {
            if (s.rfind(what + ":", 0) == 0) {
                return true;
            }
        }
        return false;
    };
    CHECK(skips("Documents/notes.txt"));
    CHECK(skips("Documents/theif0.mt-tmp"));
    CHECK(skips("states/half.mt-tmp"));
    CHECK(skips("logs"));
    CHECK(skips("unknown.txt"));
    // The notice, once, through the seam.
    const std::vector<std::string> notices = fe::take_data_notices();
    REQUIRE(notices.size() == 1);
    CHECK(notices[0] == fe::first_start_copied_text(old.string(), loc.dir));
    CHECK(notices[0].find("The old folder was left as it is") != std::string::npos);
    CHECK(fe::take_data_notices().empty());
    // No staging left, no bundle (the old folder has none).
    CHECK(leftovers(loc.dir, ".tmp-").empty());
    CHECK(leftovers(loc.dir, ".old-").empty());
    CHECK_FALSE(fs::exists(loc.bundle_dir));

    // The second start: the new folder has data now.
    const Tree after = tree(loc.dir);
    CHECK_FALSE(fe::first_start_copy(loc, old.string(), &r));
    CHECK(tree(loc.dir) == after);
    CHECK(tree(old) == old_before);
    CHECK(fe::take_data_notices().empty());
}

TEST_CASE("data copy: a source in the new layout; the conditions of the first-start copy") {
    const TempDir tmp;
    const fs::path src = tmp.path / "backup" / "data";
    Tree t = copied_folder(); // already the new layout
    write_tree(src, t);
    const Tree src_before = tree(src);
    const fe::DataLocation loc = kit_location(tmp.path);
    fe::take_data_notices();

    // Not the game folder (a user folder, MAPLETHIEF_DATA_DIR): never.
    for (const fe::DataPlace place : {fe::DataPlace::kUser, fe::DataPlace::kEnv}) {
        fe::DataLocation other = loc;
        other.place = place;
        CHECK_FALSE(fe::first_start_copy(other, src.string()));
    }
    // No old folder, the same folder, an old folder without saves or settings.ini.
    CHECK_FALSE(fe::first_start_copy(loc, ""));
    CHECK_FALSE(fe::first_start_copy(loc, loc.dir));
    const fs::path bare = tmp.path / "bare";
    write_bytes(bare / "bindings.ini", text("x"));
    write_bytes(bare / "saves" / "half.mt-tmp", text("x"));
    CHECK_FALSE(fe::data_dir_has_player_data(bare));
    CHECK_FALSE(fe::first_start_copy(loc, bare.string()));
    CHECK(tree(loc.dir).empty());
    CHECK(fe::take_data_notices().empty());

    fe::DataCopyResult r;
    REQUIRE(fe::first_start_copy(loc, src.string(), &r));
    CHECK_MESSAGE(r.ok, r.error);
    CHECK(files(tree(loc.dir)) == t);
    CHECK(tree(src) == src_before);
    CHECK(fe::take_data_notices().size() == 1);
}

TEST_CASE("data copy: a destination with data is left alone; --force keeps it but for what the "
          "source replaces, which is backed up") {
    const TempDir tmp;
    const fs::path src = tmp.path / "old";
    write_tree(src, {{"Documents/theif0", new_character(1)},
                     {"Documents/quest0", new_quests(1)},
                     {"settings.ini", text("new = 1\n")}});
    const Tree src_before = tree(src);
    const fe::DataLocation loc = kit_location(tmp.path);
    const fs::path dst = loc.dir;
    const std::string counters = "# MapleThief: the cash shop's counters\nlevel = 12\ndeaths = 3\n";
    const Tree mine = {{"saves/theif0", new_character(9)},
                       {"saves/quest0", new_quests(9)},
                       {"saves/theif1", new_character(4)},
                       {"saves/quest1", new_quests(4)},
                       {"saves/maplethief-cashshop0", text(counters)},
                       {"settings.ini", text("mine = 1\n")},
                       {"states/keep.state", Bytes(4, 4)}};
    write_tree(dst, mine);
    const Tree dst_before = tree(dst);
    fe::take_data_notices();

    // The first start: not run. Without --force: refused, nothing written.
    CHECK_FALSE(fe::first_start_copy(loc, src.string()));
    fe::DataCopyResult r = fe::copy_player_data(src, dst, loc.bundle_dir, false);
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("already has data") != std::string::npos);
    CHECK(tree(dst) == dst_before);
    CHECK(tree(src) == src_before);
    // settings.ini alone counts as data too.
    const fs::path settings_only = tmp.path / "settings-only";
    write_bytes(settings_only / "settings.ini", text("x"));
    CHECK(fe::data_dir_has_player_data(settings_only));
    CHECK_FALSE(fe::copy_player_data(src, settings_only, "", false).ok);

    // --force: the source's files win, the replaced ones are in backups/import/<time>/, the
    // replaced character's counters go with it, the rest stays.
    r = fe::copy_player_data(src, dst, loc.bundle_dir, true);
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK(tree(src) == src_before);
    const Tree got = files(tree(dst));
    CHECK(got.at("saves/theif0") == new_character(1));
    CHECK(got.at("saves/quest0") == new_quests(1));
    CHECK(got.at("settings.ini") == text("new = 1\n"));
    CHECK(got.at("saves/theif1") == new_character(4));
    CHECK(got.at("saves/quest1") == new_quests(4));
    CHECK(got.at("states/keep.state") == Bytes(4, 4));
    CHECK(got.count("saves/maplethief-cashshop0") == 0);
    REQUIRE_FALSE(r.backup.empty());
    const fs::path backup(r.backup);
    CHECK(backup.parent_path() == fs::path(dst) / "backups" / "import");
    CHECK(files(tree(backup)) == Tree{{"saves/theif0", new_character(9)},
                                      {"saves/quest0", new_quests(9)},
                                      {"saves/maplethief-cashshop0", text(counters)},
                                      {"settings.ini", text("mine = 1\n")}});
    CHECK(r.replaced.size() == 4);
    // Every file is either the source's or kept or backed up: nothing lost.
    CHECK(got.size() == 6 + 4);
    // The same bytes again: nothing replaced, no backup.
    r = fe::copy_player_data(src, dst, loc.bundle_dir, true);
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK(r.replaced.empty());
    CHECK(r.backup.empty());
    CHECK(leftovers(dst, ".tmp-").empty());
    CHECK(leftovers(dst, ".old-").empty());
}

TEST_CASE("data copy: ADR 0078's hostile save sets are refused with nothing copied, retried on "
          "the next start") {
    const TempDir tmp;
    struct Case {
        const char *name;
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
    cases.push_back({"truncated", "bad_size", with("theif0", cut)});
    Bytes big = new_quests();
    big.push_back(0);
    cases.push_back({"oversized", "bad_size", with("quest0", big)});
    cases.push_back({"oversized receipts", "bad_size",
                     with("crestore", Bytes(MT_SAVES_MAX_RECEIPTS_BYTES + 1, 0))});
    Bytes id = new_character();
    id[0x015 + 16 * 3] = MT_SAVES_EQUIP_IDS;
    cases.push_back({"equipment id", "bad_item", with("theif0", id)});
    Bytes etc = new_character();
    etc[0x325 + 3 * 2] = 200;
    cases.push_back({"item id", "bad_item", with("theif0", etc)});
    cases.push_back({"speed byte 3", "bad_config", with("config", {3, 3, 0, 1, 1, 1})});
    Bytes map = new_character();
    map[0x41C] = 11;
    cases.push_back({"area", "bad_map", with("theif0", map)});
    Tree pair = valid_set();
    pair.erase("quest2");
    cases.push_back({"missing quest", "bad_pair", pair});

    int n = 0;
    for (const Case &c : cases) {
        CAPTURE(c.name);
        const fs::path root = tmp.path / std::to_string(n++);
        const fs::path old = root / "old";
        Tree t = old_folder();
        for (auto it = t.begin(); it != t.end();) {
            it = it->first.rfind("Documents/", 0) == 0 ? t.erase(it) : std::next(it);
        }
        for (const auto &[name, bytes] : c.set) {
            t["Documents/" + name] = bytes;
        }
        write_tree(old, t);
        const Tree old_before = tree(old);
        const fe::DataLocation loc = kit_location(root);
        fe::take_data_notices();
        for (int start = 0; start < 2; start++) { // tried again on the next start
            fe::DataCopyResult r;
            REQUIRE(fe::first_start_copy(loc, old.string(), &r));
            CHECK_FALSE(r.ok);
            CHECK(r.error.find(c.code) != std::string::npos);
            CHECK(r.copied.empty());
            CHECK(tree(loc.dir).empty()); // nothing copied, not even settings.ini
            CHECK(tree(old) == old_before);
            CHECK(leftovers(loc.dir, ".tmp-").empty());
            const std::vector<std::string> notices = fe::take_data_notices();
            REQUIRE(notices.size() == 1);
            CHECK(notices[0].find("were not copied") != std::string::npos);
            CHECK(notices[0].find(c.code) != std::string::npos);
        }
        // --migrate-from refuses it the same way.
        const fe::DataCopyResult m = fe::copy_player_data(old, loc.dir, "", true);
        CHECK_FALSE(m.ok);
        CHECK(tree(loc.dir).empty());
    }
#if !defined(_WIN32)
    // A game file that is a link (to a file outside): refused too.
    const fs::path root = tmp.path / "link";
    const fs::path old = root / "old";
    write_tree(old, under("Documents/", valid_set()));
    write_bytes(root / "elsewhere", new_character(2));
    fs::remove(old / "Documents" / "theif0");
    fs::create_symlink(root / "elsewhere", old / "Documents" / "theif0");
    const fe::DataLocation loc = kit_location(root);
    fe::DataCopyResult r;
    REQUIRE(fe::first_start_copy(loc, old.string(), &r));
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("not a regular file") != std::string::npos);
    CHECK(tree(loc.dir).empty());
    CHECK(fs::is_symlink(old / "Documents" / "theif0"));
    fe::take_data_notices();
#endif
}

TEST_CASE("data copy: an interrupted copy's leftovers are cleaned up, or put back") {
    const TempDir tmp;
    const fs::path src = tmp.path / "old";
    write_tree(src, under("Documents/", valid_set()));
    const fe::DataLocation loc = kit_location(tmp.path);
    const fs::path dst = loc.dir;
    // A staging folder and a folder set aside, with <dst> there: both removed.
    write_bytes(fs::path(dst.string() + ".tmp-1a-0") / "saves" / "theif0", Bytes(3, 3));
    write_bytes(fs::path(dst.string() + ".old-1b-0") / "x", Bytes(1, 1));
    fe::DataCopyResult r = fe::copy_player_data(src, dst, "", false);
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK(leftovers(dst, ".tmp-").empty());
    CHECK(leftovers(dst, ".old-").empty());
    CHECK(files(tree(dst)) == under("saves/", valid_set()));

    // Interrupted between the two renames: <dst> is missing and its old folder is put back
    // (here it has data, so the copy is then refused).
    fs::rename(dst, fs::path(dst.string() + ".old-2c-0"));
    write_bytes(fs::path(dst.string() + ".tmp-2d-0") / "half", Bytes(1, 1));
    r = fe::copy_player_data(src, dst, "", false);
    CHECK_FALSE(r.ok);
    CHECK(files(tree(dst)) == under("saves/", valid_set()));
    CHECK(leftovers(dst, ".tmp-").empty());
    CHECK(leftovers(dst, ".old-").empty());
}

TEST_CASE("data copy: the bundle is copied when the destination has none and its executable is "
          "1.5's") {
    const TempDir tmp;
    const fs::path src = tmp.path / "old";
    write_tree(src, under("Documents/", valid_set()));
    const Bytes exe = text("the executable");
    const Tree bundle = {
        {"MapleThief", exe}, {"Info.plist", text("plist")}, {"Assets/a.png", Bytes(20, 5)}};
    write_tree(src / "game", bundle);
    const Tree src_before = tree(src);
    const fe::Sha256 sha = fe::sha256_of(exe);
    fe::Sha256 other = sha;
    other[0] ^= 1;

    SUBCASE("copied, staged as game.tmp-*, beside data/") {
        const fe::DataLocation loc = kit_location(tmp.path);
        write_bytes(fs::path(loc.base) / "game.tmp-old" / "x", Bytes(1, 1)); // a leftover
        const fe::DataCopyResult r = fe::copy_player_data(src, loc.dir, loc.bundle_dir, false, sha);
        REQUIRE_MESSAGE(r.ok, r.error);
        CHECK(files(tree(loc.bundle_dir)) == bundle);
        CHECK(std::find(r.copied.begin(), r.copied.end(), "game/") != r.copied.end());
        CHECK(fe::has_imported_game(loc.base));
        CHECK(leftovers(loc.bundle_dir, ".tmp-").empty());
        CHECK(tree(src) == src_before);
        CHECK(tree(loc.dir).count("game/") == 0); // not inside data/
    }
    SUBCASE("another executable: not copied, the data still is") {
        const fe::DataLocation loc = kit_location(tmp.path);
        const fe::DataCopyResult r =
            fe::copy_player_data(src, loc.dir, loc.bundle_dir, false, other);
        REQUIRE_MESSAGE(r.ok, r.error);
        CHECK_FALSE(fs::exists(loc.bundle_dir));
        CHECK(leftovers(loc.bundle_dir, ".tmp-").empty());
        bool said = false;
        for (const std::string &s : r.skipped) {
            said = said || s.find("not version 1.5's") != std::string::npos;
        }
        CHECK(said);
        CHECK(files(tree(loc.dir)) == under("saves/", valid_set()));
    }
    SUBCASE("a bundle there already: left alone") {
        const fe::DataLocation loc = kit_location(tmp.path);
        write_bytes(fs::path(loc.bundle_dir) / "MapleThief", text("mine"));
        const fe::DataCopyResult r = fe::copy_player_data(src, loc.dir, loc.bundle_dir, false, sha);
        REQUIRE_MESSAGE(r.ok, r.error);
        CHECK(files(tree(loc.bundle_dir)) == Tree{{"MapleThief", text("mine")}});
    }
    SUBCASE("an old kit's data/: the game/ beside it") {
        const fs::path kit_data = tmp.path / "old-kit" / "data";
        write_tree(kit_data, under("saves/", valid_set()));
        write_tree(tmp.path / "old-kit" / "game", bundle);
        const fe::DataLocation loc = kit_location(tmp.path);
        const fe::DataCopyResult r =
            fe::copy_player_data(kit_data, loc.dir, loc.bundle_dir, false, sha);
        REQUIRE_MESSAGE(r.ok, r.error);
        CHECK(files(tree(loc.bundle_dir)) == bundle);
    }
}

TEST_CASE("data copy: what isn't a source") {
    const TempDir tmp;
    const fe::DataLocation loc = kit_location(tmp.path);
    CHECK_FALSE(fe::copy_player_data(tmp.path / "missing", loc.dir, "", false).ok);
    write_bytes(tmp.path / "file", text("x"));
    CHECK_FALSE(fe::copy_player_data(tmp.path / "file", loc.dir, "", false).ok);
    fs::create_directories(tmp.path / "empty");
    const fe::DataCopyResult r = fe::copy_player_data(tmp.path / "empty", loc.dir, "", false);
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("no saves") != std::string::npos);
    write_tree(loc.dir, under("saves/", valid_set()));
    CHECK_FALSE(fe::copy_player_data(loc.dir, loc.dir, "", true).ok); // itself
    CHECK(tree(tmp.path / "empty").empty());
}

TEST_CASE("data notice: queued, taken once, wrapped for the window") {
    fe::take_data_notices();
    fe::queue_data_notice("one");
    fe::queue_data_notice("");
    fe::queue_data_notice("two");
    CHECK(fe::take_data_notices() == std::vector<std::string>{"one", "two"});
    CHECK(fe::take_data_notices().empty());

    CHECK(fe::wrap_notice("", 10).empty());
    CHECK(fe::wrap_notice("a bb ccc dddd", 6) == std::vector<std::string>{"a bb", "ccc", "dddd"});
    CHECK(fe::wrap_notice("  spaced   out  ", 20) == std::vector<std::string>{"spaced out"});
    // A long path breaks after a separator.
    CHECK(fe::wrap_notice("from /Users/x/Library/MapleThief to", 14) ==
          std::vector<std::string>{"from", "/Users/x/", "Library/", "MapleThief to"});
    CHECK(fe::wrap_notice("abcdefghij", 4) == std::vector<std::string>{"abcd", "efgh", "ij"});
    for (const std::string &line :
         fe::wrap_notice(fe::first_start_copied_text(
                             "/Users/someone/Library/Application Support/MapleThief",
                             "/Users/someone/Games/MapleThief-1.0-macos-universal/data"),
                         fe::kDataNoticeColumns)) {
        CHECK(line.size() <= fe::kDataNoticeColumns);
    }
}

TEST_CASE("data notice: a save refused as the game read it is shown once, with where it went "
          "(#413)") {
    fe::take_data_notices();
    fe::save_refused_hook("theif1", "theif1's level is 0; a level is 1 to 100",
                          "/data/backups/refused/20261113-120000-000", nullptr);
    fe::save_refused_hook("quest2", "quest2 has 11 bytes", "", nullptr);
    const std::vector<std::string> notices = fe::take_data_notices();
    REQUIRE(notices.size() == 2u);
    CHECK(notices[0] == "The save theif1 was not loaded: theif1's level is 0; a level is 1 to 100. "
                        "It was moved to /data/backups/refused/20261113-120000-000.");
    CHECK(notices[1].find("could not be moved aside") != std::string::npos);
    CHECK(fe::take_data_notices().empty());
}
