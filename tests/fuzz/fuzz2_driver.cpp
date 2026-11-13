// The shared driver of #323's fuzz targets (fuzz2.h). With libFuzzer (MAPLETHIEF_FUZZ) it only
// adds LLVMFuzzerInitialize; without, it is the program's main, a replayer:
//
//   fuzz_x --seeds                 every seed of the target, made in memory
//   fuzz_x --write-seeds DIR       the seeds as files in DIR (also MT_FUZZ_WRITE_SEEDS=DIR under
//                                  libFuzzer: written, then the program exits)
//   fuzz_x FILE|DIR...             each file (a directory's files, in name order)
//
// A replay runs each input once through LLVMFuzzerTestOneInput; an oracle or a sanitizer aborts.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "fuzz2.h"
#include "maplethief/recomp/cpu.h"

namespace fs = std::filesystem;

namespace {

int write_seeds(const char *dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    int n = 0;
    for (const mt_fuzz2::Seed &s : mt_fuzz2::seeds()) {
        std::ofstream out(fs::path(dir) / s.name, std::ios::binary);
        out.write(reinterpret_cast<const char *>(s.bytes.data()),
                  static_cast<std::streamsize>(s.bytes.size()));
        if (!out) {
            std::fprintf(stderr, "cannot write %s/%s\n", dir, s.name.c_str());
            return 1;
        }
        n++;
    }
    std::fprintf(stderr, "%d seeds written to %s\n", n, dir);
    return 0;
}

} // namespace

[[noreturn]] void mt_fuzz2::broken(const char *what) {
    std::fprintf(stderr, "fuzz oracle failed: %s\n", what);
    std::fflush(stderr);
    std::abort();
}

// No guest code is linked: the targets read files and restore states, and neither may run the
// game's code. A call into it is a finding.
extern "C" mt_guest_fn mt_dispatch(mt_cpu *, std::uint32_t target, std::uint32_t) noexcept {
    std::fprintf(stderr, "guest code called at 0x%X\n", static_cast<unsigned>(target));
    mt_fuzz2::broken("the runtime called guest code");
}

extern "C" int LLVMFuzzerInitialize(int *, char ***) {
    mt_fuzz2::init();
    if (const char *dir = std::getenv("MT_FUZZ_WRITE_SEEDS"); dir != nullptr && *dir != '\0') {
        std::exit(write_seeds(dir));
    }
    return 0;
}

#ifndef MT_FUZZ_LIBFUZZER

namespace {

bool run_file(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot read %s\n", path.string().c_str());
        return false;
    }
    const mt_fuzz2::Bytes bytes((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
    LLVMFuzzerTestOneInput(bytes.data(), bytes.size());
    return true;
}

} // namespace

int main(int argc, char **argv) {
    if (argc == 3 && std::strcmp(argv[1], "--write-seeds") == 0) {
        mt_fuzz2::init();
        return write_seeds(argv[2]);
    }
    LLVMFuzzerInitialize(&argc, &argv);
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s --seeds | --write-seeds DIR | FILE|DIR...\n", argv[0]);
        return 2;
    }
    int runs = 0;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--seeds") == 0) {
            for (const mt_fuzz2::Seed &s : mt_fuzz2::seeds()) {
                LLVMFuzzerTestOneInput(s.bytes.data(), s.bytes.size());
                runs++;
            }
            continue;
        }
        const fs::path p(argv[i]);
        std::vector<fs::path> files;
        if (fs::is_directory(p)) {
            for (const auto &e : fs::directory_iterator(p)) {
                if (e.is_regular_file()) {
                    files.push_back(e.path());
                }
            }
            std::sort(files.begin(), files.end());
        } else {
            files.push_back(p);
        }
        for (const fs::path &f : files) {
            if (!run_file(f)) {
                return 1;
            }
            runs++;
        }
    }
    std::printf("%d inputs ran\n", runs);
    return runs > 0 ? 0 : 1;
}

#endif
