// The first-launch IPA import (#182, ADR 0100, ipa_import.h, import_screen.h) on synthetic IPAs
// built here: SHA-256, the property list and Mach-O readers, bootstrap.py's entry rules; each
// refusal's reason (not a zip, no Payload, another app, an encrypted entry, an encrypted
// executable, another version, other bytes); hostile entries that write nothing; an
// interrupted import that leaves no game/ and whose temporary folder goes; leftovers removed;
// a re-import replacing game/; the import screen's state, input and loop on a dummy-driver
// window. The real IPA: ipa_import_real_test.cpp (local only).
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include "canvas.h"
#include "game.h"
#include "import_screen.h"
#include "ipa_import.h"
#include "save_zip.h"
#include "settings.h"

namespace {

namespace fs = std::filesystem;
namespace fe = maplethief::frontend;
using Bytes = std::vector<std::uint8_t>;
using fe::IpaProblem;

Bytes bytes_of(const std::string &s) { return Bytes(s.begin(), s.end()); }

void put_le(Bytes &f, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; i++) {
        f[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
    }
}
void put_be(Bytes &f, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; i++) {
        f[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (24 - 8 * i));
    }
}

// A fat executable like the game's: armv6 at 0x1000 and armv7 at 0x2000, each a thin Mach-O
// with one LC_ENCRYPTION_INFO whose cryptid is `cryptid`; `tag` makes the bytes differ.
Bytes executable(std::uint32_t cryptid = 0, std::uint8_t tag = 0) {
    Bytes f(0x2100, 0);
    put_be(f, 0, 0xCAFEBABEu);
    put_be(f, 4, 2);
    const std::uint32_t subs[2] = {6, 9};
    for (std::uint32_t i = 0; i < 2; i++) {
        const std::size_t a = 8 + 20 * i, at = 0x1000 * (i + 1);
        put_be(f, a, 12);
        put_be(f, a + 4, subs[i]);
        put_be(f, a + 8, static_cast<std::uint32_t>(at));
        put_be(f, a + 12, 0x100);
        put_be(f, a + 16, 12);
        put_le(f, at, 0xFEEDFACEu);
        put_le(f, at + 4, 12);
        put_le(f, at + 8, subs[i]);
        put_le(f, at + 16, 1);  // ncmds
        put_le(f, at + 20, 20); // sizeofcmds
        put_le(f, at + 28, 0x21);
        put_le(f, at + 32, 20);
        put_le(f, at + 36, 0x1000);
        put_le(f, at + 40, 0x800);
        put_le(f, at + 44, cryptid);
    }
    f[0x20FF] = tag;
    return f;
}

// A binary property list holding one dictionary of ASCII strings.
Bytes bplist(const std::map<std::string, std::string> &dict) {
    Bytes b = bytes_of("bplist00");
    const std::size_t n = dict.size();
    std::vector<std::size_t> offsets;
    offsets.push_back(b.size());
    b.push_back(static_cast<std::uint8_t>(0xD0 | n));
    for (std::size_t i = 0; i < n; i++) {
        b.push_back(static_cast<std::uint8_t>(1 + i)); // keys
    }
    for (std::size_t i = 0; i < n; i++) {
        b.push_back(static_cast<std::uint8_t>(1 + n + i)); // values
    }
    const auto string = [&](const std::string &s) {
        offsets.push_back(b.size());
        if (s.size() < 15) {
            b.push_back(static_cast<std::uint8_t>(0x50 | s.size()));
        } else {
            b.push_back(0x5F);
            b.push_back(0x10);
            b.push_back(static_cast<std::uint8_t>(s.size()));
        }
        b.insert(b.end(), s.begin(), s.end());
    };
    for (const auto &kv : dict) {
        string(kv.first);
    }
    for (const auto &kv : dict) {
        string(kv.second);
    }
    const std::size_t table = b.size();
    for (const std::size_t o : offsets) {
        b.push_back(static_cast<std::uint8_t>(o));
    }
    Bytes trailer(32, 0);
    trailer[6] = 1; // offset size
    trailer[7] = 1; // ref size
    trailer[15] = static_cast<std::uint8_t>(offsets.size());
    trailer[23] = 0; // top object
    trailer[31] = static_cast<std::uint8_t>(table);
    b.insert(b.end(), trailer.begin(), trailer.end());
    return b;
}

