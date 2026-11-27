// An iPhone backup read by the game (#374, ADR 0078's addendum, ios_backup.h, sqlite_read.h):
// SHA-1 and the backup's file IDs; the SQLite reader on databases this file writes (several
// leaves under an interior page, a record with an overflow chain) and on damaged ones; the
// game's files out of synthetic Manifest.db and Manifest.mbdb backups; damaged, encrypted and
// hostile backups refused with their reason and nothing changed; the Saves tab's iPhone backup,
// Check saves and Saves from a folder rows, driven with the controller only. Python-made
// backups go through the program in CTest saves.ios_backup_import (tools/mtport/iosbackup.py).
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

#include "data_location.h"
#include "ios_backup.h"
#include "save_fixtures.h"
#include "save_menu.h"
#include "save_tools.h"
#include "settings.h"
#include "settings_menu.h"
#include "sqlite_read.h"

namespace {

namespace fs = std::filesystem;
namespace fe = maplethief::frontend;
using save_fixtures::Bytes;
using save_fixtures::Tree;
using save_fixtures::valid_set;

constexpr const char *kApp = "AppDomain-com.nexonmobile.maplestory.thief";

fs::path unique_dir(const char *what) {
    const fs::path dir =
        fs::temp_directory_path() /
        (std::string("maplethief-ios-backup-") + what + "-" +
         std::to_string(std::random_device{}()) + "-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    return dir;
}

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

void write_text(const fs::path &p, const std::string &s) {
    write_bytes(p, Bytes(s.begin(), s.end()));
}

Tree tree(const fs::path &dir) {
    Tree t;
    std::error_code ec;
    if (!fs::exists(dir, ec)) {
        return t;
    }
    for (const auto &e : fs::recursive_directory_iterator(dir)) {
        if (e.is_regular_file()) {
            t[fs::relative(e.path(), dir).generic_string()] = read_bytes(e.path());
        }
    }
    return t;
}

fe::SaveLocation location(const fs::path &root) {
    fe::SaveLocation loc;
    loc.data_dir = root.string();
    loc.documents_dir = (root / "saves").string();
    loc.backups_dir = (root / "backups").string();
    fs::create_directories(loc.documents_dir);
    return loc;
}

std::string stamp(int n) {
    char s[32];
    std::snprintf(s, sizeof s, "20261127-1200%02d-000", n);
    return s;
}

// --- A small SQLite writer: rowid tables on 512-byte pages ---------------------------------------

using Value = std::variant<std::monostate, std::int64_t, std::string, Bytes>;
constexpr std::size_t kPage = 512;

void put_varint(Bytes &out, std::uint64_t v) {
    if (v > 0x00FFFFFFFFFFFFFFull) {
        std::uint8_t b[9];
        b[8] = static_cast<std::uint8_t>(v);
        v >>= 8;
        for (int i = 7; i >= 0; i--) {
            b[i] = static_cast<std::uint8_t>((v & 0x7F) | 0x80);
            v >>= 7;
        }
        out.insert(out.end(), b, b + 9);
        return;
    }
    std::uint8_t b[9];
    int n = 0;
    do {
        b[n++] = static_cast<std::uint8_t>(v & 0x7F);
        v >>= 7;
    } while (v != 0);
    for (int i = n - 1; i >= 0; i--) {
        out.push_back(static_cast<std::uint8_t>(b[i] | (i > 0 ? 0x80 : 0)));
    }
}

void put_be(Bytes &out, std::size_t at, std::uint64_t v, int n) {
    for (int i = n - 1; i >= 0; i--) {
        out[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v);
        v >>= 8;
    }
}

Bytes record(const std::vector<Value> &values) {
    Bytes types, body;
    for (const Value &v : values) {
        if (std::holds_alternative<std::monostate>(v)) {
            put_varint(types, 0);
        } else if (const auto *i = std::get_if<std::int64_t>(&v)) {
            if (*i == 0 || *i == 1) {
                put_varint(types, *i == 0 ? 8 : 9);
            } else if (*i >= -128 && *i < 128) {
                put_varint(types, 1);
                body.push_back(static_cast<std::uint8_t>(*i));
            } else {
                put_varint(types, 6);
                const std::size_t at = body.size();
                body.resize(at + 8);
                put_be(body, at, static_cast<std::uint64_t>(*i), 8);
            }
        } else if (const auto *s = std::get_if<std::string>(&v)) {
            put_varint(types, 13 + 2 * s->size());
            body.insert(body.end(), s->begin(), s->end());
        } else {
            const Bytes &b = std::get<Bytes>(v);
            put_varint(types, 12 + 2 * b.size());
            body.insert(body.end(), b.begin(), b.end());
        }
    }
    Bytes out;
    put_varint(out, types.size() + 1); // the header's size varint is one byte here
    out.insert(out.end(), types.begin(), types.end());
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

// A database holding `rows` in the table `table` (created by `sql`); page 2 is that table's root.
// More rows than a leaf holds go into leaves under an interior root; long payloads overflow.
Bytes sqlite_db(const std::string &table, const std::string &sql,
                const std::vector<std::vector<Value>> &rows) {
    constexpr std::size_t u = kPage;
    struct Cell {
        Bytes head;  // payload size and rowid
        Bytes local; // the payload's local part
        Bytes rest;  // what overflows
        std::int64_t rowid = 0;
        std::uint32_t first_overflow = 0;
        std::size_t size() const { return head.size() + local.size() + (rest.empty() ? 0 : 4); }
    };
    const auto make_cell = [](std::int64_t rowid, const Bytes &payload) {
        Cell c;
        c.rowid = rowid;
        put_varint(c.head, payload.size());
        put_varint(c.head, static_cast<std::uint64_t>(rowid));
        const std::size_t x = u - 35, m = ((u - 12) * 32 / 255) - 23;
        std::size_t local = payload.size();
        if (payload.size() > x) {
            const std::size_t k = m + ((payload.size() - m) % (u - 4));
            local = k <= x ? k : m;
        }
        c.local.assign(payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(local));
        c.rest.assign(payload.begin() + static_cast<std::ptrdiff_t>(local), payload.end());
        return c;
    };
    std::vector<Cell> cells;
    for (std::size_t i = 0; i < rows.size(); i++) {
        cells.push_back(make_cell(static_cast<std::int64_t>(i + 1), record(rows[i])));
    }
    // Leaves, greedily.
    std::vector<std::vector<std::size_t>> leaves(1);
    std::size_t used = 8;
    for (std::size_t i = 0; i < cells.size(); i++) {
        if (used + 2 + cells[i].size() > u) {
            leaves.emplace_back();
            used = 8;
        }
        leaves.back().push_back(i);
        used += 2 + cells[i].size();
    }
    const bool interior = leaves.size() > 1;
    const std::uint32_t first_leaf = interior ? 3 : 2;
    std::uint32_t next = first_leaf + static_cast<std::uint32_t>(leaves.size());
    std::vector<Bytes> overflow_pages;
    for (Cell &c : cells) {
        if (c.rest.empty()) {
            continue;
        }
        c.first_overflow = next;
        for (std::size_t at = 0; at < c.rest.size(); at += u - 4) {
            Bytes pg(u, 0);
            const std::size_t n = std::min(u - 4, c.rest.size() - at);
            std::memcpy(pg.data() + 4, c.rest.data() + at, n);
            next++;
            put_be(pg, 0, at + n < c.rest.size() ? next : 0, 4);
            overflow_pages.push_back(std::move(pg));
        }
    }
    const auto leaf_page = [&](const std::vector<std::size_t> &which, std::size_t h) {
        Bytes pg(u, 0);
        pg[h] = 13;
        put_be(pg, h + 3, which.size(), 2);
        std::size_t end = u;
        for (std::size_t k = 0; k < which.size(); k++) {
            const Cell &c = cells[which[k]];
            end -= c.size();
            Bytes bytes = c.head;
            bytes.insert(bytes.end(), c.local.begin(), c.local.end());
            if (!c.rest.empty()) {
                const std::size_t at = bytes.size();
                bytes.resize(at + 4);
                put_be(bytes, at, c.first_overflow, 4);
            }
            std::memcpy(pg.data() + end, bytes.data(), bytes.size());
            put_be(pg, h + 8 + 2 * k, end, 2);
        }
        put_be(pg, h + 5, end, 2);
        return pg;
    };
    std::vector<Bytes> pages;
    // Page 1: the header and the schema, one row.
    {
        std::vector<Cell> keep;
        keep.swap(cells);
        cells.push_back(
            make_cell(1, record({std::string("table"), table, table, std::int64_t{2}, sql})));
        Bytes pg = leaf_page({0}, 100);
        keep.swap(cells);
        std::memcpy(pg.data(), "SQLite format 3", 16);
        put_be(pg, 16, u, 2);
        pg[18] = pg[19] = 1;
        pg[21] = 64, pg[22] = 32, pg[23] = 32;
        put_be(pg, 44, 4, 4);
        put_be(pg, 56, 1, 4);
        pages.push_back(std::move(pg));
    }
    if (interior) {
        Bytes pg(u, 0);
        pg[0] = 5;
        put_be(pg, 3, leaves.size() - 1, 2);
        std::size_t end = u;
        for (std::size_t k = 0; k + 1 < leaves.size(); k++) {
            Bytes c(4, 0);
            put_be(c, 0, first_leaf + k, 4);
            put_varint(c, static_cast<std::uint64_t>(cells[leaves[k].back()].rowid));
            end -= c.size();
            std::memcpy(pg.data() + end, c.data(), c.size());
            put_be(pg, 12 + 2 * k, end, 2);
        }
        put_be(pg, 5, end, 2);
        put_be(pg, 8, first_leaf + leaves.size() - 1, 4);
        pages.push_back(std::move(pg));
    }
    for (const auto &which : leaves) {
        pages.push_back(leaf_page(which, 0));
    }
    for (Bytes &pg : overflow_pages) {
        pages.push_back(std::move(pg));
    }
    put_be(pages[0], 28, pages.size(), 4);
    Bytes out;
    for (const Bytes &pg : pages) {
        out.insert(out.end(), pg.begin(), pg.end());
    }
    return out;
}

const std::string kFilesSql = "CREATE TABLE Files (fileID TEXT PRIMARY KEY, domain TEXT, "
                              "relativePath TEXT, flags INTEGER, file BLOB)";

// --- Synthetic backups ---------------------------------------------------------------------------

struct Entry {
    std::string domain;
    std::string path;
    std::optional<Bytes> data; // none: a folder
    bool link = false;
};

std::vector<Entry> entries_for(const Tree &documents) {
    std::vector<Entry> out = {
        {"HomeDomain", "Library/Preferences", std::nullopt},
        {"HomeDomain", "Library/Preferences/com.apple.springboard.plist", Bytes{'<', 'x', '>'}},
        {"AppDomain-com.example.other", "Documents/theif0", Bytes{'n', 'o'}},
        {kApp, "", std::nullopt},
        {kApp, "Documents", std::nullopt},
        {kApp, "Library/Preferences/com.nexonmobile.maplestory.thief.plist", Bytes{'c'}},
    };
    for (const auto &[name, bytes] : documents) {
        out.push_back({kApp, "Documents/" + name, bytes});
    }
    return out;
}

std::string xml_plist(const std::string &body) {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\">\n<dict>\n" + body +
           "</dict>\n</plist>\n";
}

void write_plists(const fs::path &dir, bool encrypted, const std::string &device) {
    write_text(dir / "Manifest.plist", xml_plist(std::string("  <key>IsEncrypted</key>\n  ") +
                                                 (encrypted ? "<true/>" : "<false/>") +
                                                 "\n  <key>Version</key>\n  "
                                                 "<string>10.0</string>\n"));
    write_text(dir / "Info.plist",
               xml_plist("  <key>Device Name</key>\n  <string>" + device +
                         "</string>\n  <key>Product Version</key>\n  <string>12.5.7</string>\n"));
    write_text(dir / "Status.plist",
               xml_plist("  <key>SnapshotState</key>\n  <string>finished</string>\n"));
}

// A Finder backup (Manifest.db), with `extra` rows of other domains (to make the tree deep).
void write_db_backup(const fs::path &dir, const std::vector<Entry> &entries,
                     const std::string &device = "Test iPod", std::size_t extra = 0) {
    write_plists(dir, false, device);
    std::vector<std::vector<Value>> rows;
    for (std::size_t i = 0; i < extra; i++) {
        const std::string path = "Media/DCIM/100APPLE/IMG_" + std::to_string(i) + ".JPG";
        rows.push_back({fe::ios_file_id("CameraRollDomain", path), std::string("CameraRollDomain"),
                        path, std::int64_t{1}, Bytes(i % 7 == 0 ? 700 : 40, 0x5A)});
    }
    for (const Entry &e : entries) {
        const std::string id = fe::ios_file_id(e.domain, e.path);
        const std::int64_t flags = e.link ? 4 : e.data ? 1 : 2;
        rows.push_back({id, e.domain, e.path, flags, Bytes(60, 0x11)});
        if (e.data && !e.link) {
            write_bytes(dir / id.substr(0, 2) / id, *e.data);
        }
    }
    write_bytes(dir / "Manifest.db", sqlite_db("Files", kFilesSql, rows));
}

void put_str(Bytes &out, const std::string &s, bool none_if_empty = true) {
    if (s.empty() && none_if_empty) {
        out.push_back(0xFF), out.push_back(0xFF);
        return;
    }
    out.push_back(static_cast<std::uint8_t>(s.size() >> 8));
    out.push_back(static_cast<std::uint8_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}

// An iTunes backup (Manifest.mbdb). `key` gives the game's files an encryption key; `size_off`
// adds to the sizes recorded.
void write_mbdb_backup(const fs::path &dir, const std::vector<Entry> &entries, bool key = false,
                       int size_off = 0) {
    write_plists(dir, false, "Old iPhone");
    Bytes m = {'m', 'b', 'd', 'b', 5, 0};
    for (const Entry &e : entries) {
        put_str(m, e.domain);
        put_str(m, e.path);
        put_str(m, e.link ? "/var/mobile/x" : "");
        put_str(m, e.data ? std::string(20, 'h') : "");
        put_str(m, key && e.data ? std::string(40, 'k') : "");
        const std::uint16_t mode = e.link ? 0xA1ED : e.data ? 0x81A4 : 0x41ED;
        m.push_back(static_cast<std::uint8_t>(mode >> 8)),
            m.push_back(static_cast<std::uint8_t>(mode));
        m.insert(m.end(), 8 + 4 + 4 + 12, 0); // inode, uid, gid, times
        const std::uint64_t size = e.data ? e.data->size() + static_cast<std::size_t>(size_off) : 0;
        for (int i = 7; i >= 0; i--) {
            m.push_back(static_cast<std::uint8_t>(size >> (8 * i)));
        }
        m.push_back(3); // protection class
        m.push_back(1); // one property
        put_str(m, "com.apple.attr");
        put_str(m, "v", false);
        if (e.data && !e.link) {
            write_bytes(dir / fe::ios_file_id(e.domain, e.path), *e.data);
        }
    }
    write_bytes(dir / "Manifest.mbdb", m);
}

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

// The pad moves the menu to the Saves tab and onto the action row `key`.
void pad_to_action(fe::SettingsMenu &menu, const char *key) {
    for (std::size_t n = 0;
         n < menu.tabs().size() && menu.tabs()[menu.tab()] != fe::SettingGroup::kSaves; n++) {
        press(menu, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    }
    REQUIRE(menu.tabs()[menu.tab()] == fe::SettingGroup::kSaves);
    const auto on = [&menu, key] {
        const fe::SettingsMenu::Item &it = menu.items()[menu.selected()];
        return it.kind == fe::SettingsMenu::ItemKind::kAction &&
               std::strcmp(it.action->key, key) == 0;
    };
    for (std::size_t n = 0; n < menu.items().size() && !on(); n++) {
        press(menu, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    }
    REQUIRE(on());
}

} // namespace

TEST_CASE("ios backup: SHA-1 and a backup's file IDs") {
    const std::string abc = "abc";
    const auto d = fe::sha1_of(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t *>(abc.data()), abc.size()));
    CHECK(d[0] == 0xA9);
    CHECK(d[1] == 0x99);
    CHECK(d[19] == 0x9D);
    // docs/formats/ios-backup.md's table.
    CHECK(fe::ios_file_id(kApp, "Documents/theif0") == "cae5c77bdc2c044c07abbbe215c128fd70720274");
    CHECK(fe::ios_file_id(kApp, "Documents/quest0") == "45a129dc684a02611475f7c541e2038958826ced");
    CHECK(fe::ios_file_id(kApp, "Documents/config") == "597f7576d892c5eb53ae7becab90b5cf08f96b1f");
}

TEST_CASE("sqlite: records, CREATE TABLE columns, and a table over several pages") {
    fe::SqliteRow row;
    REQUIRE(fe::sqlite_record(record({std::monostate{}, std::int64_t{-5}, std::int64_t{1},
                                      std::int64_t{1} << 40, std::string("hé"), Bytes{1, 2}}),
                              &row));
    REQUIRE(row.size() == 6);
    CHECK(row[0].type == fe::SqliteValue::Type::kNull);
    CHECK(row[1].integer == -5);
    CHECK(row[2].integer == 1);
    CHECK(row[3].integer == (std::int64_t{1} << 40));
    CHECK(row[4].is_text());
    CHECK(row[4].bytes == "hé");
    CHECK(row[5].type == fe::SqliteValue::Type::kBlob);
    CHECK(row[5].bytes == std::string("\x01\x02", 2));
    CHECK_FALSE(fe::sqlite_record(Bytes{5, 13}, &row));   // the header runs past the record
    CHECK_FALSE(fe::sqlite_record(Bytes{2, 0x21}, &row)); // a text past the record

    CHECK(fe::sqlite_create_columns(kFilesSql) ==
          std::vector<std::string>{"fileID", "domain", "relativePath", "flags", "file"});
    CHECK(fe::sqlite_create_columns("CREATE TABLE \"T\" (\"a b\" TEXT, [c] INT DEFAULT (1, 2), "
                                    "d, PRIMARY KEY (a, d), CONSTRAINT x UNIQUE (d))") ==
          std::vector<std::string>{"a b", "c", "d"});
    CHECK(fe::sqlite_create_columns("not sql").empty());

    // 300 rows, some with an overflow chain: an interior root over many leaves.
    std::vector<std::vector<Value>> rows;
    for (int i = 0; i < 300; i++) {
        rows.push_back({std::string("row ") + std::to_string(i), std::int64_t{i},
                        Bytes(static_cast<std::size_t>(i % 50 == 0 ? 3000 : 10),
                              static_cast<std::uint8_t>(i))});
    }
    const Bytes db = sqlite_db("Big", "CREATE TABLE Big (name TEXT, n INTEGER, data BLOB)", rows);
    fe::SqliteReader r;
    std::string why;
    REQUIRE_MESSAGE(r.open(db, &why), why);
    CHECK(r.page_size() == kPage);
    std::uint32_t root = 0;
    std::vector<std::string> columns;
    REQUIRE_MESSAGE(r.table("big", &root, &columns, &why), why); // names ignore case
    CHECK(root == 2);
    CHECK(columns == std::vector<std::string>{"name", "n", "data"});
    std::vector<std::int64_t> seen;
    REQUIRE_MESSAGE(r.scan(
                        root,
                        [&](std::int64_t rowid, const fe::SqliteRow &v) {
                            const auto i = static_cast<std::size_t>(rowid - 1);
                            CHECK(v[0].bytes == std::get<std::string>(rows[i][0]));
                            CHECK(v[1].integer == static_cast<std::int64_t>(i));
                            const Bytes &want = std::get<Bytes>(rows[i][2]);
                            CHECK(v[2].bytes == std::string(want.begin(), want.end()));
                            seen.push_back(rowid);
                            return true;
                        },
                        &why),
                    why);
    REQUIRE(seen.size() == 300);
    CHECK(std::is_sorted(seen.begin(), seen.end()));
    CHECK_FALSE(r.table("Nope", &root, &columns, &why));
    CHECK(why == "the database has no table Nope");
    // Stopping early.
    int n = 0;
    CHECK(r.scan(root, [&n](std::int64_t, const fe::SqliteRow &) { return ++n < 5; }, &why));
    CHECK(n == 5);
}

TEST_CASE("sqlite: damaged databases are refused with a reason, never read past") {
    std::vector<std::vector<Value>> rows;
    for (int i = 0; i < 60; i++) {
        rows.push_back(
            {std::string(30, static_cast<char>('a' + i % 26)), Bytes(i == 7 ? 2000 : 5, 1)});
    }
    const Bytes good = sqlite_db("T", "CREATE TABLE T (a TEXT, b BLOB)", rows);
    const auto scan_error = [](const Bytes &db) {
        fe::SqliteReader r;
        std::string why;
        if (!r.open(db, &why)) {
            return "open: " + why;
        }
        std::uint32_t root = 0;
        std::vector<std::string> columns;
        if (!r.table("T", &root, &columns, &why)) {
            return "table: " + why;
        }
        if (!r.scan(root, [](std::int64_t, const fe::SqliteRow &) { return true; }, &why)) {
            return "scan: " + why;
        }
        return std::string();
    };
    REQUIRE(scan_error(good).empty());

    Bytes b = good;
    b[0] = 'X';
    CHECK(scan_error(b) == "open: not an SQLite database (no \"SQLite format 3\" header)");
    b = good;
    put_be(b, 16, 1000, 2);
    CHECK(scan_error(b) == "open: an SQLite page size of 1000 is not valid");
    b = good;
    put_be(b, 56, 2, 4);
    CHECK(scan_error(b) == "open: the database's text is UTF-16, which is not read");
    CHECK(scan_error(Bytes(good.begin(), good.begin() + 50)) ==
          "open: too short for an SQLite database");
    // The interior root's right child points to itself.
    b = good;
    REQUIRE(b[kPage] == 5);
    put_be(b, kPage + 8, 2, 4);
    CHECK(scan_error(b) == "scan: the table's tree reaches page 2 twice");
    // A child past the end.
    b = good;
    put_be(b, kPage + 8, 9999, 4);
    CHECK(scan_error(b).find("past its") != std::string::npos);
    // A leaf of another kind.
    b = good;
    b[2 * kPage] = 10;
    CHECK(scan_error(b) == "scan: page 3 is not a table page (type 10)");
    // A cell pointer outside the page.
    b = good;
    put_be(b, 2 * kPage + 8, 0xFFFF, 2);
    CHECK(scan_error(b) == "scan: a cell of page 3 points outside it");
    // Too many cells for the page.
    b = good;
    put_be(b, 2 * kPage + 3, 400, 2);
    CHECK(scan_error(b) == "scan: page 3 lists more cells than it can hold");
    // The file cut short: the last pages (the overflow chain) are gone.
    b = Bytes(good.begin(), good.end() - static_cast<std::ptrdiff_t>(kPage));
    CHECK(scan_error(b).find("scan: the database points to page") == 0);
    // Garbage everywhere after the header never crashes.
    std::mt19937 rng(374);
    for (int round = 0; round < 200; round++) {
        b = good;
        for (int k = 0; k < 20; k++) {
            b[100 + rng() % (b.size() - 100)] = static_cast<std::uint8_t>(rng());
        }
        (void)scan_error(b);
    }
}

TEST_CASE("ios backup: the game's files out of a Manifest.db and a Manifest.mbdb backup") {
    const Tree set = valid_set();
    std::vector<Entry> entries = entries_for(set);
    entries.push_back({kApp, "Documents/notes.txt", Bytes{'x'}});
    entries.push_back({kApp, "Documents/sub/theif1", Bytes{'x'}});
    entries.push_back({kApp, "Documents/theif1.mt-tmp", Bytes{'x'}});
    for (const char *format : {"db", "mbdb"}) {
        CAPTURE(format);
        const TempDir tmp("read");
        const fs::path dir = tmp.path / "00008030-ABC";
        if (std::string(format) == "db") {
            write_db_backup(dir, entries, "Test iPod", 100); // many leaves, overflow chains
        } else {
            write_mbdb_backup(dir, entries);
        }
        CHECK(fe::looks_like_ios_backup(dir.string()));
        const fe::IosBackupInfo info = fe::ios_backup_info(dir.string());
        CHECK(info.manifest == format);
        CHECK_FALSE(info.encrypted);
        CHECK(info.problem.empty());
        CHECK(info.time > 0);
        fe::IosBackupFiles files;
        std::string why;
        REQUIRE_MESSAGE(fe::read_ios_backup(dir.string(), &files, &why), why);
        Tree got;
        for (const fe::SaveFile &f : files.files) {
            got[f.name] = f.bytes;
        }
        CHECK(got == set);
        CHECK(files.ignored == std::vector<std::string>{"Documents/sub/theif1", "notes.txt"});
        CHECK(files.warnings.empty());
        CHECK(fe::check_save_set(files.files).empty());
    }
    // A folder of backups is not a backup; find_ios_backups looks inside it.
    const TempDir tmp("roots");
    write_db_backup(tmp.path / "b1", entries_for(set));
    write_mbdb_backup(tmp.path / "b2", entries_for(set));
    fs::create_directories(tmp.path / "not one");
    const std::vector<fe::IosBackupInfo> found = fe::find_ios_backups({tmp.path.string()});
    REQUIRE(found.size() == 2);
    CHECK(found[0].device == "Test iPod");
    CHECK(fe::ios_backup_label(found[0]) == "Test iPod, iOS 12.5.7");
    fe::IosBackupFiles files;
    std::string why;
    CHECK_FALSE(fe::read_ios_backup(tmp.path.string(), &files, &why));
    CHECK(why.find("is a folder of backups") != std::string::npos);
}

TEST_CASE("ios backup: damaged, encrypted and hostile backups are refused with their reason") {
    const Tree set = valid_set();
    const auto refusal = [](const fs::path &dir) {
        fe::IosBackupFiles files;
        std::string why;
        CHECK_FALSE(fe::read_ios_backup(dir.string(), &files, &why));
        return why;
    };
    const TempDir tmp("refused");
    int n = 0;
    const auto next = [&] { return tmp.path / std::to_string(n++); };

    fs::path d = next();
    write_db_backup(d, entries_for(set));
    write_plists(d, true, "x");
    CHECK(refusal(d).rfind("This backup is encrypted", 0) == 0);

    d = next();
    write_db_backup(d, entries_for(set));
    write_bytes(d / "Manifest.db", Bytes(4096, 0x5A)); // iOS 10.2+: the database encrypted too
    CHECK(refusal(d).rfind("Manifest.db is not an SQLite database", 0) == 0);

    d = next();
    write_mbdb_backup(d, entries_for(set), true);
    CHECK(refusal(d).rfind("This backup is encrypted", 0) == 0);

    d = next();
    write_db_backup(d, entries_for(set));
    {
        Bytes db = read_bytes(d / "Manifest.db");
        put_be(db, kPage + 8, 2, 4); // a cycle in the tree (the root is an interior page)
        write_bytes(d / "Manifest.db", db);
    }
    const std::string cycle = refusal(d);
    CHECK(cycle.rfind("Manifest.db cannot be read: ", 0) == 0);
    CHECK(cycle.find("page") != std::string::npos);

    d = next();
    write_mbdb_backup(d, entries_for(set));
    {
        Bytes m = read_bytes(d / "Manifest.mbdb");
        m.resize(m.size() - 9);
        write_bytes(d / "Manifest.mbdb", m);
    }
    CHECK(refusal(d).rfind("Manifest.mbdb is cut short: ", 0) == 0);

    d = next();
    write_mbdb_backup(d, entries_for(set), false, 1);
    CHECK(refusal(d).find("the manifest says") != std::string::npos);

    d = next();
    std::vector<Entry> twice = entries_for(set);
    twice.push_back({kApp, "Documents/config", Bytes{3, 1, 0, 1, 1, 1}});
    write_mbdb_backup(d, twice);
    CHECK(refusal(d) == "the backup lists Documents/config twice");

    d = next();
    std::vector<Entry> linked = entries_for(set);
    linked.push_back({kApp, "Documents/theif1", std::nullopt, true});
    write_db_backup(d, linked);
    CHECK(refusal(d) == "Documents/theif1 is a link in the backup, not a file");

    d = next();
    write_db_backup(d, entries_for(set));
    const std::string id = fe::ios_file_id(kApp, "Documents/theif0");
    fs::remove(d / id.substr(0, 2) / id);
    CHECK(refusal(d).find("is missing") != std::string::npos);

    d = next();
    write_db_backup(d, entries_for({}));
    CHECK(refusal(d) ==
          "this backup has no saves of the game in " + std::string(kApp) + "'s Documents/");

    d = next();
    write_db_backup(d, {{"HomeDomain", "Library/x", Bytes{1}}});
    CHECK(refusal(d).rfind("this backup has no files of MapleStory Thief Edition", 0) == 0);

    d = next();
    fs::create_directories(d);
    write_bytes(d / "Manifest.plist", Bytes{'n', 'o'});
    CHECK(refusal(d) == "Manifest.plist is not a property list");

    d = next();
    fs::create_directories(d);
    write_bytes(d / "Status.mdinfo", Bytes{1});
    write_plists(d, false, "x");
    CHECK(refusal(d) == "an iOS 3 backup (.mdinfo files): not supported");
}

TEST_CASE("save menu: an iPhone backup is imported from the Saves tab with only a controller "
          "(#374)") {
    const TempDir tmp("menu");
    const fe::SaveLocation loc = location(tmp.path / "data");
    const Tree before = {{"theif0", save_fixtures::new_character(9)},
                         {"quest0", save_fixtures::new_quests(2)}};
    for (const auto &[name, bytes] : before) {
        write_bytes(fs::path(loc.documents_dir) / name, bytes);
    }
    // Finder's folder holds a backup of the game; import/ holds a damaged one.
    const fs::path finder = tmp.path / "MobileSync" / "Backup";
    const Tree set = valid_set();
    write_db_backup(finder / "00008030-AAAA", entries_for(set));
    write_mbdb_backup(fs::path(loc.data_dir) / "import" / "old-phone", entries_for(set), false, 1);
    const auto now = fs::file_time_type::clock::now();
    fs::last_write_time(finder / "00008030-AAAA" / "Manifest.db", now - std::chrono::hours(1));
    fs::last_write_time(fs::path(loc.data_dir) / "import" / "old-phone" / "Manifest.mbdb",
                        now - std::chrono::hours(48));

    std::int32_t panel = -1;
    fe::Settings live;
    fe::SettingsMenu menu(fe::settings_registry(), nullptr, nullptr, nullptr);
    menu.attach(&live);
    menu.set_action("ios_backup", [&] {
        return fe::ios_backup_action(
            loc, {finder.string()}, [&panel] { return panel; }, [] { return stamp(1); },
            [&menu](fe::MenuList l) { menu.open_list(std::move(l)); });
    });
    press(menu, SDL_GAMEPAD_BUTTON_GUIDE);
    REQUIRE(menu.is_open());
    pad_to_action(menu, "ios_backup");
    // With a character loaded it is refused at once.
    panel = 0;
    press(menu, SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK_FALSE(menu.in_list());
    CHECK(menu.status() == "Import from the title or the main menu (a character is loaded)");
    panel = -1;
    press(menu, SDL_GAMEPAD_BUTTON_SOUTH);
    REQUIRE(menu.in_list());
    REQUIRE(menu.list().rows.size() == 2);
    CHECK(menu.list().rows[0].label == "Test iPod, iOS 12.5.7");
    CHECK(menu.list().rows[1].label == "Old iPhone, iOS 12.5.7");

    // The damaged one (second row): refused with its reason, nothing changed.
    press(menu, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    press(menu, SDL_GAMEPAD_BUTTON_SOUTH);
    REQUIRE(menu.confirming());
    CHECK(menu.help().rfind("Import the saves of Old iPhone, iOS 12.5.7 (", 0) == 0);
    press(menu, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    press(menu, SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK(menu.status().rfind("Not imported: Documents/", 0) == 0);
    CHECK(menu.status().find("the manifest says") != std::string::npos);
    CHECK(tree(loc.documents_dir) == before);
    CHECK_FALSE(fs::exists(loc.backups_dir));

    // The good one: Yes imports it, and what it replaced is backed up.
    press(menu, SDL_GAMEPAD_BUTTON_SOUTH);
    REQUIRE(menu.in_list());
    press(menu, SDL_GAMEPAD_BUTTON_SOUTH);
    REQUIRE(menu.confirming());
    CHECK(tree(loc.documents_dir) == before); // No is focused
    press(menu, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    press(menu, SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK(menu.status() ==
          "Imported " + std::to_string(set.size()) + " files from Test iPod, iOS 12.5.7");
    CHECK_FALSE(menu.in_list());
    CHECK(tree(loc.documents_dir) == set);
    CHECK(tree(fs::path(loc.backups_dir) / "import" / stamp(1)) == before);
}

TEST_CASE("save menu: a backup whose saves fail the checks is refused and nothing is changed") {
    const TempDir tmp("menu-bad");
    const fe::SaveLocation loc = location(tmp.path / "data");
    Tree bad = valid_set();
    bad["theif0"].push_back(0); // the wrong size
    write_db_backup(fs::path(loc.data_dir) / "import" / "phone", entries_for(bad));
    const std::string status = fe::import_ios_backup_status(
        loc, (fs::path(loc.data_dir) / "import" / "phone").string(), stamp(1));
    CHECK(status.rfind("Not imported: theif0: bad_size: ", 0) == 0);
    CHECK(status.find("nothing was imported") != std::string::npos);
    CHECK(tree(loc.documents_dir).empty());
    CHECK_FALSE(fs::exists(loc.backups_dir));
    // Nothing found: what to do (and import/ is there to put one in).
    const TempDir empty("menu-none");
    const fe::SaveLocation none = location(empty.path);
    CHECK(fe::ios_backup_action(none, {}, nullptr, nullptr, nullptr) ==
          "No iPhone backup found: make an unencrypted one, or copy its folder into import/");
    CHECK(fs::is_directory(empty.path / "import"));
    CHECK(fe::ios_backup_action(fe::SaveLocation{}, {}, nullptr, nullptr, nullptr) ==
          "No data folder: use --import-saves");
    // The usual places.
#if defined(_WIN32)
    CHECK(fe::default_ios_backup_roots("C:\\Users\\p", "C:\\Users\\p\\AppData\\Roaming").size() ==
          2);
#elif defined(__APPLE__) && !defined(SDL_PLATFORM_IOS)
    CHECK(fe::default_ios_backup_roots("/Users/p") ==
          std::vector<std::string>{"/Users/p/Library/Application Support/MobileSync/Backup"});
#else
    CHECK(fe::default_ios_backup_roots("/home/p").empty());
#endif
}

TEST_CASE("save menu: Check saves lists each file's verdict with the controller; nothing is "
          "written") {
    const TempDir tmp("check");
    const fe::SaveLocation loc = location(tmp.path);
    CHECK(fe::check_saves_action(loc, nullptr) == "No saves to check yet");
    Tree set = valid_set();
    set["config"] = {3, 3, 0, 1, 1, 1}; // speed byte 3
    for (const auto &[name, bytes] : set) {
        write_bytes(fs::path(loc.documents_dir) / name, bytes);
    }
    fe::Settings live;
    fe::SettingsMenu menu(fe::settings_registry(), nullptr, nullptr, nullptr);
    menu.attach(&live);
    menu.set_action("check_saves", [&] {
        return fe::check_saves_action(loc,
                                      [&menu](fe::MenuList l) { menu.open_list(std::move(l)); });
    });
    press(menu, SDL_GAMEPAD_BUTTON_GUIDE);
    pad_to_action(menu, "check_saves");
    press(menu, SDL_GAMEPAD_BUTTON_SOUTH);
    REQUIRE(menu.in_list());
    CHECK(menu.list().title == "CHECK: 6 of 7 files pass");
    std::size_t config = 99;
    for (std::size_t i = 0; i < menu.list().rows.size(); i++) {
        if (menu.list().rows[i].label == "config") {
            config = i;
            CHECK(menu.list().rows[i].value == "bad_config");
        } else {
            CHECK(menu.list().rows[i].value == "ok");
        }
    }
    REQUIRE(config < menu.list().rows.size());
    for (std::size_t i = 0; i < config; i++) {
        press(menu, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    }
    press(menu, SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK(menu.status().rfind("config: bad_config: ", 0) == 0);
    CHECK(tree(loc.documents_dir) == set);
    CHECK_FALSE(fs::exists(loc.backups_dir));
}

TEST_CASE("save menu: saves from another folder: the other data folder and import/'s folders") {
    const TempDir tmp("other");
    const fe::SaveLocation loc = location(tmp.path / "game" / "data");
    const fs::path user = tmp.path / "user";
    const Tree set = valid_set();
    write_bytes(user / "settings.ini", Bytes{'x'});
    for (const auto &[name, bytes] : set) {
        write_bytes(user / "saves" / name, bytes);
    }
    // An old kit's data folder (Documents/) and a folder without saves in import/.
    write_bytes(fs::path(loc.data_dir) / "import" / "old kit" / "Documents" / "config",
                save_fixtures::kConfig);
    fs::create_directories(fs::path(loc.data_dir) / "import" / "empty");

    fe::DataLocation in_game = fe::data_location_at(loc.data_dir, fe::DataPlace::kGame);
    const auto others = fe::other_data_dirs(in_game, [&user] { return user.string(); });
    REQUIRE(others.size() == 1);
    CHECK(others[0].second == "user folder");
    fe::DataLocation in_user = fe::data_location_at(
        user.string(), fe::DataPlace::kUser, fe::DataReason::kFlag, (tmp.path / "game").string());
    const auto back = fe::other_data_dirs(in_user, [] { return std::string(); });
    REQUIRE(back.size() == 1);
    CHECK(fs::path(back[0].first) == tmp.path / "game" / "data");
    CHECK(back[0].second == "game folder");

    const std::vector<fe::OtherSaveFolder> folders = fe::other_save_folders(loc, others);
    REQUIRE(folders.size() == 2);
    CHECK(folders[0].name == "user folder");
    CHECK(fs::path(folders[0].source) == user / "saves");
    CHECK(folders[1].name == "old kit");
    CHECK(folders[1].where == "import/");
    CHECK(fs::path(folders[1].source) ==
          fs::path(loc.data_dir) / "import" / "old kit" / "Documents");

    std::int32_t panel = -1;
    fe::Settings live;
    fe::SettingsMenu menu(fe::settings_registry(), nullptr, nullptr, nullptr);
    menu.attach(&live);
    menu.set_action("other_saves", [&] {
        return fe::other_folder_action(
            loc, others, [&panel] { return panel; }, [] { return stamp(2); },
            [&menu](fe::MenuList l) { menu.open_list(std::move(l)); });
    });
    press(menu, SDL_GAMEPAD_BUTTON_GUIDE);
    pad_to_action(menu, "other_saves");
    press(menu, SDL_GAMEPAD_BUTTON_SOUTH);
    REQUIRE(menu.in_list());
    press(menu, SDL_GAMEPAD_BUTTON_SOUTH);
    REQUIRE(menu.confirming());
    CHECK(menu.help() == "Bring the saves of user folder here? The files they replace are backed "
                         "up first.");
    press(menu, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    press(menu, SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK(menu.status() == "Imported " + std::to_string(set.size()) + " files from user folder");
    CHECK(tree(loc.documents_dir) == set);
    // The other folder is never changed.
    CHECK(tree(user / "saves") == set);
    // Nothing to bring: what to do.
    const TempDir lone("other-none");
    CHECK(fe::other_folder_action(location(lone.path), {}, nullptr, nullptr, nullptr) ==
          "No other folder with saves: copy the old data folder into import/");
}
