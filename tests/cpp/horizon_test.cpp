// The runtime's OS layer as the Nintendo Switch needs it (#236): guest memory committed on
// demand and given back, an interrupted save recovered at the next start, the file system calls
// of files.cpp and bundle.cpp, and a thread made the way GCD makes its workers. Part of
// runtime_test on every desktop, and of MapleThiefTests.nro, where the same cases run on Horizon
// in applet mode and in title takeover (scripts/switch-test).
#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <system_error>

#include "maplethief/memory.h"
#include "maplethief/saves.h"
#include "maplethief/stubs.h"
#include "mem/vm.h"

#if !defined(_WIN32)
#include <pthread.h>
#endif

namespace {

namespace fs = std::filesystem;

constexpr std::uint32_t k_64mib = 64u << 20;

bool all_zero(const std::uint8_t *p, std::size_t n) {
    for (std::size_t i = 0; i < n; i += 4096u) {
        if (p[i] != 0u || p[i + 4095u] != 0u) { // the first and last byte of every page
            return false;
        }
    }
    return true;
}

// A fresh directory on the file system the data folder is on: the SD card on the Switch
// (MapleThiefTests.nro sets TMPDIR to sdmc:/switch/maplethief-tests/tmp), the temporary
// directory elsewhere.
fs::path scratch(const char *what) {
    const fs::path p =
        fs::temp_directory_path() /
        (std::string("maplethief-horizon-") + what + "-" + std::to_string(std::random_device{}()) +
         "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p);
    return p;
}

std::string read_file(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("horizon: 64 MiB of guest heap is committed, freed, and committed again as zeros") {
    // Applet mode leaves a few hundred MiB for the whole process: eight rounds of 64 MiB pass
    // only if each round's memory goes back to the system when its address space ends.
    for (int round = 0; round < 8; round++) {
        CAPTURE(round);
        mt_mem_config cfg = mt_mem_config_default();
        cfg.reservation = MT_MEM_RESERVE_ARENA; // the Switch's model
        mt_mem *mem = mt_mem_create(&cfg);
        REQUIRE(mem != nullptr);
        const std::uint32_t block = mt_heap_malloc(mem, k_64mib);
        REQUIRE(block != 0u);
        std::uint8_t *host = mt_mem_base(mem) + block;
        CHECK(all_zero(host, k_64mib));
        std::memset(host, 0xA5, k_64mib);
        CHECK(mt_heap_free(mem, block) == 0);
        mt_mem_destroy(mem);
    }
}

TEST_CASE("horizon: guest pages decommitted and committed again read zero") {
    const std::size_t page = mt::vm::page_size();
    std::uint8_t *base = mt::vm::reserve(std::uint64_t{k_64mib} + 2u * page);
    REQUIRE(base != nullptr);
    std::uint8_t *at = base + page; // a page of reserved space on either side
    for (int round = 0; round < 3; round++) {
        CAPTURE(round);
        REQUIRE(mt::vm::commit(at, k_64mib));
        CHECK(all_zero(at, k_64mib));
        std::memset(at, 0x5A, k_64mib);
        REQUIRE(mt::vm::commit(at, page)); // committing again keeps the bytes
        CHECK(at[0] == 0x5Au);
        REQUIRE(mt::vm::decommit(at, k_64mib));
    }
    mt::vm::release(base, std::uint64_t{k_64mib} + 2u * page);
}

TEST_CASE("horizon: an interrupted save is the old or the new file at the next start") {
    const fs::path dir = scratch("save");
    const fs::path file = dir / "theif0";
    const std::string old_bytes(1124, 'o'), new_bytes(1124, 'n');
    for (const mt_saves_fault fault :
         {MT_SAVES_FAULT_OPENED, MT_SAVES_FAULT_HALF_WRITTEN, MT_SAVES_FAULT_WRITTEN,
          MT_SAVES_FAULT_READY, MT_SAVES_FAULT_REMOVED}) {
        CAPTURE(static_cast<int>(fault));
        REQUIRE(mt_saves_replace_file(file.string().c_str(), old_bytes.data(), old_bytes.size()) ==
                0);
        mt_saves_set_fault(fault); // the write is killed here (the platform's own way: AUTO)
        mt_saves_replace_file(file.string().c_str(), new_bytes.data(), new_bytes.size());
        mt_saves_set_fault(MT_SAVES_FAULT_NONE);
        mt_saves_set_documents_dir(dir.string().c_str()); // the next start
        const std::string now = read_file(file);
        CHECK((now == old_bytes || now == new_bytes));
        CHECK_FALSE(fs::exists(dir / ("theif0" MT_SAVES_READY_SUFFIX)));
    }
    mt_saves_set_documents_dir(nullptr);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("horizon: the file system calls of files.cpp and bundle.cpp work on the data folder's "
          "file system") {
    const fs::path dir = scratch("fs");
    fs::create_directories(dir / "Documents" / "sub");
    {
        std::ofstream(dir / "Documents" / "theif0", std::ios::binary) << std::string(1124, 'x');
    }
    {
        std::ofstream(dir / "Documents" / "sub" / "a.png", std::ios::binary) << "png";
    }
    CHECK(fs::exists(dir / "Documents" / "theif0"));
    CHECK(fs::is_directory(dir / "Documents" / "sub"));
    CHECK(fs::is_regular_file(dir / "Documents" / "theif0"));
    CHECK(fs::file_size(dir / "Documents" / "theif0") == 1124u);
    int top = 0, deep = 0;
    for (const auto &e : fs::directory_iterator(dir / "Documents")) {
        (void)e;
        top++;
    }
    for (const auto &e : fs::recursive_directory_iterator(dir)) {
        (void)e;
        deep++;
    }
    CHECK(top == 2);
    CHECK(deep == 4); // Documents, theif0, sub, a.png
    CHECK(fs::remove_all(dir) == 5u);
    CHECK_FALSE(fs::exists(dir));
}

TEST_CASE("horizon: the runtime's default data folder") {
    const char *env = std::getenv("MAPLETHIEF_DATA_DIR");
    if (env != nullptr && env[0] != '\0') {
        return; // a test run that chose its own
    }
#if defined(__SWITCH__)
    CHECK(std::string(mt_stubs_data_dir()) == MT_STUBS_SWITCH_DATA_DIR);
#else
    CHECK(std::string(mt_stubs_data_dir()).find("hief") != std::string::npos);
#endif
}

#if !defined(_WIN32)
namespace {

// Uses about `bytes` of the calling thread's stack, 64 KiB per call.
std::uint32_t use_stack(std::uint32_t bytes) {
    volatile std::uint8_t frame[64 * 1024];
    for (std::size_t i = 0; i < sizeof frame; i += 512u) {
        frame[i] = static_cast<std::uint8_t>(i >> 9);
    }
    const std::uint32_t below = bytes > sizeof frame ? use_stack(bytes - sizeof frame) : 0u;
    return below + frame[512];
}

void *deep_thread(void *out) {
    *static_cast<std::uint32_t *>(out) = use_stack(6u << 20);
    return nullptr;
}

} // namespace

TEST_CASE("horizon: a thread made like a GCD worker has its 8 MiB stack") {
    // gcd.cpp's start_thread: pthread_attr_setstacksize(8 MiB) for the generated code's
    // recursion (ADR 0033). libnx maps the stack from the heap; 6 MiB of it are used here.
    pthread_attr_t attr;
    REQUIRE(pthread_attr_init(&attr) == 0);
    REQUIRE(pthread_attr_setstacksize(&attr, std::size_t{8} << 20) == 0);
    pthread_t t;
    std::uint32_t result = 0;
    REQUIRE(pthread_create(&t, &attr, deep_thread, &result) == 0);
    pthread_attr_destroy(&attr);
    REQUIRE(pthread_join(t, nullptr) == 0);
    CHECK(result == 96u); // 96 frames of 64 KiB, each with frame[512] == 1
}
#endif
