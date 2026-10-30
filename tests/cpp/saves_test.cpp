// The save files on the host (#78, ADR 0074, maplethief/saves.h): atomic replacement, a save
// killed at each stage (the fault seam) and, on POSIX, a child process killed with SIGKILL while
// it saves; the read-time sanitiser; where Documents/ is. The game's own sequence through
// NSFileHandle is in foundation_test.cpp.
#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
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