struct Ipa {
    Bytes zip;
    fe::IpaExpected expected; // this IPA's own size and hashes
};

// A synthetic IPA: Payload/<app>/Info.plist (version), the executable, two data files and the
// iTunesArtwork entry at the top. `extra` entries are added as they are.
Ipa make_ipa(const std::string &version = "1.5", std::uint32_t cryptid = 0,
             std::vector<fe::ZipEntry> extra = {}, const std::string &app = "MapleThief.app",
             std::uint8_t tag = 0) {
    std::map<std::string, std::string> info = {{"CFBundleExecutable", "MapleThief"},
                                               {"CFBundleIdentifier", "com.example.thief"}};
    if (!version.empty()) {
        info["CFBundleVersion"] = version;
    }
    const Bytes exe = executable(cryptid, tag);
    std::vector<fe::ZipEntry> entries = {
        {"Payload/" + app + "/Info.plist", bplist(info)},
        {"Payload/" + app + "/MapleThief", exe},
        {"Payload/" + app + "/data/a.txt", bytes_of("first file")},
        {"Payload/" + app + "/b.bin", Bytes(300, tag)},
        {"iTunesArtwork", bytes_of("not extracted")},
    };
    entries.insert(entries.end(), extra.begin(), extra.end());
    Ipa ipa;
    ipa.zip = fe::zip_store(entries);
    ipa.expected.size = ipa.zip.size();
    ipa.expected.ipa_sha256 = fe::sha256_of(ipa.zip);
    ipa.expected.executable_sha256 = fe::sha256_of(exe);
    return ipa;
}

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

// The same IPA with its expected values recomputed (after a byte edit).
Ipa rehash(Ipa ipa) {
    ipa.expected.size = ipa.zip.size();
    ipa.expected.ipa_sha256 = fe::sha256_of(ipa.zip);
    return ipa;
}

