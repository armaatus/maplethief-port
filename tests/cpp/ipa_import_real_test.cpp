// The first-launch IPA import on the real IPA (#182, ipa_import.h), local only: skipped (exit 77)
// without original/MapleThief-1.5.ipa or work/ipa/ (scripts/bootstrap). The player's IPA passes
// check_ipa with the 1.5 values and is imported into a fresh data directory, whose game/ must
// hold exactly the files of work/ipa/Payload/MapleThief.app (bootstrap.py's extraction), byte
// for byte. Nothing is kept: the temporary data directory is removed at the end.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <system_error>
#include <vector>

#include "ipa_import.h"
#include "maplethief/recomp/calls.h"

// The boot (linked through the frontend library) refers to the dispatcher; nothing here runs
// guest code.
extern "C" mt_guest_fn mt_dispatch(mt_cpu *, std::uint32_t target, std::uint32_t) noexcept {
    std::fprintf(stderr, "ipa_import_real_test: mt_dispatch(0x%X)\n",
                 static_cast<unsigned>(target));
    std::abort();
}

namespace {

namespace fs = std::filesystem;
namespace fe = maplethief::frontend;
using Bytes = std::vector<unsigned char>;

Bytes read_file(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Every file below `root`, by its path relative to it ('/' separated).
std::map<std::string, fs::path> files_below(const fs::path &root) {
    std::map<std::string, fs::path> files;
    for (const auto &e : fs::recursive_directory_iterator(root)) {
        if (e.is_regular_file()) {
            files[fs::relative(e.path(), root).generic_string()] = e.path();
        }
    }
    return files;
}

int failures = 0;
void check(bool ok, const std::string &what) {
    if (!ok) {
        std::printf("FAILED: %s\n", what.c_str());
        failures++;
    }
}

} // namespace

int main() {
    const fs::path repo = MT_REPO_DIR;
    const fs::path ipa = repo / "original" / "MapleThief-1.5.ipa";
    const fs::path app = repo / "work" / "ipa" / "Payload" / "MapleThief.app";
    if (!fs::is_regular_file(ipa) || !fs::is_directory(app)) {
        std::printf("skipped: %s or %s is missing (run scripts/bootstrap)\n", ipa.string().c_str(),
                    app.string().c_str());
        return 77;
    }
    const fe::IpaResult checked = fe::check_ipa(ipa.string(), fe::ipa_expected());
    check(checked.ok(), "check_ipa: " + checked.why);

    const fs::path data = fs::temp_directory_path() / "mt_ipa_import_real_test";
    std::error_code ec;
    fs::remove_all(data, ec);
    unsigned long long last = 0, total = 0;
    const fe::IpaResult r = fe::import_ipa(ipa.string(), data.string(), fe::ipa_expected(),
                                           [&](std::uint64_t done, std::uint64_t all) {
                                               last = done;
                                               total = all;
                                               return true;
                                           });
    check(r.ok(), "import_ipa: " + r.why);
    if (r.ok()) {
        check(fs::path(r.game_dir) == data / "game", "game_dir is <data>/game: " + r.game_dir);
        check(last == total && total > 0, "progress reached the total");
        const auto want = files_below(app);
        const auto got = files_below(data / "game");
        check(want.size() == got.size(),
              "file count: " + std::to_string(got.size()) + " of " + std::to_string(want.size()));
        std::size_t same = 0;
        for (const auto &[rel, path] : want) {
            const auto it = got.find(rel);
            if (it == got.end()) {
                check(false, "missing: " + rel);
                continue;
            }
            if (read_file(path) == read_file(it->second)) {
                same++;
            } else {
                check(false, "differs: " + rel);
            }
        }
        for (const auto &[rel, path] : got) {
            check(want.count(rel) != 0, "extra: " + rel);
        }
        check(fe::has_imported_game(data.string()), "has_imported_game");
        std::printf("imported %zu files (%llu bytes), all equal to work/ipa's: %s\n", got.size(),
                    total, same == want.size() ? "yes" : "no");
    }
    fs::remove_all(data, ec);
    std::printf("%s\n", failures == 0 ? "ok" : "FAILED");
    return failures == 0 ? 0 : 1;
}
