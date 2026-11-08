// Copying a player's data folder (#262, ADR 0127); see data_copy.h.
#include "data_copy.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <system_error>
#include <utility>

#include "data_notice.h"
#include "maplethief/saves.h"
#include "save_tools.h"

namespace maplethief::frontend {

namespace fs = std::filesystem;

namespace {

using Bytes = std::vector<std::uint8_t>;

constexpr const char *kSettingsFile = "settings.ini";
// The top-level files copied as they are.
constexpr const char *kTopFiles[] = {"settings.ini", "bindings.ini", "window.ini"};
// The top-level folders copied byte for byte.
constexpr const char *kTopTrees[] = {kBackupsDirName, kStatesDirName, kScreenshotsDirName,
                                     kExportsDirName, kImportDirName};
constexpr const char *kBundleExecutable = "MapleThief";
constexpr const char *kBundleTmpPrefix = "game.tmp-";

fs::path utf8_path(const std::string &s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string path_utf8(const fs::path &p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}
// "saves/theif0": a path below a folder, with '/' on every OS.
std::string rel_utf8(const fs::path &p) {
    const std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

bool ends_with(const std::string &s, std::string_view suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix.data(), suffix.size()) == 0;
}
bool is_tmp_file(const std::string &name) { return ends_with(name, MT_SAVES_TMP_SUFFIX); }

// A suffix no other copy of this process or a concurrent one uses.
std::string unique_suffix() {
    static std::atomic<unsigned> counter{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    char buf[40];
    std::snprintf(buf, sizeof buf, "%llx-%u", static_cast<unsigned long long>(now),
                  counter.fetch_add(1));
    return buf;
}

// Not following links: a regular file, a folder, or something else (a link included).
enum class Kind { kNone, kFile, kDir, kOther };
Kind kind_of(const fs::path &p) {
    std::error_code ec;
    const fs::file_status st = fs::symlink_status(p, ec);
    if (ec || !fs::exists(st)) {
        return Kind::kNone;
    }
    if (fs::is_regular_file(st)) {
        return Kind::kFile;
    }
    if (fs::is_directory(st)) {
        return Kind::kDir;
    }
    return Kind::kOther;
}

std::optional<Bytes> read_file(const fs::path &p, std::uintmax_t max) {
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(p, ec);
    if (ec || size > max) {
        return std::nullopt;
    }
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    Bytes b(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char *>(b.data()), static_cast<std::streamsize>(size));
    if (static_cast<std::uintmax_t>(in.gcount()) != size) {
        return std::nullopt;
    }
    return b;
}

bool write_file(const fs::path &p, const Bytes &bytes) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    out.close();
    return static_cast<bool>(out);
}

bool copy_file_to(const fs::path &from, const fs::path &to, std::string *why) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    ec.clear();
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        *why = "cannot copy " + path_utf8(from) + " to " + path_utf8(to) + ": " + ec.message();
        return false;
    }
    return true;
}

// Whether two files hold the same bytes.
bool same_bytes(const fs::path &a, const fs::path &b) {
    std::error_code ec;
    const std::uintmax_t sa = fs::file_size(a, ec);
    if (ec || sa != fs::file_size(b, ec) || ec) {
        return false;
    }
    std::ifstream fa(a, std::ios::binary);
    std::ifstream fb(b, std::ios::binary);
    char ba[65536];
    char bb[65536];
    while (fa && fb) {
        fa.read(ba, sizeof ba);
        fb.read(bb, sizeof bb);
        if (fa.gcount() != fb.gcount() || !std::equal(ba, ba + fa.gcount(), bb)) {
            return false;
        }
    }
    return fa.eof() && fb.eof();
}

void remove_quietly(const fs::path &p) {
    std::error_code ec;
    fs::remove_all(p, ec);
}

// The source's saves folder: saves/, else the old Documents/ ("" if neither).
fs::path saves_dir_of(const fs::path &dir) {
    if (kind_of(dir / kSavesDirName) == Kind::kDir) {
        return dir / kSavesDirName;
    }
    if (kind_of(dir / kOldDocumentsDirName) == Kind::kDir) {
        return dir / kOldDocumentsDirName;
    }
    return {};
}

// --- what the source holds ---------------------------------------------------------------------

struct Plan {
    SaveSet saves;                                    // checked
    std::vector<std::pair<fs::path, fs::path>> files; // source file -> path below the staging
    std::vector<std::string> skipped;
    std::vector<SaveProblem> problems;
    std::string unreadable; // a game file that can't be taken
};

// Every regular file below `tree` (links and others skipped), as `rel` + its path below it.
void plan_tree(const fs::path &tree, const fs::path &rel, Plan *plan) {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(tree, fs::directory_options::none, ec), end;
         !ec && it != end; it.increment(ec)) {
        const fs::path below = rel / fs::relative(it->path(), tree, ec);
        const Kind k = kind_of(it->path());
        if (k == Kind::kDir) {
            continue;
        }
        if (k != Kind::kFile) {
            plan->skipped.push_back(rel_utf8(below) + ": not a regular file");
            if (it->is_symlink(ec)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (is_tmp_file(path_utf8(it->path().filename()))) {
            plan->skipped.push_back(rel_utf8(below) + ": a temporary file");
            continue;
        }
        plan->files.emplace_back(it->path(), below);
    }
}

Plan plan_copy(const fs::path &src) {
    Plan plan;
    const fs::path saves = saves_dir_of(src);
    std::error_code ec;
    // The save set, read and checked first.
    std::optional<fs::path> gamecenter;
    if (!saves.empty()) {
        std::vector<fs::path> entries;
        for (fs::directory_iterator it(saves, ec), end; !ec && it != end; it.increment(ec)) {
            entries.push_back(it->path());
        }
        std::sort(entries.begin(), entries.end());
        const std::string saves_name = path_utf8(saves.filename());
        for (const fs::path &p : entries) {
            const std::string name = path_utf8(p.filename());
            const std::string shown = saves_name + "/" + name;
            if (mt_saves_is_game_file(name.c_str()) != 0) {
                if (kind_of(p) != Kind::kFile) {
                    plan.unreadable = shown + " is not a regular file";
                    continue;
                }
                std::optional<Bytes> bytes = read_file(p, kMaxSaveSourceFileBytes);
                if (!bytes) {
                    plan.unreadable = shown + " cannot be read or is too large";
                    continue;
                }
                plan.saves.push_back(SaveFile{name, std::move(*bytes)});
            } else if (name == kGameCenterFileName && kind_of(p) == Kind::kFile) {
                gamecenter = p;
            } else if (is_tmp_file(name)) {
                plan.skipped.push_back(shown + ": a temporary file");
            } else {
                plan.skipped.push_back(shown + ": not a file the game saves");
            }
        }
        plan.problems = check_save_set(plan.saves);
    }
    if (!gamecenter && kind_of(src / kGameCenterFileName) == Kind::kFile) {
        gamecenter = src / kGameCenterFileName; // the old layout
    }
    if (gamecenter) {
        plan.files.emplace_back(*gamecenter, fs::path(kSavesDirName) / kGameCenterFileName);
    }
    // The top level.
    std::vector<fs::path> entries;
    for (fs::directory_iterator it(src, ec), end; !ec && it != end; it.increment(ec)) {
        entries.push_back(it->path());
    }
    std::sort(entries.begin(), entries.end());
    for (const fs::path &p : entries) {
        const std::string name = path_utf8(p.filename());
        if ((!saves.empty() && p == saves) || name == kGameCenterFileName) {
            continue;
        }
        const Kind k = kind_of(p);
        const bool top_file = std::any_of(std::begin(kTopFiles), std::end(kTopFiles),
                                          [&](const char *f) { return name == f; });
        const bool top_tree = std::any_of(std::begin(kTopTrees), std::end(kTopTrees),
                                          [&](const char *f) { return name == f; });
        if (top_file && k == Kind::kFile) {
            plan.files.emplace_back(p, fs::path(utf8_path(name)));
        } else if (top_tree && k == Kind::kDir) {
            plan_tree(p, utf8_path(name), &plan);
        } else if (name == kBundleDirName && k == Kind::kDir) {
            continue; // the bundle: copied on its own, or not at all
        } else if (name == kSavesDirName || name == kOldDocumentsDirName) {
            plan.skipped.push_back(name + ": " + path_utf8(saves.filename()) + "/ is used");
        } else if (name == kLogsDirName || name == kCrashesDirName) {
            plan.skipped.push_back(name + ": not player data");
        } else if (top_file || top_tree) {
            plan.skipped.push_back(name + ": not a regular file or folder");
        } else {
            plan.skipped.push_back(name + ": not a file the port knows");
        }
    }
    return plan;
}

// --- the staging -------------------------------------------------------------------------------

// The folders an interrupted copy of `dst` left beside it: <dst>.old-* renamed back when <dst>
// is missing, then every <dst>.tmp-* and <dst>.old-* removed.
void recover_leftovers(const fs::path &dst) {
    const fs::path parent = dst.parent_path();
    const std::string base = path_utf8(dst.filename());
    const std::string tmp_prefix = base + ".tmp-";
    const std::string old_prefix = base + ".old-";
    std::vector<fs::path> tmp;
    std::vector<fs::path> old;
    std::error_code ec;
    for (fs::directory_iterator it(parent, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = path_utf8(it->path().filename());
        if (name.rfind(tmp_prefix, 0) == 0) {
            tmp.push_back(it->path());
        } else if (name.rfind(old_prefix, 0) == 0) {
            old.push_back(it->path());
        }
    }
    std::sort(old.begin(), old.end());
    if (kind_of(dst) == Kind::kNone && !old.empty()) {
        fs::rename(old.back(), dst, ec);
        if (!ec) {
            SDL_Log("data: put back %s, left aside by an interrupted copy", path_utf8(dst).c_str());
            old.pop_back();
        }
    }
    for (const fs::path &p : tmp) {
        remove_quietly(p);
    }
    if (kind_of(dst) != Kind::kNone) {
        for (const fs::path &p : old) {
            remove_quietly(p);
        }
    }
}

// Copies the tree `from` into `to` (created), links as links; false with `why`.
bool copy_tree(const fs::path &from, const fs::path &to, std::string *why) {
    std::error_code ec;
    fs::create_directories(to, ec);
    for (fs::recursive_directory_iterator it(from, fs::directory_options::none, ec), end;
         !ec && it != end; it.increment(ec)) {
        const fs::path target = to / fs::relative(it->path(), from, ec);
        switch (kind_of(it->path())) {
        case Kind::kDir:
            fs::create_directories(target, ec);
            break;
        case Kind::kFile:
            if (!copy_file_to(it->path(), target, why)) {
                return false;
            }
            break;
        default:
            if (it->is_symlink(ec)) {
                it.disable_recursion_pending();
                fs::copy_symlink(it->path(), target, ec);
            }
            break;
        }
        if (ec) {
            *why = "cannot copy " + path_utf8(it->path()) + ": " + ec.message();
            return false;
        }
    }
    if (ec) {
        *why = "cannot read " + path_utf8(from) + ": " + ec.message();
        return false;
    }
    return true;
}

// The bundle (data_copy.h): copied when dst has none and src has one.
void copy_bundle(const fs::path &src, const fs::path &dst_bundle, const Sha256 &exe_sha,
                 DataCopyResult *r) {
    if (dst_bundle.empty() || kind_of(dst_bundle) != Kind::kNone) {
        return;
    }
    fs::path from;
    if (has_imported_game(path_utf8(src))) {
        from = src / kBundleDirName;
    } else if (path_utf8(src.filename()) == kDataDirName &&
               has_imported_game(path_utf8(src.parent_path()))) {
        from = src.parent_path() / kBundleDirName;
    } else {
        return;
    }
    const std::string shown = path_utf8(from);
    std::optional<Bytes> exe = read_file(from / kBundleExecutable, 256u << 20);
    if (!exe || sha256_of(*exe) != exe_sha) {
        r->skipped.push_back(shown + ": its MapleThief executable is not version 1.5's; "
                                     "import the IPA instead");
        return;
    }
    const fs::path parent = dst_bundle.parent_path();
    std::error_code ec;
    fs::create_directories(parent, ec);
    for (fs::directory_iterator it(parent, ec), end; !ec && it != end; it.increment(ec)) {
        if (path_utf8(it->path().filename()).rfind(kBundleTmpPrefix, 0) == 0) {
            remove_quietly(it->path());
        }
    }
    const fs::path tmp = parent / (std::string(kBundleTmpPrefix) + unique_suffix());
    std::string why;
    // Regular files only: a bundle with links in it is copied without them.
    bool ok = true;
    fs::create_directories(tmp, ec);
    for (fs::recursive_directory_iterator it(from, fs::directory_options::none, ec), end;
         ok && !ec && it != end; it.increment(ec)) {
        const fs::path target = tmp / fs::relative(it->path(), from, ec);
        const Kind k = kind_of(it->path());
        if (k == Kind::kDir) {
            fs::create_directories(target, ec);
        } else if (k == Kind::kFile) {
            ok = copy_file_to(it->path(), target, &why);
        } else if (it->is_symlink(ec)) {
            it.disable_recursion_pending();
        }
    }
    if (ok && ec) {
        ok = false;
        why = ec.message();
    }
    // The executable as it landed.
    if (ok) {
        std::optional<Bytes> copied = read_file(tmp / kBundleExecutable, 256u << 20);
        if (!copied || sha256_of(*copied) != exe_sha) {
            ok = false;
            why = "the copied executable is not version 1.5's";
        }
    }
    if (ok) {
        fs::rename(tmp, dst_bundle, ec);
        if (ec) {
            ok = false;
            why = "cannot rename it to " + path_utf8(dst_bundle) + ": " + ec.message();
        }
    }
    if (!ok) {
        remove_quietly(tmp);
        r->skipped.push_back(shown + ": " + why + "; import the IPA instead");
        return;
    }
    r->copied.emplace_back(std::string(kBundleDirName) + "/");
}

DataCopyResult refuse(std::string why) {
    DataCopyResult r;
    r.error = std::move(why);
    return r;
}

} // namespace

bool data_dir_has_player_data(const fs::path &dir) {
    if (dir.empty()) {
        return false;
    }
    if (kind_of(dir / kSettingsFile) == Kind::kFile) {
        return true;
    }
    for (const char *sub : {kSavesDirName, kOldDocumentsDirName}) {
        std::error_code ec;
        for (fs::directory_iterator it(dir / sub, ec), end; !ec && it != end; it.increment(ec)) {
            if (!is_tmp_file(path_utf8(it->path().filename())) &&
                kind_of(it->path()) != Kind::kDir) {
                return true;
            }
        }
    }
    return false;
}

DataCopyResult copy_player_data(const fs::path &src_dir, const fs::path &dst_dir,
                                const fs::path &dst_bundle_dir, bool force) {
    Sha256 exe{};
    parse_sha256(kExecutableSha256, &exe);
    return copy_player_data(src_dir, dst_dir, dst_bundle_dir, force, exe);
}

DataCopyResult copy_player_data(const fs::path &src_dir, const fs::path &dst_dir,
                                const fs::path &dst_bundle_dir, bool force,
                                const Sha256 &executable_sha256) {
    if (src_dir.empty() || dst_dir.empty()) {
        return refuse("no folder to copy from or to");
    }
    std::error_code ec;
    if (kind_of(src_dir) == Kind::kNone || !fs::is_directory(src_dir, ec)) {
        return refuse(path_utf8(src_dir) + " is not a folder");
    }
    const fs::path src = fs::canonical(src_dir, ec);
    if (ec) {
        return refuse("cannot read " + path_utf8(src_dir) + ": " + ec.message());
    }
    // A data folder that is a link (a synced folder, say): the copy goes where it points.
    fs::path dst = dst_dir;
    if (kind_of(dst_dir) != Kind::kNone) {
        dst = fs::canonical(dst_dir, ec);
        if (ec) {
            return refuse("cannot read " + path_utf8(dst_dir) + ": " + ec.message());
        }
        if (fs::equivalent(src, dst, ec)) {
            return refuse(path_utf8(src_dir) + " is the data folder itself");
        }
    }
    if (!data_dir_has_player_data(src)) {
        return refuse(path_utf8(src_dir) + " holds no saves and no settings.ini");
    }
    recover_leftovers(dst);
    if (!force && data_dir_has_player_data(dst)) {
        return refuse(path_utf8(dst_dir) + " already has data; nothing was copied (--force "
                                           "replaces it, backing up what it replaces)");
    }
    Plan plan = plan_copy(src);
    if (!plan.unreadable.empty() || !plan.problems.empty()) {
        std::string why = "the saves in " + path_utf8(src_dir) +
                          " can't be used, so nothing "
                          "was copied: ";
        if (!plan.unreadable.empty()) {
            why += plan.unreadable;
        }
        for (const SaveProblem &p : plan.problems) {
            why += (why.back() == ' ' ? "" : "; ") + p.line();
        }
        return refuse(why);
    }

    // The staging folder: what dst holds, then the source over it.
    const fs::path parent = dst.parent_path();
    const std::string base = path_utf8(dst.filename());
    const fs::path tmp = parent / utf8_path(base + ".tmp-" + unique_suffix());
    fs::create_directories(parent, ec);
    std::string why;
    if (!fs::create_directory(tmp, ec)) {
        return refuse("cannot create " + path_utf8(tmp) + ": " + ec.message());
    }
    const auto failed = [&tmp](std::string what) {
        remove_quietly(tmp);
        return refuse(std::move(what));
    };
    if (kind_of(dst) == Kind::kDir && !copy_tree(dst, tmp, &why)) {
        return failed(why);
    }
    DataCopyResult r;
    r.skipped = std::move(plan.skipped);
    // A file the source replaces goes to backups/import/<time>/ first (another name if that
    // folder is in the source or the destination already).
    std::string stamp = backup_stamp_now();
    for (int n = 2;
         kind_of(src / kBackupsDirName / kImportBackupsName / utf8_path(stamp)) != Kind::kNone ||
         kind_of(tmp / kBackupsDirName / kImportBackupsName / utf8_path(stamp)) != Kind::kNone;
         n++) {
        stamp = backup_stamp_now() + "-" + std::to_string(n);
    }
    const fs::path backup = tmp / kBackupsDirName / kImportBackupsName / utf8_path(stamp);
    const auto put_aside = [&](const fs::path &rel) {
        const fs::path at = tmp / rel;
        const fs::path to = backup / rel;
        std::error_code e;
        fs::create_directories(to.parent_path(), e);
        fs::rename(at, to, e);
        if (e) {
            why = "cannot back up " + path_utf8(at) + ": " + e.message();
            return false;
        }
        r.replaced.push_back(rel_utf8(rel));
        return true;
    };
    // `rel` is about to be written with other bytes than what is there.
    const auto make_room = [&](const fs::path &rel, const fs::path *from, const Bytes *bytes) {
        const fs::path at = tmp / rel;
        const Kind k = kind_of(at);
        if (k == Kind::kNone) {
            return true;
        }
        if (k == Kind::kFile) {
            if (from != nullptr
                    ? same_bytes(*from, at)
                    : read_file(at, kMaxSaveSourceFileBytes) == std::optional<Bytes>(*bytes)) {
                return true;
            }
            return put_aside(rel);
        }
        return put_aside(rel); // a folder or a link where a file goes
    };
    for (const SaveFile &f : plan.saves) {
        const fs::path rel = fs::path(kSavesDirName) / utf8_path(f.name);
        if (!make_room(rel, nullptr, &f.bytes)) {
            return failed(why);
        }
        if (!write_file(tmp / rel, f.bytes)) {
            return failed("cannot write " + path_utf8(tmp / rel));
        }
        r.copied.push_back(rel_utf8(rel));
    }
    // A slot's cash shop counters belong to its character (ADR 0078's import): a character
    // copied without them takes the old ones away.
    for (int slot = 0; slot < 3; slot++) {
        const std::string theif = "theif" + std::to_string(slot);
        const std::string counters = MT_SAVES_COUNTERS_PREFIX + std::to_string(slot);
        const auto has = [&plan](const std::string &n) {
            return std::any_of(plan.saves.begin(), plan.saves.end(),
                               [&n](const SaveFile &f) { return f.name == n; });
        };
        const fs::path rel = fs::path(kSavesDirName) / counters;
        if (has(theif) && !has(counters) && kind_of(tmp / rel) != Kind::kNone && !put_aside(rel)) {
            return failed(why);
        }
    }
    for (const auto &[from, rel] : plan.files) {
        if (!make_room(rel, &from, nullptr)) {
            return failed(why);
        }
        if (!copy_file_to(from, tmp / rel, &why)) {
            return failed(why);
        }
        r.copied.push_back(rel_utf8(rel));
    }
    if (!r.replaced.empty()) {
        r.backup = path_utf8(dst_dir / kBackupsDirName / kImportBackupsName / utf8_path(stamp));
    }

    // Into place: dst aside, the staging renamed, the old one removed (or put back).
    fs::path old;
    if (kind_of(dst) != Kind::kNone) {
        old = parent / utf8_path(base + ".old-" + unique_suffix());
        fs::rename(dst, old, ec);
        if (ec) {
            return failed("cannot move " + path_utf8(dst) + " aside: " + ec.message());
        }
    }
    fs::rename(tmp, dst, ec);
    if (ec) {
        const std::string what = ec.message();
        if (!old.empty()) {
            std::error_code back;
            fs::rename(old, dst, back);
        }
        return failed("cannot rename the copy to " + path_utf8(dst) + ": " + what);
    }
    if (!old.empty()) {
        remove_quietly(old);
    }
    r.ok = true;
    copy_bundle(src, dst_bundle_dir, executable_sha256, &r);
    return r;
}

std::string first_start_copied_text(const std::string &old_dir, const std::string &new_dir) {
    return "Your saves and settings were copied from " + old_dir + " to " + new_dir +
           ". The old folder was left as it is.";
}

std::string first_start_refused_text(const std::string &old_dir, const std::string &why) {
    return "Your saves in " + old_dir + " were not copied to the game's data folder: " + why +
           ". The old folder was left as it is; the copy is tried again on the next start.";
}

bool first_start_copy(const DataLocation &loc, const std::string &old_dir, DataCopyResult *result) {
    if (loc.place != DataPlace::kGame || loc.dir.empty() || old_dir.empty()) {
        return false;
    }
    const fs::path dst = utf8_path(loc.dir);
    const fs::path src = utf8_path(old_dir);
    std::error_code ec;
    if (fs::equivalent(src, dst, ec) || data_dir_has_player_data(dst) ||
        !data_dir_has_player_data(src)) {
        return false;
    }
    DataCopyResult r = copy_player_data(src, dst, utf8_path(loc.bundle_dir), false);
    for (const std::string &line : r.copied) {
        SDL_Log("data: copied %s", line.c_str());
    }
    for (const std::string &line : r.skipped) {
        SDL_Log("data: not copied: %s", line.c_str());
    }
    queue_data_notice(r.ok ? first_start_copied_text(old_dir, loc.dir)
                           : first_start_refused_text(old_dir, r.error));
    if (result != nullptr) {
        *result = std::move(r);
    }
    return true;
}

} // namespace maplethief::frontend
