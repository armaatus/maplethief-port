// The save files on the host (#78, ADR 0074, maplethief/saves.h): atomic replacement, a save
// killed at each stage (the fault seam) and, on POSIX, a child process killed with SIGKILL while
// it saves; the read-time sanitiser; where Documents/ is. The game's own sequence through
// NSFileHandle is in foundation_test.cpp.
#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <system_error>

#include "maplethief/saves.h"
#include "maplethief/stubs.h"

#if !defined(_WIN32)
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;

fs::path unique_dir(const char *what) {
    return fs::temp_directory_path() /
           (std::string("maplethief-saves-test-") + what + "-" +
            std::to_string(std::random_device{}()) + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

std::string read_host(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

int replace(const fs::path &p, const std::string &bytes) {
    return mt_saves_replace_file(p.string().c_str(), bytes.data(), bytes.size());
}

fs::path tmp_of(const fs::path &p) { return fs::path(p.string() + MT_SAVES_TMP_SUFFIX); }

} // namespace

TEST_CASE("Saves: a replacement creates the directories and swaps the whole file") {
    const fs::path dir = unique_dir("replace");
    const fs::path file = dir / "a" / "b" / "theif0";
    CHECK(replace(file, "first") == 0);
    CHECK(read_host(file) == "first");
    CHECK(replace(file, "2nd") == 0); // shorter: the file is exactly the new bytes
    CHECK(read_host(file) == "2nd");
    CHECK(replace(file, "") == 0);
    CHECK(fs::exists(file));
    CHECK(fs::file_size(file) == 0u);
    CHECK_FALSE(fs::exists(tmp_of(file)));
    // Nowhere to write: an error, nothing changed.
    CHECK(mt_saves_replace_file("", "x", 1) == -1);
    CHECK(mt_saves_replace_file(nullptr, "x", 1) == -1);
    CHECK(mt_saves_replace_file((dir / "a").string().c_str(), "x", 1) == -1); // a directory
    CHECK(fs::is_directory(dir / "a"));
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("Saves: a replacement stopped at any stage leaves the old file whole") {
    const fs::path dir = unique_dir("faults");
    const fs::path file = dir / "quest0";
    const std::string old_bytes(4781, 'o'), new_bytes(4781, 'n');
    for (const mt_saves_fault fault :
         {MT_SAVES_FAULT_OPENED, MT_SAVES_FAULT_HALF_WRITTEN, MT_SAVES_FAULT_WRITTEN}) {
        CAPTURE(static_cast<int>(fault));
        REQUIRE(replace(file, old_bytes) == 0);
        mt_saves_set_fault(fault);
        CHECK(replace(file, new_bytes) == -1);
        mt_saves_set_fault(MT_SAVES_FAULT_NONE);
        CHECK(read_host(file) == old_bytes);
        // What the stopped process left behind, never read as the save.
        const std::string left = read_host(tmp_of(file));
        CHECK(left.size() == (fault == MT_SAVES_FAULT_OPENED         ? 0u
                              : fault == MT_SAVES_FAULT_HALF_WRITTEN ? new_bytes.size() / 2
                                                                     : new_bytes.size()));
        // The next save goes through and takes the leftover with it.
        CHECK(replace(file, new_bytes) == 0);
        CHECK(read_host(file) == new_bytes);
        CHECK_FALSE(fs::exists(tmp_of(file)));
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

#if !defined(_WIN32)
TEST_CASE("Saves: a process killed while it saves leaves one whole version") {
    // A child saves two versions in turn as fast as it can; it is killed with SIGKILL after a
    // random time, 40 times. Each time the file holds all of one version or all of the other.
    const fs::path dir = unique_dir("sigkill");
    const fs::path file = dir / "theif0";
    const std::string a(1 << 20, 'a'), b(1 << 20, 'b');
    REQUIRE(replace(file, a) == 0);
    std::mt19937 rng(78);
    int interrupted = 0;
    for (int run = 0; run < 40; run++) {
        const pid_t pid = fork();
        REQUIRE(pid >= 0);
        if (pid == 0) {
            for (unsigned i = 0;; i++) {
                const std::string &v = i % 2 == 0 ? b : a;
                mt_saves_replace_file(file.c_str(), v.data(), v.size());
            }
        }
        usleep(1000 + rng() % 30000);
        kill(pid, SIGKILL);
        int status = 0;
        waitpid(pid, &status, 0);
        CHECK(WIFSIGNALED(status));
        const std::string now = read_host(file);
        CHECK((now == a || now == b));
        interrupted += fs::exists(tmp_of(file)) ? 1 : 0;
    }
    MESSAGE(interrupted << " of 40 kills stopped a save part way (a temporary file was left)");
    std::error_code ec;
    fs::remove_all(dir, ec);
}
#endif

TEST_CASE("Saves: the sanitiser clamps config's speed and byte 5 only") {
    std::uint8_t ok[6] = {3, 1, 0, 1, 1, 1};
    CHECK(mt_saves_sanitise("config", ok, 6) == 0u);
    CHECK(ok[1] == 1);
    for (const std::uint8_t bad : {3, 4, 0x7F, 0x80, 0xFF}) {
        std::uint8_t c[6] = {0xFF, bad, 0xFF, 0xFF, 0xFF, bad};
        CHECK(mt_saves_sanitise("config", c, 6) == 2u);
        CHECK(c[1] == 2);
        CHECK(c[5] == 2);
        CHECK(c[0] == 0xFF); // sound: bits, any value works
        CHECK(c[3] == 0xFF);
    }
    std::uint8_t shortened[2] = {3, 9};
    CHECK(mt_saves_sanitise("config", shortened, 2) == 1u);
    CHECK(shortened[1] == 2);
    std::uint8_t other[6] = {9, 9, 9, 9, 9, 9};
    CHECK(mt_saves_sanitise("theif0", other, 6) == 0u);
    CHECK(mt_saves_sanitise("configx", other, 6) == 0u);
    CHECK(mt_saves_sanitise(nullptr, other, 6) == 0u);
    CHECK(other[1] == 9);
}

TEST_CASE("Saves: Documents is <data dir>/Documents unless set") {
    const fs::path data = unique_dir("data");
    REQUIRE(mt_stubs_set_data_dir(data.string().c_str()) == 0);
    CHECK(fs::path(mt_saves_documents_dir()) == data / "Documents");
    REQUIRE(mt_stubs_set_data_dir((data.string() + "/").c_str()) == 0); // SDL_GetPrefPath's '/'
    CHECK(fs::path(mt_saves_documents_dir()) == data / "Documents");
    REQUIRE(mt_saves_set_documents_dir("/var/mobile/Containers/Data/Application/X/Documents//") ==
            0);
    CHECK(std::string(mt_saves_documents_dir()) ==
          "/var/mobile/Containers/Data/Application/X/Documents");
    CHECK(mt_saves_set_documents_dir(std::string(2000, 'x').c_str()) == -1);
    REQUIRE(mt_saves_set_documents_dir("") == 0);
    CHECK(fs::path(mt_saves_documents_dir()) == data / "Documents");
    mt_stubs_set_data_dir(nullptr);
}

// --- the checks (#148, ADR 0078) -----------------------------------------------------------------
// Every rule on its own here; the same rules against tools/mtport/savecheck.py on savedata.py's
// fixtures: CTest saves.check_parity.

namespace {

// A new character's bytes (docs/formats/save.md, "A new character"): what the checks must pass.
std::string new_character_bytes() {
    std::string b(0x464, '\0');
    b[2] = 1;
    for (std::size_t i = 0; i < 48; i++) {
        b[0x015 + 16 * i] = '\xFF';
        b[0x325 + 3 * i] = '\xFF';
    }
    b[0x325] = 3, b[0x326] = 5, b[0x328] = 6, b[0x329] = 5;
    for (std::size_t i = 0; i < 5; i++) {
        b[0x3C2 + 16 * i] = '\xFF';
    }
    b[0x3C2 + 16] = 0x48;
    b[0x41C] = 2, b[0x41D] = 8;
    return b;
}

mt_saves_verdict check(const char *name, const std::string &b, std::string *why = nullptr) {
    char text[256];
    const mt_saves_verdict v = mt_saves_check(
        name, reinterpret_cast<const std::uint8_t *>(b.data()), b.size(), text, sizeof text);
    if (why != nullptr) {
        *why = text;
    }
    return v;
}

std::string bytes(std::initializer_list<int> list) {
    std::string b;
    for (const int v : list) {
        b.push_back(static_cast<char>(v));
    }
    return b;
}

} // namespace

TEST_CASE("Saves: the checks pass the game's own files and name the rule a file breaks") {
    const std::string hero = new_character_bytes();
    std::string why;
    CHECK(check("theif0", hero, &why) == MT_SAVES_OK);
    CHECK(why.empty());
    std::string quests(0x12AD, '\0');
    for (std::size_t r = 0; r < 10; r++) {
        quests[0xC9 + 0x1C9 * r] = '\xFF';
    }
    CHECK(check("quest2", quests) == MT_SAVES_OK);
    CHECK(check("config", bytes({3, 1, 0, 1, 1, 1})) == MT_SAVES_OK);
    CHECK(check("config", bytes({0xFF, 2, 9, 9, 9, 2})) == MT_SAVES_OK); // sound: bits; 2-4 free
    CHECK(check("cupon", std::string(0x174A, '\0')) == MT_SAVES_OK);
    CHECK(check("CTerms", std::string(2, '\0')) == MT_SAVES_OK);
    CHECK(check("crestore", bytes({0, 0})) == MT_SAVES_OK);

    CHECK(check("theif3", hero) == MT_SAVES_BAD_NAME);
    CHECK(check("thief0", hero) == MT_SAVES_BAD_NAME); // the game's spelling is "theif"
    CHECK(check("theif0", hero.substr(0, 1000), &why) == MT_SAVES_BAD_SIZE);
    CHECK(why == "theif0 has 1000 bytes; a character file has exactly 1124 (it is cut short)");
    CHECK(check("theif0", hero + "x") == MT_SAVES_BAD_SIZE);
    const auto poked = [&](std::size_t at, int v) {
        std::string b = hero;
        b[at] = static_cast<char>(v);
        return b;
    };
    CHECK(check("theif0", poked(0x000, 3)) == MT_SAVES_BAD_JOB);
    CHECK(check("theif0", poked(0x001, 5)) == MT_SAVES_BAD_JOB);
    CHECK(check("theif0", poked(0x002, 0)) == MT_SAVES_BAD_LEVEL);
    CHECK(check("theif0", poked(0x002, 101)) == MT_SAVES_BAD_LEVEL);
    CHECK(check("theif0", poked(0x002, 100)) == MT_SAVES_OK);
    CHECK(check("theif0", poked(0x015 + 16 * 47, 74), &why) == MT_SAVES_BAD_ITEM);
    CHECK(why == "theif0: equipment inventory slot 47 holds equipment 74; the game has "
                 "equipment 0-73");
    CHECK(check("theif0", poked(0x015, 73)) == MT_SAVES_OK);
    CHECK(check("theif0", poked(0x325 + 3 * 9, 138)) == MT_SAVES_BAD_ITEM);
    CHECK(check("theif0", poked(0x3B5 + 12, 21)) == MT_SAVES_BAD_SKILL);
    CHECK(check("theif0", poked(0x3B5 + 12, 20)) == MT_SAVES_OK);
    CHECK(check("theif0", poked(0x3C2 + 16 * 4, 100)) == MT_SAVES_BAD_ITEM);
    CHECK(check("theif0", poked(0x41C, 11)) == MT_SAVES_BAD_MAP);
    CHECK(check("theif0", poked(0x41D, 12)) == MT_SAVES_BAD_MAP); // area 2 has maps 0-11
    CHECK(check("theif0", poked(0x41D, 11)) == MT_SAVES_OK);

    const std::size_t rec = 0xC9;
    std::string quest = quests;
    for (std::size_t k = 0; k < 5; k++) {
        quest[rec + 0x143 + 14 * k] = '\xFF';
        quest[rec + 0x189 + 13 * k] = '\xFF';
    }
    quest[rec] = 0; // an active record with empty keys and both lists empty
    CHECK(check("quest0", quest) == MT_SAVES_OK);
    std::string q = quest;
    q[rec + 0x003] = '\x9C';
    CHECK(check("quest0", q, &why) == MT_SAVES_BAD_QUEST);
    CHECK(why == "quest0: quest 0's title is not a text key (byte 0x9C at 0)");
    q = quest;
    q.replace(rec + 0x003, 18, "\xE3\x85\x8A\xE3\x84\xB9\xE3\x85\x8D.dialog.1");
    CHECK(check("quest0", q) == MT_SAVES_OK); // the Korean Scripts.strings key ㅊㄹㅍ.dialog.1
    q[rec + 0x003 + 2] = 0;                   // ㅊ cut by the NUL
    CHECK(check("quest0", q, &why) == MT_SAVES_BAD_QUEST);
    CHECK(why == "quest0: quest 0's title is not a text key (byte 0xE3 at 0)");
    for (const char *bad : {"\xC0\x80", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\x01", "\x7F"}) {
        q = quest;
        q.replace(rec + 0x003, std::strlen(bad), bad);
        CHECK(check("quest0", q) == MT_SAVES_BAD_QUEST);
    }
    q = quest;
    q.replace(rec + 0x043, 256, std::string(256, 'A'));
    CHECK(check("quest0", q) == MT_SAVES_BAD_QUEST);
    q = quest;
    q[rec + 0x143] = 3; // requirement 0 of kind 3
    CHECK(check("quest0", q) == MT_SAVES_BAD_QUEST);
    q[rec + 0x143] = 0, q[rec + 0x144] = 1, q[rec + 0x145] = static_cast<char>(138);
    CHECK(check("quest0", q) == MT_SAVES_BAD_ITEM);
    q[rec + 0x144] = 2;
    CHECK(check("quest0", q) == MT_SAVES_BAD_QUEST);
    q = quest;
    q[rec + 0x189] = 0, q[rec + 0x18B] = 74; // reward 0: equipment 74
    CHECK(check("quest0", q) == MT_SAVES_BAD_ITEM);
    q = quests;
    q[rec + 0x1C9 + 0x003] = '\x9C'; // after the first empty record: never read
    CHECK(check("quest0", q) == MT_SAVES_OK);

    CHECK(check("config", bytes({3, 3, 0, 1, 1, 1}), &why) == MT_SAVES_BAD_CONFIG);
    CHECK(why.find("game speed is 3") != std::string::npos);
    CHECK(check("config", bytes({3, 2, 0, 1, 1, 3})) == MT_SAVES_BAD_CONFIG);
    CHECK(check("config", bytes({3, 1, 0})) == MT_SAVES_BAD_SIZE);
    CHECK(check("totalrestore", bytes({1})) == MT_SAVES_BAD_SIZE);
    CHECK(check("totalrestore", bytes({0xFF, 0xFF})) == MT_SAVES_BAD_RECEIPTS);
    CHECK(check("totalrestore", bytes({1, 0, 0, 5, 0, 'a', 'b', 'c'})) == MT_SAVES_BAD_RECEIPTS);
    CHECK(check("totalrestore", bytes({1, 0, 0, 3, 0, 'a', 'b', 'c', 'o', 'l', 'd'})) ==
          MT_SAVES_OK); // an old tail after the last entry
    CHECK(check("riprestore1", std::string(MT_SAVES_MAX_RECEIPTS_BYTES + 1, '\0')) ==
          MT_SAVES_BAD_SIZE);
}

TEST_CASE("Saves: the checks' names, the reason cut to fit, the file names, the counters") {
    CHECK(std::string(mt_saves_verdict_name(MT_SAVES_OK)) == "ok");
    CHECK(std::string(mt_saves_verdict_name(MT_SAVES_BAD_RECEIPTS)) == "bad_receipts");
    CHECK(std::string(mt_saves_verdict_name(MT_SAVES_BAD_COUNTERS)) == "bad_counters");
    CHECK(std::string(mt_saves_verdict_name(static_cast<mt_saves_verdict>(99))) == "?");
    const std::string hero = new_character_bytes();
    const auto *p = reinterpret_cast<const std::uint8_t *>(hero.data());
    char small[8];
    CHECK(mt_saves_check("theif0", p, 5, small, sizeof small) == MT_SAVES_BAD_SIZE);
    CHECK(std::string(small) == "theif0 ");
    CHECK(mt_saves_check("theif0", p, 5, nullptr, 0) == MT_SAVES_BAD_SIZE);
    CHECK(mt_saves_check(nullptr, nullptr, 0, nullptr, 0) == MT_SAVES_BAD_NAME);
    for (const char *name : {"theif0", "theif1", "theif2", "quest0", "quest1", "quest2", "config",
                             "cupon", "CTerms", "crestore", "riprestore1", "riprestore2",
                             "totalrestore", "maplethief-cashshop0", "maplethief-cashshop2"}) {
        CHECK(mt_saves_is_game_file(name) == 1);
    }
    for (const char *name :
         {"theif3", "quest", "Config", "theif0.mt-tmp", "", "settings.ini", "NoCharge1",
          "maplethief-cashshop3", "maplethief-cashshop", "maplethief-cashshop00"}) {
        CHECK(mt_saves_is_game_file(name) == 0);
    }
    // The cash shop's counters (#156): "key = number" text up to 1 KiB.
    const auto counters = [](const std::string &text, std::string *why = nullptr) {
        char buf[160] = {};
        const mt_saves_verdict v = mt_saves_check(
            "maplethief-cashshop1", reinterpret_cast<const std::uint8_t *>(text.data()),
            text.size(), buf, sizeof buf);
        if (why != nullptr) {
            *why = buf;
        }
        return v;
    };
    std::string why;
    CHECK(counters("# comment\nlevel = 7\r\n\n  mesopack3=4294967295  \n") == MT_SAVES_OK);
    CHECK(counters("") == MT_SAVES_OK);
    CHECK(counters("level = 7\x01\n", &why) == MT_SAVES_BAD_COUNTERS);
    CHECK(why == "maplethief-cashshop1 is not text (byte 0x01 at 9)");
    CHECK(counters("# ok\nlevel 7\n", &why) == MT_SAVES_BAD_COUNTERS);
    CHECK(why == "maplethief-cashshop1: line 2 is not \"key = number\"");
    CHECK(counters("deaths = 4294967296") == MT_SAVES_BAD_COUNTERS);
    CHECK(counters("deaths = -1") == MT_SAVES_BAD_COUNTERS);
    CHECK(counters("= 1") == MT_SAVES_BAD_COUNTERS);
    CHECK(counters(std::string(MT_SAVES_MAX_COUNTERS_BYTES, '#')) == MT_SAVES_OK);
    CHECK(counters(std::string(MT_SAVES_MAX_COUNTERS_BYTES + 1, '#')) == MT_SAVES_BAD_SIZE);
    CHECK(mt_saves_maps_in_area(4) == 19u);
    CHECK(mt_saves_maps_in_area(MT_SAVES_AREAS) == 0u);
}
