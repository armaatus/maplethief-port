// Text packs (#189, ADR 0108, maplethief/textpack.h) on synthetic packs written at run time: the
// .strings rules (tools/mtport/textpack.py's tests check the same cases), the manifest, a table
// row written into a synthetic table, a missing key falling back, invalid UTF-8 refused with its
// reason, the Korean encodings with a pack open, and the Text_Wrap hooks' steps. The pack
// through NSBundle and CFURL is in foundation_test.cpp.
#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "cf/cf_objects.h"
#include "dir_state.h"
#include "maplethief/diag.h"
#include "maplethief/hooks.h"
#include "maplethief/textpack.h"
#include "textpack/strings_file.h"
#include "textpack/textpack_internal.h"

namespace {

namespace fs = std::filesystem;
using mt_textpack::StringsFile;

std::vector<std::string> g_log;
void capture(void *, const char *line) { g_log.emplace_back(line); }
bool logged(const std::string &text) {
    for (const auto &l : g_log) {
        if (l.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool parse(const std::string &text, StringsFile *out, std::string *why) {
    return mt_textpack::parse_strings(reinterpret_cast<const std::uint8_t *>(text.data()),
                                      text.size(), out, why);
}

// The reason `text` is refused ("" if it is not).
std::string refusal(const std::string &text) {
    StringsFile f;
    std::string why;
    return parse(text, &f, &why) ? std::string() : why;
}

// A pack directory of its own, removed afterwards; the log captured.
struct PackDir {
    fs::path dir = fs::temp_directory_path() /
                   ("maplethief-textpack-test-" + std::to_string(std::random_device{}()) + "-" +
                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    PackDir() {
        fs::create_directories(dir);
        g_log.clear();
        mt_diag_config cfg = mt_diag_config_default();
        cfg.log = capture;
        mt_diag_configure(&cfg);
    }
    ~PackDir() {
        mt_textpack_close();
        mt_diag_reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    PackDir(const PackDir &) = delete;
    PackDir &operator=(const PackDir &) = delete;
    void write(const char *name, const std::string &bytes) const {
        std::ofstream(dir / name, std::ios::binary) << bytes;
    }
    std::string path() const { return dir.string(); }
};

const char *k_manifest = "\"format\" = \"1\";\n\"name\" = \"Test\";\n\"language\" = \"de\";\n"
                         "\"version\" = \"0.1\";\n\"game_version\" = \"1.5\";\n";

// A synthetic item_etc.dat (14 columns, 218 bytes a row) with `rows` rows named "Potion <n>".
std::vector<std::uint8_t> item_etc(std::uint16_t rows) {
    std::vector<std::uint8_t> b(6 + static_cast<std::size_t>(rows) * 218, 0);
    const std::uint16_t header[3] = {14, rows, 218};
    std::memcpy(b.data(), header, 6);
    for (std::uint16_t r = 0; r < rows; r++) {
        const std::string name = "Potion " + std::to_string(r);
        b[6 + r * 218u] = static_cast<std::uint8_t>(r); // the key
        std::memcpy(b.data() + 6 + r * 218u + 1, name.data(), name.size());
        std::memcpy(b.data() + 6 + r * 218u + 90, "desc", 4);
        b[6 + r * 218u + 65] = 0x05; // the type column, after the name's 64 bytes
    }
    return b;
}

std::string field(const std::vector<std::uint8_t> &b, std::uint32_t row, std::uint32_t offset,
                  std::uint32_t width) {
    const char *p = reinterpret_cast<const char *>(b.data() + 6 + row * 218u + offset);
    return std::string(p, strnlen(p, width));
}

std::u16string decode(const std::string &bytes, std::uint32_t encoding, bool *ok) {
    std::u16string out;
    *ok = mt_cf::decode(reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size(),
                        encoding, out);
    return out;
}

} // namespace

TEST_CASE("textpack: the .strings syntax: entries, comments, escapes, a byte order mark") {
    StringsFile f;
    std::string why;
    const std::string text = "\xEF\xBB\xBF/* a comment\n over lines */\n"
                             "\"a\" = \"P\xC3\xB6tion\"; // the rest of the line\n"
                             "\"b.c.1\"\t=\t\"line 1\\nline 2 \\\"q\\\" \\\\ \\t\\r\\'\";\n"
                             "\"u\" = \"\\U00e9\\u00E9 \\UD83D\\UDE00\";\n"
                             "\"multi\" = \"two\nlines\";\n"
                             "\"a\" = \"again\";\n";
    REQUIRE_MESSAGE(parse(text, &f, &why), why);
    CHECK(f.entries.size() == 4);
    CHECK(f.entries["a"] == "again");
    CHECK(f.duplicates == 1);
    CHECK(f.entries["b.c.1"] == "line 1\nline 2 \"q\" \\ \t\r'");
    CHECK(f.entries["u"] == "\xC3\xA9\xC3\xA9 \xF0\x9F\x98\x80");
    CHECK(f.entries["multi"] == "two\nlines");
    REQUIRE(parse("", &f, &why));
    CHECK(f.entries.empty());
    REQUIRE(parse("  // only a comment", &f, &why));
}

TEST_CASE("textpack: a .strings file that breaks a rule is refused with its line and reason") {
    CHECK(refusal("\"a\" = \"x\";\n\"b\" = \"P\xC3\";\n") ==
          "line 2: invalid UTF-8 (byte 0xC3 at offset 19)");
    CHECK(refusal("\"a\" = \"\xC0\xAF\";").find("invalid UTF-8") != std::string::npos); // overlong
    CHECK(refusal("\"a\" = \"\xED\xA0\x80\";").find("invalid UTF-8") != std::string::npos);
    CHECK(refusal("\"a\" = \"\xF4\x90\x80\x80\";").find("invalid UTF-8") != std::string::npos);
    CHECK(refusal("\"a\" = \"\xFF\";").find("invalid UTF-8") != std::string::npos);
    CHECK(refusal("\"a\" = \"x\"") == "line 1: expected ';' after the value");
    CHECK(refusal("\"a\" \"x\";") == "line 1: expected '=' after the key");
    CHECK(refusal("a = \"x\";") == "line 1: expected a quoted key");
    CHECK(refusal("\"a\" = x;") == "line 1: expected a quoted value after '='");
    CHECK(refusal("\n\"a\" = \"x;\n") == "line 2: a string without its closing quote");
    CHECK(refusal("/* open\n") == "line 1: a comment without its end (*/)");
    CHECK(refusal("\"a\" = \"\\q\";") == "line 1: an unknown escape \\q");
    CHECK(refusal("\"a\" = \"\\U12\";") == "line 1: \\U needs four hex digits");
    CHECK(refusal("\"a\" = \"\\UD800\";") == "line 1: a lone surrogate (\\UD800)");
    CHECK(refusal("\"a\" = \"\\UDC00\";") == "line 1: a lone surrogate (\\UDC00)");
    CHECK(refusal("\"a\" = \"\\U0000\";") == "line 1: a NUL character");
    CHECK(refusal(std::string("\"a\" = \"x\0y\";", 12)) == "line 1: a NUL character");
    CHECK(refusal("\"\" = \"x\";") == "line 1: an empty key");
    CHECK(refusal("\"" + std::string(MT_TEXTPACK_MAX_KEY + 1, 'k') + "\" = \"x\";") ==
          "line 1: a key longer than 256 bytes");
    CHECK(refusal("\"a\" = \"" + std::string(MT_TEXTPACK_MAX_VALUE + 1, 'v') + "\";") ==
          "line 1: a value longer than 4096 bytes");
    CHECK(refusal(std::string(MT_TEXTPACK_MAX_FILE + 1, ' ')) == "larger than 4 MiB");
    std::string many;
    for (std::uint32_t i = 0; i <= MT_TEXTPACK_MAX_ENTRIES; i++) {
        many += "\"" + std::to_string(i) + "\"=\"\";";
    }
    CHECK(refusal(many) == "line 1: more than 65536 entries");
}

TEST_CASE("textpack: UTF-8 helpers: lengths, a cut at a whole character, the wrap's steps") {
    CHECK(mt_textpack::utf8_length('a') == 1);
    CHECK(mt_textpack::utf8_length(0xC3) == 2);
    CHECK(mt_textpack::utf8_length(0xEA) == 3);
    CHECK(mt_textpack::utf8_length(0xF0) == 4);
    CHECK(mt_textpack::utf8_length(0xB6) == 1); // a continuation byte alone
    CHECK(mt_textpack::utf8_cut("P\xC3\xB6tion", 2) == "P");
    CHECK(mt_textpack::utf8_cut("P\xC3\xB6tion", 3) == "P\xC3\xB6");
    CHECK(mt_textpack::utf8_cut("abc", 9) == "abc");
    // The game's own step is 3 bytes for any byte >= 0x80: "Hénésys" would be 5 characters and
    // its cuts would fall inside "é". The hooks count 7 and cut whole characters.
    const std::string henesys = "H\xC3\xA9n\xC3\xA9sys";
    CHECK(mt_textpack::char_count(henesys) == 7);
    CHECK(mt_textpack::sub_chars(henesys, 0, 2) == "H\xC3\xA9");
    CHECK(mt_textpack::sub_chars(henesys, 1, 4) == "\xC3\xA9n\xC3\xA9");
    CHECK(mt_textpack::sub_chars(henesys, 5, 99) == "ys");
    CHECK(mt_textpack::sub_chars(henesys, 9, 12).empty());
    // Hangul in UTF-8 (3 bytes) and ASCII step as the game's own.
    const std::string hangul = "\xEB\xB8\x8C\xEB\xA3\xA8 ab";
    CHECK(mt_textpack::char_count(hangul) == 5);
    CHECK(mt_textpack::sub_chars(hangul, 1, 3) == "\xEB\xA3\xA8 ");
}

TEST_CASE("textpack: a pack overrides a table row and a script string; missing keys fall back") {
    PackDir p;
    p.write("textpack.strings", k_manifest);
    p.write("Scripts.strings", "\"Chapter1.dialog.1\" = \"Willkommen in H\xC3\xA9n\xC3\xA9sys!\";");
    p.write("Tables.strings", "\"item_etc.1.name\" = \"P\xC3\xB6tion\";\n"
                              "\"item_etc.2.description\" = \"" +
                                  std::string(140, 'd') +
                                  "\";\n"
                                  "\"item_etc.3.name\" = \"" +
                                  std::string(63, 'x') +
                                  "\xC3\xB6\";\n"
                                  "\"item_etc.0.name\" = \"" +
                                  std::string(64, 'f') +
                                  "\";\n"
                                  "\"item_etc.99.name\" = \"past the rows\";\n"
                                  "\"item_etc.1.price\" = \"not text\";\n"
                                  "\"potion\" = \"no key\";\n");
    REQUIRE(mt_textpack_open(p.path().c_str()) == 8);
    CHECK(mt_textpack_is_open() == 1);
    CHECK(std::string(mt_textpack_manifest("name")) == "Test");
    CHECK(std::string(mt_textpack_manifest("language")) == "de");
    CHECK(mt_textpack_manifest("author") == nullptr);
    CHECK(logged("textpack: " + p.path() + ": \"Test\" (de, version 0.1, for the game's 1.5)"));

    // The script string; a key the pack lacks, a table it lacks: the game's own (NULL).
    CHECK(std::string(mt_textpack_lookup("Scripts", "Chapter1.dialog.1")) ==
          "Willkommen in H\xC3\xA9n\xC3\xA9sys!");
    CHECK(mt_textpack_lookup("Scripts", "Chapter1.dialog.2") == nullptr);
    CHECK(mt_textpack_lookup("ScriptExtra", "Chapter1.dialog.1") == nullptr);
    CHECK(mt_textpack_lookup("textpack", "name") == nullptr);

    // The table row; the rows the pack does not name keep the game's text.
    std::vector<std::uint8_t> table = item_etc(4);
    const std::vector<std::uint8_t> before = table;
    CHECK(mt_textpack::patch_table("English/data/item_etc.dat", table) == 4);
    CHECK(field(table, 1, 1, 64) == "P\xC3\xB6tion");
    CHECK(field(table, 1, 90, 128) == "desc");
    CHECK(table[6 + 218 + 0] == 1); // the key column untouched
    // A value that fills its field whole, as the game's own longest names do: no NUL needed
    // (0x37999 copies the field into a buffer one longer and ends it), the next column kept.
    CHECK(field(table, 0, 1, 64) == std::string(64, 'f'));
    CHECK(table[6 + 0 * 218 + 1 + 64] == 0x05); // item_etc's type column of row 0
    // Longer than the field: cut, logged. "ö" past 64 bytes is not split.
    CHECK(field(table, 2, 90, 128) == std::string(128, 'd'));
    CHECK(field(table, 3, 1, 64) == std::string(63, 'x'));
    CHECK(logged("\"item_etc.3.name\" is 65 bytes, its field holds 64: cut to"));
    CHECK(logged("textpack: overflow: Tables.strings \"item_etc.2.description\" is 140 bytes, "
                 "its field holds 128"));
    CHECK(logged("\"item_etc.99.name\": item_etc has 4 rows: ignored"));
    CHECK(logged("\"item_etc.1.price\": no table text column item_etc.price: ignored"));
    CHECK(logged("\"potion\": not <table>.<row>.<column>: ignored"));
    // Only the language tables, and only with the game's layout.
    std::vector<std::uint8_t> other = before;
    CHECK(mt_textpack::patch_table("png/item_etc.dat", other) == 0);
    CHECK(mt_textpack::patch_table("English/data/mob.dat", other) == 0);
    std::vector<std::uint8_t> bad = before;
    bad[4] = 200; // stride
    CHECK(mt_textpack::patch_table("Korean/data/item_etc.dat", bad) == 0);
    CHECK(logged("is not the table the game reads"));

    const mt_textpack_stats s = mt_textpack_get_stats();
    CHECK(s.files == 3);
    CHECK(s.refused == 0);
    CHECK(s.entries == 8);
    CHECK(s.unknown == 2);
    CHECK(s.fields == 4);
    CHECK(s.overflows == 2);
    CHECK(s.lookups == 4);
    CHECK(s.replaced == 1);

    // Closed: nothing is replaced.
    mt_textpack_close();
    CHECK(mt_textpack_is_open() == 0);
    CHECK(mt_textpack_lookup("Scripts", "Chapter1.dialog.1") == nullptr);
    std::vector<std::uint8_t> again = before;
    CHECK(mt_textpack::patch_table("English/data/item_etc.dat", again) == 0);
    CHECK(again == before);
}

TEST_CASE("textpack: invalid UTF-8 is refused with a reason; the game's text stays") {
    PackDir p;
    p.write("textpack.strings", k_manifest);
    p.write("SCRIPTS.STRINGS", "\"k\" = \"ok\";\n\"Chapter1.dialog.1\" = \"H\xE9n\xE9sys\";\n");
    p.write("Port.strings", "\"settings.title\" = \"EINSTELLUNGEN\";");
    REQUIRE(mt_textpack_open(p.path().c_str()) == 1);
    CHECK(logged("SCRIPTS.STRINGS: line 2: invalid UTF-8 (byte 0xE9 at offset 36): refused"));
    CHECK(mt_textpack_lookup("Scripts", "k") == nullptr); // the whole file
    CHECK(mt_textpack_lookup("Scripts", "Chapter1.dialog.1") == nullptr);
    CHECK(std::string(mt_textpack_lookup("Port", "settings.title")) == "EINSTELLUNGEN");
    CHECK(mt_textpack_get_stats().refused == 1);
}

TEST_CASE("textpack: a pack without a good manifest is not opened") {
    {
        PackDir p;
        p.write("Scripts.strings", "\"k\" = \"v\";");
        CHECK(mt_textpack_open(p.path().c_str()) == -1);
        CHECK(logged("has no textpack.strings (the manifest): no text pack"));
        CHECK(mt_textpack_is_open() == 0);
    }
    const char *bad[][2] = {
        {"\"format\" = \"2\"; \"name\" = \"x\"; \"language\" = \"de\";",
         "its \"format\" is not \"1\""},
        {"\"format\" = \"1\"; \"language\" = \"de\";", "it has no \"name\""},
        {"\"format\" = \"1\"; \"name\" = \"x\"; \"language\" = \"de de\";",
         "its \"language\" is not a language tag"},
        {"\"format\" = \"1\"; \"name\" = \"x\" \"language\" = \"de\";", "the manifest was refused"},
    };
    for (const auto &b : bad) {
        PackDir p;
        p.write("textpack.strings", b[0]);
        CHECK(mt_textpack_open(p.path().c_str()) == -1);
        CHECK_MESSAGE(logged(b[1]), b[1]);
        CHECK(mt_textpack_is_open() == 0);
    }
    PackDir p;
    CHECK(mt_textpack_open((p.path() + "/missing").c_str()) == -1);
    CHECK(logged("is no directory"));
    CHECK(mt_textpack_open("") == -1);
}

TEST_CASE("textpack: with a pack the Korean encodings read and write UTF-8; without, EUC-KR") {
    const std::string potion = "P\xC3\xB6tion"; // UTF-8
    const std::string euc = "\xBC\xD2\xC8\xAF"; // EUC-KR 소환
    const std::string arrow = "\xA1\xE7";       // EUC-KR ←
    bool ok = false;
    // Without a pack: "ö" (C3 B6) is the Hangul syllable 철 in EUC-KR.
    CHECK(decode(potion, mt_cf::k_enc_euc_kr, &ok) == u"P\uCCA0tion");
    CHECK(ok);
    PackDir p;
    p.write("textpack.strings", k_manifest);
    REQUIRE(mt_textpack_open(p.path().c_str()) == 0);
    CHECK(mt_cf::korean_utf8());
    CHECK(decode(potion, mt_cf::k_enc_euc_kr, &ok) == u"P\u00F6tion");
    CHECK(decode(potion, mt_cf::k_enc_mac_korean, &ok) == u"P\u00F6tion");
    CHECK(decode(euc, mt_cf::k_enc_euc_kr, &ok) == u"\uC18C\uD658"); // not UTF-8: EUC-KR
    CHECK(decode(arrow, mt_cf::k_enc_euc_kr, &ok) == u"\u2190");
    CHECK(decode("plain", mt_cf::k_enc_euc_kr, &ok) == u"plain");
    std::string out;
    const std::u16string s = u"P\u00F6tion \u2190";
    REQUIRE(mt_cf::encode(s.data(), s.size(), mt_cf::k_enc_euc_kr, out));
    CHECK(out == "P\xC3\xB6tion \xE2\x86\x90");
    // The Text_Wrap hooks are registered while the pack is open.
    CHECK(mt_hook_enabled(MT_HOOK_FN_TEXT_CHAR_COUNT) == 1);
    CHECK(mt_hook_enabled(MT_HOOK_FN_TEXT_SUB_CHARS) == 1);
    mt_textpack_close();
    CHECK_FALSE(mt_cf::korean_utf8());
    CHECK(mt_hook_enabled(MT_HOOK_FN_TEXT_CHAR_COUNT) == 0);
    out.clear();
    CHECK_FALSE(mt_cf::encode(s.data(), s.size(), mt_cf::k_enc_euc_kr, out)); // no ö in KS X 1001
    CHECK(decode(potion, mt_cf::k_enc_euc_kr, &ok) == u"P\uCCA0tion");
}

TEST_CASE("textpack: a text wider than its texture is logged once while a pack is open") {
    PackDir p;
    mt_textpack::note_overflow("long", 600, 512);
    CHECK_FALSE(logged("overflow"));
    p.write("textpack.strings", k_manifest);
    REQUIRE(mt_textpack_open(p.path().c_str()) == 0);
    mt_textpack::note_overflow("fits", 500, 512);
    mt_textpack::note_overflow("long", 600, 512);
    mt_textpack::note_overflow("long", 600, 512);
    // Several lines are measured as one: not judged (the game's own speech boxes).
    mt_textpack::note_overflow("two\nlines", 900, 512);
    CHECK(logged("textpack: overflow: \"long\" is 600 points wide, its texture 512: cut"));
    CHECK_FALSE(logged("\"fits\""));
    CHECK_FALSE(logged("two"));
    CHECK(mt_textpack_get_stats().overflows == 1);
}

TEST_CASE("textpack: the draw observer sees the drawn texts while a pack is open") {
    PackDir p;
    static std::vector<std::string> seen;
    seen.clear();
    mt_textpack_set_draw_observer([](const char *t, void *) { seen.emplace_back(t); }, nullptr);
    mt_textpack::note_drawn("before");
    p.write("textpack.strings", k_manifest);
    REQUIRE(mt_textpack_open(p.path().c_str()) == 0);
    mt_textpack::note_drawn("P\xC3\xB6tion");
    mt_textpack_set_draw_observer(nullptr, nullptr);
    mt_textpack::note_drawn("after");
    CHECK(seen == std::vector<std::string>{"P\xC3\xB6tion"});
}

TEST_CASE("textpack: a symbolic link in a pack is not followed, logged once (#413)") {
    PackDir outside;
    outside.write("far.strings", "\"k\" = \"from outside\";");
    outside.write("manifest.strings", k_manifest);
    std::error_code ec;
    {
        PackDir p;
        p.write("textpack.strings", k_manifest);
        p.write("Port.strings", "\"settings.title\" = \"EINSTELLUNGEN\";");
        fs::create_symlink(outside.dir / "far.strings", p.dir / "Scripts.strings", ec);
        if (ec) {
            MESSAGE("no symbolic link here (" << ec.message() << "): skipped");
            return;
        }
        REQUIRE(mt_textpack_open(p.path().c_str()) == 1); // Port's entry only
        CHECK(mt_textpack_lookup("Scripts", "k") == nullptr);
        CHECK(std::string(mt_textpack_lookup("Port", "settings.title")) == "EINSTELLUNGEN");
        int lines = 0;
        for (const auto &l : g_log) {
            lines += l.find("symbolic link") != std::string::npos ? 1 : 0;
        }
        CHECK(lines == 1);
        CHECK(logged("Scripts.strings"));
    }
    // A manifest that is a link: no manifest, no pack.
    PackDir p;
    fs::create_symlink(outside.dir / "manifest.strings", p.dir / "textpack.strings", ec);
    REQUIRE(!ec);
    CHECK(mt_textpack_open(p.path().c_str()) == -1);
    CHECK(logged("has no textpack.strings (the manifest): no text pack"));
    CHECK(mt_textpack_is_open() == 0);
}

TEST_CASE("textpack: a file over 4 MiB is refused before it is read (#413)") {
    PackDir p;
    p.write("textpack.strings", k_manifest);
    p.write("Scripts.strings", "\"k\" = \"v\";");
    fs::resize_file(p.dir / "Scripts.strings", std::uintmax_t{MT_TEXTPACK_MAX_FILE} + 1); // sparse
    const std::vector<std::string> before = dir_state(p.dir);
    // Unreadable as well, where the system has permissions: only the size can refuse it then.
    std::error_code ec;
    fs::permissions(p.dir / "Scripts.strings", fs::perms::none, ec);
    REQUIRE(mt_textpack_open(p.path().c_str()) == 0);
    fs::permissions(p.dir / "Scripts.strings", fs::perms::owner_all, ec);
    CHECK(logged("Scripts.strings: larger than 4 MiB: refused"));
    CHECK(mt_textpack_lookup("Scripts", "k") == nullptr);
    CHECK(mt_textpack_get_stats().refused == 1);
    CHECK(dir_state(p.dir) == before); // nothing moved, renamed or written
}

TEST_CASE("textpack: the manifest's language is 1 to 35 characters (#413)") {
    const auto manifest = [](const std::string &language) {
        return "\"format\" = \"1\";\n\"name\" = \"x\";\n\"language\" = \"" + language + "\";\n";
    };
    {
        PackDir p;
        p.write("textpack.strings", manifest("de-" + std::string(32, 'x')));
        CHECK(mt_textpack_open(p.path().c_str()) == 0);
        CHECK(mt_textpack_is_open() == 1);
    }
    for (const std::string &bad : {"de-" + std::string(33, 'x'), std::string()}) {
        PackDir p;
        p.write("textpack.strings", manifest(bad));
        CHECK(mt_textpack_open(p.path().c_str()) == -1);
        CHECK(logged("its \"language\" is not a language tag"));
        CHECK(mt_textpack_is_open() == 0);
    }
}

TEST_CASE("textpack: a Tables key's row is at most 5 digits (#413)") {
    PackDir p;
    p.write("textpack.strings", k_manifest);
    p.write("Tables.strings", "\"item_etc.99999.name\" = \"five\";\n"
                              "\"item_etc.100000.name\" = \"six\";\n"
                              "\"item_etc.000001.name\" = \"six too\";\n"
                              "\"item_etc..name\" = \"none\";\n"
                              "\"item_etc.-1.name\" = \"a sign\";\n");
    REQUIRE(mt_textpack_open(p.path().c_str()) == 5);
    CHECK(mt_textpack_get_stats().unknown == 4);
    CHECK(logged("\"item_etc.100000.name\": the row is not a number: ignored"));
    CHECK(logged("\"item_etc.000001.name\": the row is not a number: ignored"));
    CHECK(!logged("\"item_etc.99999.name\""));
}