// A fresh, empty folder for one test.
fs::path fresh_dir(const std::string &name) {
    const fs::path dir = fs::temp_directory_path() / "mt_ipa_import_test" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

Bytes read_file(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
void write_file(const fs::path &p, const Bytes &b) {
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char *>(b.data()), static_cast<std::streamsize>(b.size()));
}

// The names in `dir` (not recursive), sorted.
std::vector<std::string> names_in(const fs::path &dir) {
    std::vector<std::string> names;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        names.push_back(it->path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

fe::IpaResult import_bytes(const Ipa &ipa, const fs::path &data,
                           const fe::IpaProgress &progress = {}) {
    return fe::import_ipa_bytes(ipa.zip, data.string(), ipa.expected, progress);
}

} // namespace

TEST_CASE("ipa import: SHA-256, its hex, and the 1.5 values") {
    const Bytes abc = bytes_of("abc");
    CHECK(fe::sha256_hex(fe::sha256_of(abc)) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    fe::Sha256 d{};
    REQUIRE(fe::parse_sha256(fe::kIpaSha256, &d));
    CHECK(fe::sha256_hex(d) == fe::kIpaSha256);
    CHECK_FALSE(fe::parse_sha256("abc", &d));
    CHECK_FALSE(fe::parse_sha256(std::string(64, 'g'), &d));
    const fe::IpaExpected e = fe::ipa_expected();
    CHECK(e.size == 17872861);
    CHECK(fe::sha256_hex(e.ipa_sha256) == fe::kIpaSha256);
    CHECK(fe::sha256_hex(e.executable_sha256) == fe::kExecutableSha256);
}

TEST_CASE("ipa import: Info.plist's strings from a binary or XML plist; hostile ones refused") {
    std::string v;
    const Bytes b = bplist({{"CFBundleVersion", "1.5"}, {"CFBundleExecutable", "MapleThief"}});
    REQUIRE(fe::plist_string(b, "CFBundleVersion", &v));
    CHECK(v == "1.5");
    REQUIRE(fe::plist_string(b, "CFBundleExecutable", &v));
    CHECK(v == "MapleThief");
    CHECK_FALSE(fe::plist_string(b, "CFBundleShortVersionString", &v));
    const Bytes longer = bplist({{"CFBundleShortVersionString", "1.4.0-a-long-version"}});
    REQUIRE(fe::plist_string(longer, "CFBundleShortVersionString", &v));
    CHECK(v == "1.4.0-a-long-version");

    // UTF-16 strings: a key and a value of type 0x6.
    Bytes u = bytes_of("bplist00");
    u.insert(u.end(), {0xD1, 1, 2});
    const std::string key = "CFBundleVersion";
    u.push_back(0x5F), u.push_back(0x10), u.push_back(static_cast<std::uint8_t>(key.size()));
    u.insert(u.end(), key.begin(), key.end());
    const std::size_t value_at = u.size();
    u.insert(u.end(), {0x63, 0, '1', 0, '.', 0x00, 0xE9}); // "1.é"
    const std::size_t table = u.size();
    u.insert(u.end(), {8, 11, static_cast<std::uint8_t>(value_at)});
    Bytes trailer(32, 0);
    trailer[6] = 1, trailer[7] = 1, trailer[15] = 3, trailer[31] = static_cast<std::uint8_t>(table);
    u.insert(u.end(), trailer.begin(), trailer.end());
    REQUIRE(fe::plist_string(u, "CFBundleVersion", &v));
    CHECK(v == "1.\xC3\xA9");

    const Bytes xml = bytes_of("<?xml version=\"1.0\"?>\n<plist version=\"1.0\"><dict>\n"
                               "\t<key>CFBundleVersion</key>\n\t<string>1.4</string>\n"
                               "</dict></plist>\n");
    REQUIRE(fe::plist_string(xml, "CFBundleVersion", &v));
    CHECK(v == "1.4");

    // Every cut and a few broken bytes: never true with garbage, never a crash.
    for (std::size_t n = 0; n < b.size(); n++) {
        CHECK_FALSE(fe::plist_string(Bytes(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(n)),
                                     "CFBundleVersion", &v));
    }
    for (std::size_t i = 8; i < b.size(); i++) {
        Bytes bad = b;
        bad[i] ^= 0xFF;
        fe::plist_string(bad, "CFBundleVersion", &v); // whatever it says, it stays inside
    }
    CHECK_FALSE(fe::plist_string(bytes_of("not a plist"), "CFBundleVersion", &v));
}

TEST_CASE("ipa import: the cryptid of LC_ENCRYPTION_INFO; hostile load commands refused") {
    std::uint32_t id = 7;
    const Bytes clear = executable(0), locked = executable(1);
    REQUIRE(fe::macho_cryptid(std::span<const std::uint8_t>(clear).subspan(0x2000, 0x100), &id));
    CHECK(id == 0);
    REQUIRE(fe::macho_cryptid(std::span<const std::uint8_t>(locked).subspan(0x2000, 0x100), &id));
    CHECK(id == 1);
    Bytes bad = locked;
    put_le(bad, 0x2000 + 32, 0x7FFFFFF0u); // cmdsize past the end
    CHECK_FALSE(fe::macho_cryptid(std::span<const std::uint8_t>(bad).subspan(0x2000, 0x100), &id));
    bad = locked;
    put_le(bad, 0x2000 + 20, 0xFFFFFFF0u); // sizeofcmds past the end
    CHECK_FALSE(fe::macho_cryptid(std::span<const std::uint8_t>(bad).subspan(0x2000, 0x100), &id));
    bad = locked;
    put_le(bad, 0x2000 + 32, 0); // a zero cmdsize would loop
    CHECK_FALSE(fe::macho_cryptid(std::span<const std::uint8_t>(bad).subspan(0x2000, 0x100), &id));
    CHECK_FALSE(fe::macho_cryptid(Bytes(10, 0), &id));
}

TEST_CASE("ipa import: entry names follow bootstrap.py's entry_parts") {
    std::vector<std::string> parts;
    std::string why;
    REQUIRE(fe::ipa_entry_parts("Payload/MapleThief.app/a b/c.png", &parts, &why));
    CHECK(parts == std::vector<std::string>{"Payload", "MapleThief.app", "a b", "c.png"});
    REQUIRE(fe::ipa_entry_parts("Payload/", &parts, &why));
    CHECK(parts == std::vector<std::string>{"Payload"});
    for (const char *ok : {"conx", "com", "comx1", "a.con", "x/lpt", "\xC3\xA9t\xC3\xA9"}) {
        INFO(ok);
        CHECK(fe::ipa_entry_parts(ok, &parts, &why));
    }
    for (const char *bad :
         {"/etc/passwd", "a/../b", "../a",      "a/./b",     "a//b",    "",        "a\\b",
          "C:/x",        "C:x",    "a:b",       "a\x01",     "a<b",     "a>b",     "a\"b",
          "a|b",         "a?b",    "a*b",       "con",       "CON.txt", "aux.a.b", "Nul",
          "com1",        "LPT9.x", "trailing.", "trailing ", "x/prn/y"}) {
        INFO(bad);
        why.clear();
        CHECK_FALSE(fe::ipa_entry_parts(bad, &parts, &why));
        CHECK_FALSE(why.empty());
        CHECK(parts.empty());
    }
}

TEST_CASE("ipa import: each wrong file is refused with its own reason") {
    const Ipa good = make_ipa();
    CHECK(fe::check_ipa_bytes(good.zip, good.expected).ok());
    // The 1.5 IPA's real values, which no synthetic file has: each check explains why.
    const fe::IpaExpected real = fe::ipa_expected();

    const auto problem = [&](const Bytes &file, const fe::IpaExpected &e) {
        const fe::IpaResult r = fe::check_ipa_bytes(file, e);
        INFO(r.why);
        CHECK_FALSE(r.why.empty());
        return r.problem;
    };
    // Not a zip at all: text, nothing, random bytes.
    CHECK(problem(bytes_of("hello, this is a text file\n"), real) == IpaProblem::kNotZip);
    CHECK(problem({}, real) == IpaProblem::kNotZip);
    CHECK(problem(Bytes(5000, 0x5A), real) == IpaProblem::kNotZip);
    // A zip without Payload/<name>.app/.
    CHECK(problem(fe::zip_store(std::vector<fe::ZipEntry>{{"readme.txt", bytes_of("hi")},
                                                          {"Payload/notes.txt", bytes_of("x")}}),
                  real) == IpaProblem::kNotIpa);
    // Another app's IPA.
    CHECK(problem(make_ipa("1.5", 0, {}, "OtherGame.app").zip, real) == IpaProblem::kOtherApp);
    // Another version (its own SHA-256, so not the 1.5 IPA).
    const fe::IpaResult v14 = fe::check_ipa_bytes(make_ipa("1.4").zip, real);
    CHECK(v14.problem == IpaProblem::kOtherVersion);
    CHECK(v14.version == "1.4");
    CHECK(v14.why.find("1.4") != std::string::npos);
    // No version the reader can find: "not version 1.5".
    CHECK(problem(make_ipa("").zip, real) == IpaProblem::kOtherVersion);
    // Version 1.5, other bytes.
    CHECK(problem(good.zip, real) == IpaProblem::kModified);
    // An encrypted executable (cryptid 1 in the armv7 slice).
    const fe::IpaResult enc = fe::check_ipa_bytes(make_ipa("1.5", 1).zip, real);
    CHECK(enc.problem == IpaProblem::kEncryptedApp);
    CHECK(enc.why.find("encrypted") != std::string::npos);
    // An entry encrypted with a zip password (the general purpose flag's bit 0).
    Ipa zipcrypt = make_ipa();
    zipcrypt.zip[central_entries(zipcrypt.zip)[2] + 8] |= 1;
    CHECK(problem(zipcrypt.zip, real) == IpaProblem::kEncryptedZip);
    // Even with its own values: the import refuses an encrypted entry.
    zipcrypt = rehash(zipcrypt);
    const fs::path data = fresh_dir("encrypted_entry");
    CHECK(import_bytes(zipcrypt, data).problem == IpaProblem::kEncryptedZip);
    CHECK_FALSE(fs::exists(data / "game"));

    // A file that is not there, and a folder.
    CHECK(fe::check_ipa((data / "missing.ipa").string(), real).problem == IpaProblem::kUnreadable);
    CHECK(fe::check_ipa(data.string(), real).problem == IpaProblem::kUnreadable);
}

TEST_CASE("ipa import: a synthetic IPA is extracted into game/ with its executable") {
    const Ipa ipa = make_ipa();
    const fs::path data = fresh_dir("success");
    const fs::path file = data / "MapleThief-1.5.ipa";
    write_file(file, ipa.zip);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> calls;
    const fe::IpaResult r = fe::import_ipa(file.string(), data.string(), ipa.expected,
                                           [&calls](std::uint64_t done, std::uint64_t total) {
                                               calls.emplace_back(done, total);
                                               return true;
                                           });
    REQUIRE_MESSAGE(r.ok(), r.why);
    CHECK(fs::path(r.game_dir) == data / "game");
    CHECK(fe::imported_game_dir(data.string()) == (data / "game").string());
    CHECK(fe::has_imported_game(data.string()));
    CHECK(read_file(data / "game" / "MapleThief") == executable());
    CHECK(read_file(data / "game" / "data" / "a.txt") == bytes_of("first file"));
    CHECK(read_file(data / "game" / "b.bin") == Bytes(300, 0));
    CHECK(fs::exists(data / "game" / "Info.plist"));
    CHECK_FALSE(fs::exists(data / "game" / "iTunesArtwork"));
    CHECK(names_in(data) == std::vector<std::string>{"MapleThief-1.5.ipa", "game"});
    REQUIRE(calls.size() == 4);
    CHECK(calls.back().first == calls.back().second);
}

TEST_CASE("ipa import: unsafe entries are refused and nothing is written") {
    struct Bad {
        const char *what;
        Ipa ipa;
    };
    std::vector<Bad> bad = {
        {"..", make_ipa("1.5", 0, {{"Payload/MapleThief.app/../../evil.txt", bytes_of("x")}})},
        {"absolute", make_ipa("1.5", 0, {{"/tmp/evil.txt", bytes_of("x")}})},
        {"backslash", make_ipa("1.5", 0, {{"Payload/MapleThief.app/..\\..\\evil", bytes_of("x")}})},
        {"drive", make_ipa("1.5", 0, {{"C:/evil.txt", bytes_of("x")}})},
        {"reserved", make_ipa("1.5", 0, {{"Payload/MapleThief.app/aux.txt", bytes_of("x")}})},
        {"case", make_ipa("1.5", 0, {{"Payload/MapleThief.app/DATA/A.TXT", bytes_of("x")}})},
        {"file and folder",
         make_ipa("1.5", 0, {{"Payload/MapleThief.app/b.bin/c", bytes_of("x")}})},
    };
    // A symbolic link (Unix mode 0120777 in the external attributes).
    Ipa link = make_ipa("1.5", 0, {{"Payload/MapleThief.app/link", bytes_of("/etc/passwd")}});
    put_le(link.zip, central_entries(link.zip).back() + 38, 0120777u << 16);
    bad.push_back({"symlink", rehash(link)});
    Ipa fifo = make_ipa("1.5", 0, {{"Payload/MapleThief.app/fifo", {}}});
    put_le(fifo.zip, central_entries(fifo.zip).back() + 38, 0010644u << 16);
    bad.push_back({"fifo", rehash(fifo)});
    for (const Bad &b : bad) {
        INFO(b.what);
        const fs::path data = fresh_dir(std::string("unsafe_") + b.what);
        const fe::IpaResult r = import_bytes(b.ipa, data);
        INFO(r.why);
        CHECK(r.problem == IpaProblem::kUnsafeEntry);
        CHECK(names_in(data).empty());
        CHECK_FALSE(fs::exists(data.parent_path() / "evil.txt"));
    }
}

TEST_CASE("ipa import: an interrupted import leaves no game/, and its folder goes") {
    const Ipa ipa = make_ipa();
    const fs::path data = fresh_dir("interrupted");
    // Stopped after the second file: the files so far are in game.tmp-*, which goes.
    int calls = 0;
    const fe::IpaResult r =
        import_bytes(ipa, data, [&calls](std::uint64_t, std::uint64_t) { return ++calls < 2; });
    CHECK(r.problem == IpaProblem::kInterrupted);
    CHECK(calls == 2);
    CHECK(names_in(data).empty());
    CHECK_FALSE(fe::has_imported_game(data.string()));

    // A killed import's leftovers (game.tmp-*, game.old-*) go at the next try.
    fs::create_directories(data / "game.tmp-dead" / "sub");
    write_file(data / "game.tmp-dead" / "sub" / "half.png", bytes_of("half"));
    fs::create_directories(data / "game.old-dead");
    write_file(data / "keep.txt", bytes_of("the player's"));
    REQUIRE(import_bytes(ipa, data).ok());
    CHECK(names_in(data) == std::vector<std::string>{"game", "keep.txt"});

    // Interrupting a re-import keeps the game/ there was.
    const Ipa other = make_ipa("1.5", 0, {{"Payload/MapleThief.app/new.txt", bytes_of("n")}},
                               "MapleThief.app", 3);
    calls = 0;
    CHECK(import_bytes(other, data, [&calls](std::uint64_t, std::uint64_t) {
              return ++calls < 3;
          }).problem == IpaProblem::kInterrupted);
    CHECK(names_in(data) == std::vector<std::string>{"game", "keep.txt"});
    CHECK(read_file(data / "game" / "b.bin") == Bytes(300, 0));
    CHECK_FALSE(fs::exists(data / "game" / "new.txt"));

    // A wrong executable (another build's) is found before game/ is touched.
    fe::IpaExpected wrong = other.expected;
    wrong.executable_sha256 = fe::sha256_of(bytes_of("another executable"));
    const fe::IpaResult w = fe::import_ipa_bytes(other.zip, data.string(), wrong);
    CHECK(w.problem == IpaProblem::kWrongExecutable);
    CHECK(names_in(data) == std::vector<std::string>{"game", "keep.txt"});
    CHECK(read_file(data / "game" / "b.bin") == Bytes(300, 0));

    // Importing again replaces game/ as a whole.
    REQUIRE(import_bytes(other, data).ok());
    CHECK(names_in(data) == std::vector<std::string>{"game", "keep.txt"});
    CHECK(read_file(data / "game" / "b.bin") == Bytes(300, 3));
    CHECK(read_file(data / "game" / "new.txt") == bytes_of("n"));
    CHECK(read_file(data / "game" / "MapleThief") == executable(0, 3));
}

TEST_CASE("bundle: --bundle, MAPLETHIEF_BUNDLE_DIR, the build's default, next to the "
          "executable, the imported game, else the import") {
    const fs::path root = fresh_dir("lookup");
    const fs::path flag = root / "flag", env = root / "env", dev = root / "dev",
                   beside = root / "exe" / "game" / "MapleThief.app", data = root / "data";
    for (const fs::path &p : {flag, env, dev, beside, data / "game"}) {
        fs::create_directories(p);
    }
    write_file(data / "game" / "MapleThief", bytes_of("exe"));
    const std::string missing = (root / "missing").string();
    const std::string envs = env.string(), devs = dev.string();

    fe::BundleSearch s;
    s.flag = flag.string();
    s.env = envs.c_str();
    s.build_default = devs.c_str();
    s.beside_exe = beside.string();
    s.data_dir = data.string();
    fe::BundleChoice c = fe::find_bundle(s);
    CHECK(c.dir == flag.string());
    CHECK(c.source == "--bundle");
    s.flag.clear();
    CHECK(fe::find_bundle(s).dir == envs);
    CHECK(fe::find_bundle(s).source == "MAPLETHIEF_BUNDLE_DIR");
    s.env = "";
    CHECK(fe::find_bundle(s).dir == devs);
    s.env = nullptr;
    CHECK(fe::find_bundle(s).dir == devs);
    // A build default that is not there (a dev build before scripts/bootstrap) is skipped, as
    // is none (a player build).
    s.build_default = missing.c_str();
    CHECK(fe::find_bundle(s).dir == beside.string());
    s.build_default = nullptr;
    c = fe::find_bundle(s);
    CHECK(c.dir == beside.string());
    CHECK(c.source == "next to the executable");
    s.beside_exe = missing;
    c = fe::find_bundle(s);
    CHECK(c.dir == (data / "game").string());
    CHECK(c.source == "the imported game");
    CHECK_FALSE(c.import);
    // game/ without its executable is not a bundle.
    fs::remove(data / "game" / "MapleThief");
    c = fe::find_bundle(s);
    CHECK(c.dir.empty());
    CHECK(c.import);
    CHECK(c.why.find("--import-ipa") != std::string::npos);
    s.data_dir.clear();
    CHECK(fe::find_bundle(s).import);

    // Named by the player, it must be there: an error, not the import screen.
    s.flag = missing;
    c = fe::find_bundle(s);
    CHECK(c.dir.empty());
    CHECK_FALSE(c.import);
    CHECK(c.why.find("--bundle") != std::string::npos);
    s.flag.clear();
    s.env = missing.c_str();
    c = fe::find_bundle(s);
    CHECK(c.dir.empty());
    CHECK_FALSE(c.import);
    CHECK(c.why.find("MAPLETHIEF_BUNDLE_DIR") != std::string::npos);
}

TEST_CASE("import screen: text helpers") {
    CHECK(fe::wrap_text("one two three", 7) == std::vector<std::string>{"one two", "three"});
    CHECK(fe::wrap_text("abcdefghij", 4) == std::vector<std::string>{"abcd", "efgh", "ij"});
    CHECK(fe::wrap_text("", 10).empty());
    CHECK(fe::shorten_path("C:/a/b/c", 20) == "C:/a/b/c");
    CHECK(fe::shorten_path("C:/Users/someone/AppData/Roaming/MapleThief", 16) == "...g/MapleThief");
    CHECK(fe::shorten_path("C:/Users/someone/AppData/Roaming/MapleThief", 16).size() == 16);
}

namespace {

SDL_Event drop_event(const char *path) {
    SDL_Event e{};
    e.type = SDL_EVENT_DROP_FILE;
    e.drop.data = path;
    return e;
}
SDL_Event key_event(SDL_Keycode k) {
    SDL_Event e{};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = k;
    e.key.down = true;
    return e;
}
std::uint64_t fnv(const std::vector<std::uint8_t> &px) {
    std::uint64_t h = 1469598103934665603ull;
    for (const std::uint8_t b : px) {
        h = (h ^ b) * 1099511628211ull;
    }
    return h;
}

} // namespace

TEST_CASE("import screen: choose, drop, progress, a refusal, a retry and the import") {
    const Ipa ipa = make_ipa();
    const fs::path data = fresh_dir("screen");
    const fs::path good = data / "good.ipa", wrong = data / "notes.txt";
    write_file(good, ipa.zip);
    write_file(wrong, bytes_of("not an ipa"));
    std::vector<std::string> asked;
    fe::ImportScreen screen(data.string(), [&](const std::string &path, const fe::IpaProgress &p) {
        asked.push_back(path);
        return fe::import_ipa(path, data.string(), ipa.expected, p);
    });
    constexpr std::uint32_t w = 960, h = 640;
    std::vector<std::uint8_t> px(std::size_t{w} * h * 4);
    REQUIRE(screen.dirty());
    screen.draw(px.data(), w, h, 2);
    const std::uint64_t waiting = fnv(px);
    CHECK_FALSE(screen.dirty());
    CHECK(screen.state() == fe::ImportScreen::State::kWaiting);

    // Enter, Space and the pad's South ask for the dialog; other keys do nothing.
    fe::ImportScreen::Action action{};
    CHECK(screen.handle(key_event(SDLK_RETURN), &action));
    CHECK(action == fe::ImportScreen::Action::kChoose);
    CHECK(screen.handle(key_event(SDLK_SPACE), &action));
    CHECK(action == fe::ImportScreen::Action::kChoose);
    CHECK(screen.handle(key_event(SDLK_A), &action));
    CHECK(action == fe::ImportScreen::Action::kNone);
    SDL_Event pad{};
    pad.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    pad.gbutton.button = SDL_GAMEPAD_BUTTON_SOUTH;
    CHECK(screen.handle(pad, &action));
    CHECK(action == fe::ImportScreen::Action::kChoose);
    // A click on the button: pressed and released on it.
    const fe::ScreenPoint on{fe::ImportScreen::kButtonX + 5.0f, fe::ImportScreen::kButtonY + 5.0f,
                             true};
    const fe::ScreenPoint off{300, 300, true};
    CHECK(screen.pointer(on, true, false) == fe::ImportScreen::Action::kNone);
    CHECK(screen.pointer(on, false, true) == fe::ImportScreen::Action::kChoose);
    CHECK(screen.pointer(on, true, false) == fe::ImportScreen::Action::kNone);
    CHECK(screen.pointer(off, false, true) == fe::ImportScreen::Action::kNone);

    // A dropped file that is not an IPA: refused, with its reason drawn.
    const std::string wrong_s = wrong.string(), good_s = good.string();
    SDL_Event drop = drop_event(wrong_s.c_str());
    CHECK(screen.handle(drop, &action));
    screen.wait();
    CHECK(screen.state() == fe::ImportScreen::State::kRefused);
    CHECK(screen.result().problem == IpaProblem::kNotZip);
    REQUIRE(screen.dirty());
    screen.draw(px.data(), w, h, 2);
    const std::uint64_t refused = fnv(px);
    CHECK(refused != waiting);
    CHECK_FALSE(fs::exists(data / "game"));

    // Retry: the right one, dropped.
    drop = drop_event(good_s.c_str());
    CHECK(screen.handle(drop, &action));
    CHECK(screen.state() == fe::ImportScreen::State::kImporting);
    CHECK_FALSE(screen.start(good_s)); // one at a time
    screen.draw(px.data(), w, h, 2);   // "Importing..." with its bar, whatever the progress
    screen.wait();
    CHECK(screen.state() == fe::ImportScreen::State::kImported);
    CHECK(screen.progress() == 1.0);
    CHECK(fe::has_imported_game(data.string()));
    screen.draw(px.data(), w, h, 2);
    CHECK(fnv(px) != refused);
    // Done: nothing more is taken.
    CHECK(screen.handle(key_event(SDLK_RETURN), &action));
    CHECK(action == fe::ImportScreen::Action::kNone);
    CHECK(asked == std::vector<std::string>{wrong_s, good_s});

    // The folder route (no dialog here): Choose imports <data dir>/MapleThief-1.5.ipa.
    fe::ImportScreen folder(data.string(), {}, true);
    CHECK(fs::path(folder.folder_file()) == data / "MapleThief-1.5.ipa");
    folder.draw(px.data(), w, h, 2);
}

TEST_CASE("import screen: the window loop imports a dropped IPA, and a close quits") {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    REQUIRE_MESSAGE(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD), SDL_GetError());
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    REQUIRE_MESSAGE(SDL_CreateWindowAndRenderer("ipa_import_test", 960, 640, 0, &window, &renderer),
                    SDL_GetError());
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    const fe::Settings settings;
    const Ipa ipa = make_ipa();
    const fs::path data = fresh_dir("screen_loop");
    const fs::path file = data / "dropped.ipa";
    write_file(file, ipa.zip);
    const fe::ImportScreen::Importer importer = [&](const std::string &path,
                                                    const fe::IpaProgress &p) {
        return fe::import_ipa(path, data.string(), ipa.expected, p);
    };
    bool quit = true;
    const std::string path = file.string();
    SDL_Event drop = drop_event(path.c_str());
    REQUIRE(SDL_PushEvent(&drop));
    const std::optional<std::string> dir =
        fe::run_import_screen(window, renderer, settings, data.string(), &quit, importer);
    CHECK_FALSE(quit);
    REQUIRE(dir.has_value());
    CHECK(fs::path(*dir) == data / "game");
    CHECK(fe::has_imported_game(data.string()));

    SDL_Event close{};
    close.type = SDL_EVENT_QUIT;
    REQUIRE(SDL_PushEvent(&close));
    CHECK_FALSE(fe::run_import_screen(window, renderer, settings, data.string(), &quit, importer)
                    .has_value());
    CHECK(quit);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}
