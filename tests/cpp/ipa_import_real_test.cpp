// The first-launch IPA import on the real IPA (#182, ipa_import.h), local only: skipped (exit 77)
// without original/MapleThief-1.5.ipa or work/ipa/ (scripts/bootstrap). The player's IPA passes
// check_ipa with the 1.5 values and is imported into a fresh data directory, whose game/ must
// hold exactly the files of work/ipa/Payload/MapleThief.app (bootstrap.py's extraction), byte
// for byte. The Nintendo Switch's SD-card route (#243, import_screen.h) on a fake SD folder holding
// a wrong .ipa and a copy of the IPA: the copy is found first, imported, and deleted when the
// player says Yes. A repack (#350, ADR 0135's addendum): with the per-file hash list compiled in
// (work/gen/ipa_files.inc), the IPA's entries stored in reverse order are imported through the
// SD-card route, and the same with one listed file changed is refused by its name. Nothing is
// kept: the temporary folders are removed at the end.
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

#include "import_screen.h"
#include "ipa_import.h"
#include "maplethief/recomp/calls.h"
#include "save_zip.h"

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

    // The SD-card route: <base> with the IPA under its own name and a wrong file before it in
    // name order; the IPA's name comes first anyway.
    const fs::path sd = fs::temp_directory_path() / "mt_ipa_import_real_test_sd";
    fs::remove_all(sd, ec);
    fs::create_directories(sd, ec);
    fs::copy_file(ipa, sd / "MapleThief-1.5.ipa", ec);
    check(!ec, "copy the IPA to the fake SD folder: " + ec.message());
    std::ofstream(sd / "aaa-notes.ipa") << "not an ipa";
    fe::ImportScreen screen(
        sd.string(),
        [&sd](const std::string &path, const fe::IpaProgress &p) {
            return fe::import_ipa(path, sd.string(), fe::ipa_expected(), p);
        },
        fe::ImportScreen::Route::kSdCard);
    check(screen.look(), "SD route: look");
    screen.wait();
    check(screen.state() == fe::ImportScreen::State::kAskDelete,
          "SD route: imported, asking to delete: " + screen.result().why);
    check(fs::path(screen.file()) == sd / "MapleThief-1.5.ipa", "SD route: the IPA's name first");
    check(screen.refused().empty(), "SD route: nothing refused before it");
    check(screen.answer_delete(true) && screen.deleted() && !fs::exists(sd / "MapleThief-1.5.ipa"),
          "SD route: Yes deletes the IPA");
    check(fe::has_imported_game(sd.string()), "SD route: game/ in <base>");
    check(fs::exists(sd / "aaa-notes.ipa"), "SD route: the other file is left alone");
    std::printf("SD route: imported %s into %s, then deleted it\n", screen.file().c_str(),
                (sd / "game").string().c_str());
    fs::remove_all(sd, ec);

    // A repack: the same entries, stored, in reverse order; then one listed file changed.
    const fe::IpaExpected expected = fe::ipa_expected();
    if (expected.files.empty()) {
        std::printf("repack: not checked (no work/gen/ipa_files.inc in this build)\n");
    } else {
        std::vector<std::uint8_t> bytes;
        check(fe::check_ipa(ipa.string(), expected, &bytes).ok(), "read the IPA");
        std::vector<fe::ZipEntry> entries;
        std::string why;
        check(fe::zip_read(bytes, fe::kMaxIpaEntryBytes, &entries, &why), "zip_read: " + why);
        const std::vector<fe::ZipEntry> reversed(entries.rbegin(), entries.rend());
        fs::create_directories(sd, ec);
        const fs::path repack = sd / "repacked.ipa";
        const std::vector<std::uint8_t> zip = fe::zip_store(reversed);
        std::ofstream(repack, std::ios::binary)
            .write(reinterpret_cast<const char *>(zip.data()),
                   static_cast<std::streamsize>(zip.size()));
        check(zip.size() != fe::kIpaSize, "the repack is another file");
        fe::ImportScreen repacked(
            sd.string(),
            [&sd, &expected](const std::string &path, const fe::IpaProgress &p) {
                return fe::import_ipa(path, sd.string(), expected, p);
            },
            fe::ImportScreen::Route::kSdCard);
        check(repacked.look(), "repack: look");
        repacked.wait();
        check(repacked.state() == fe::ImportScreen::State::kAskDelete,
              "repack: imported through the SD route: " + repacked.result().why);
        check(fe::has_imported_game(sd.string()), "repack: game/ in <base>");
        check(files_below(sd / "game").size() == expected.files.size() + 1,
              "repack: game/ holds the executable and the listed files only");

        const std::string victim = "Payload/MapleThief.app/" + expected.files.front().path;
        std::vector<fe::ZipEntry> changed = reversed;
        for (fe::ZipEntry &e : changed) {
            if (e.name == victim) {
                e.bytes.push_back(0);
            }
        }
        const fe::IpaResult refused = fe::check_ipa_bytes(fe::zip_store(changed), expected);
        check(refused.problem == fe::IpaProblem::kModified &&
                  refused.why.find(expected.files.front().path) != std::string::npos,
              "repack with " + victim + " changed: refused by name: " + refused.why);
        std::printf("repack: %zu entries in reverse order imported (%zu listed files); "
                    "changed: \"%s\"\n",
                    entries.size(), expected.files.size(), refused.why.c_str());
        fs::remove_all(sd, ec);
    }
    std::printf("%s\n", failures == 0 ? "ok" : "FAILED");
    return failures == 0 ? 0 : 1;
}
