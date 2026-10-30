// The save tools (#148, ADR 0078); see save_tools.h.
#include "save_tools.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <system_error>
#include <utility>

#include "maplethief/hooks.h"
#include "maplethief/saves.h"
#include "maplethief/stubs.h"
#include "save_zip.h"

namespace maplethief::frontend {

namespace fs = std::filesystem;

namespace {

// The largest zip an import reads (a set of saves is ~25 KB; receipt lists add a few KB each).
constexpr std::uintmax_t kMaxZipBytes = 64u * 1024u * 1024u;

SaveLocation g_location;
bool g_configured = false;

// A path from a UTF-8 string, and back (on Windows a narrow string is not UTF-8).
fs::path utf8_path(const std::string &s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string path_utf8(const fs::path &p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

bool fail(std::string *why, const std::string &what) {
    if (why != nullptr) {
        *why = what;
    }
    return false;
}

std::optional<std::vector<std::uint8_t>> read_file(const fs::path &p, std::uintmax_t max) {
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(p, ec);
    if (ec || size > max) {
        return std::nullopt;
    }
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (size > 0 &&
        !f.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size))) {
        return std::nullopt;
    }
    return bytes;
}

bool write_file(const fs::path &p, const std::vector<std::uint8_t> &bytes) {
    return mt_saves_replace_file(path_utf8(p).c_str(), bytes.data(), bytes.size()) == 0;
}

bool ends_with(std::string_view s, std::string_view tail) {
    return s.size() >= tail.size() && s.substr(s.size() - tail.size()) == tail;
}

bool is_tmp(std::string_view name) { return ends_with(name, MT_SAVES_TMP_SUFFIX); }

bool is_game_file(const std::string &name) { return mt_saves_is_game_file(name.c_str()) != 0; }

// Whether `name` is a backup folder's: "YYYYMMDD-HHMMSS-mmm", maybe with "-N". `order` (if not
// null) gets what sorts it by age: the stamp, then N.
bool backup_name(std::string_view name, std::pair<std::string, unsigned> *order = nullptr) {
    constexpr std::string_view kShape = "dddddddd-dddddd-ddd";
    if (name.size() < kShape.size()) {
        return false;
    }
    for (std::size_t i = 0; i < kShape.size(); i++) {
        const bool digit = std::isdigit(static_cast<unsigned char>(name[i])) != 0;
        if (kShape[i] == 'd' ? !digit : name[i] != '-') {
            return false;
        }
    }
    unsigned n = 0;
    std::string_view rest = name.substr(kShape.size());
    if (!rest.empty()) {
        if (rest.size() < 2 || rest.size() > 4 || rest[0] != '-') {
            return false;
        }
        for (const char c : rest.substr(1)) {
            if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
                return false;
            }
            n = n * 10 + static_cast<unsigned>(c - '0');
        }
    }
    if (order != nullptr) {
        *order = {std::string(name.substr(0, kShape.size())), n};
    }
    return true;
}

// The backup folders in `dir`, newest first.
std::vector<fs::path> backup_dirs(const fs::path &dir) {
    std::vector<std::pair<std::pair<std::string, unsigned>, fs::path>> found;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::pair<std::string, unsigned> order;
        const std::string name = path_utf8(it->path().filename());
        if (it->is_directory(ec) && backup_name(name, &order)) {
            found.emplace_back(order, it->path());
        }
    }
    std::sort(found.begin(), found.end(),
              [](const auto &a, const auto &b) { return a.first > b.first; });
    std::vector<fs::path> out;
    for (auto &f : found) {
        out.push_back(std::move(f.second));
    }
    return out;
}

// A new folder for a backup in `parent`: <stamp>, or <stamp>-N if that exists.
std::optional<fs::path> new_backup_dir(const fs::path &parent, const std::string &stamp) {
    std::error_code ec;
    for (unsigned n = 0; n < 1000; n++) {
        const fs::path dir = parent / utf8_path(n == 0 ? stamp : stamp + "-" + std::to_string(n));
        if (!fs::exists(dir, ec)) {
            fs::create_directories(dir, ec);
            if (ec) {
                return std::nullopt;
            }
            return dir;
        }
    }
    return std::nullopt;
}

bool same_path(const fs::path &a, const fs::path &b) {
    std::error_code ec;
    return fs::equivalent(a, b, ec) && !ec;
}

// Copies `files` into a new backup folder in `parent`; "" (and the folder removed) on failure.
std::string write_backup(const fs::path &parent, const std::string &stamp, const SaveSet &files,
                         std::string *why) {
    const std::optional<fs::path> dir = new_backup_dir(parent, stamp);
    if (!dir) {
        fail(why, "cannot make a backup folder in " + path_utf8(parent));
        return "";
    }
    for (const SaveFile &f : files) {
        if (!write_file(*dir / utf8_path(f.name), f.bytes)) {
            std::error_code ec;
            fs::remove_all(*dir, ec);
            fail(why, "cannot write the backup of " + f.name + " in " + path_utf8(*dir));
            return "";
        }
    }
    return path_utf8(*dir);
}

const SaveFile *find_file(const SaveSet &files, const std::string &name) {
    for (const SaveFile &f : files) {
        if (f.name == name) {
            return &f;
        }
    }
    return nullptr;
}

} // namespace

// --- Where -------------------------------------------------------------------------------------

std::string portable_base_dir(std::string_view base_path) {
    std::string base(base_path);
    while (base.size() > 1 && (base.back() == '/' || base.back() == '\\')) {
        base.pop_back();
    }
    // Inside an app bundle: the folder around the .app.
    const std::size_t app = base.rfind(".app/Contents");
    if (app != std::string::npos) {
        const std::size_t slash = base.rfind('/', app);
        return slash == std::string::npos ? std::string(".") : base.substr(0, slash);
    }
    return base;
}

std::string portable_data_dir(const std::string &base_dir) {
    if (base_dir.empty()) {
        return "";
    }
    std::error_code ec;
    const fs::path base = utf8_path(base_dir);
    if (!fs::is_regular_file(base / kPortableFileName, ec)) {
        return "";
    }
    return path_utf8(base / kPortableDataDirName);
}

SaveLocation configure_save_location(const Settings &settings, const std::string &data_dir,
                                     const std::string &documents_override) {
    const char *env = SDL_getenv("MAPLETHIEF_DATA_DIR");
    if ((env == nullptr || *env == '\0') && !data_dir.empty()) {
        mt_stubs_set_data_dir(data_dir.c_str());
    }
    const std::string &docs = !settings.save_dir.empty() ? settings.save_dir : documents_override;
    mt_saves_set_documents_dir(docs.empty() ? nullptr : docs.c_str());
    g_location = SaveLocation{};
    g_location.data_dir = mt_stubs_data_dir();
    g_location.documents_dir = mt_saves_documents_dir();
    if (!g_location.data_dir.empty()) {
        g_location.backups_dir = path_utf8(utf8_path(g_location.data_dir) / kBackupsDirName);
    }
    g_location.keep = settings.save_backups;
    g_configured = true;
    return g_location;
}

SaveLocation save_location() {
    if (g_configured) {
        return g_location;
    }
    SaveLocation loc;
    loc.data_dir = mt_stubs_data_dir();
    loc.documents_dir = mt_saves_documents_dir();
    if (!loc.data_dir.empty()) {
        loc.backups_dir = path_utf8(utf8_path(loc.data_dir) / kBackupsDirName);
    }
    return loc;
}

// --- Sets of save files ------------------------------------------------------------------------

std::string SaveProblem::line() const { return file + ": " + code + ": " + why; }

SaveSet read_save_dir(const std::string &dir) {
    SaveSet files;
    if (dir.empty()) {
        return files;
    }
    std::error_code ec;
    for (fs::directory_iterator it(utf8_path(dir), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = path_utf8(it->path().filename());
        if (!is_game_file(name) || !it->is_regular_file(ec)) {
            continue;
        }
        if (auto bytes = read_file(it->path(), kMaxSaveSourceFileBytes)) {
            files.push_back(SaveFile{name, std::move(*bytes)});
        }
    }
    std::sort(files.begin(), files.end(),
              [](const SaveFile &a, const SaveFile &b) { return a.name < b.name; });
    return files;
}

bool read_save_source(const std::string &source, SaveSet *files, std::vector<std::string> *ignored,
                      std::string *why) {
    files->clear();
    ignored->clear();
    std::error_code ec;
    fs::path path = utf8_path(source);
    if (source.empty() || !fs::exists(path, ec)) {
        return fail(why, "cannot find " + source);
    }
    if (fs::is_directory(path, ec)) {
        const auto has_game_files = [](const fs::path &dir) {
            std::error_code e;
            for (fs::directory_iterator it(dir, e), end; !e && it != end; it.increment(e)) {
                if (is_game_file(path_utf8(it->path().filename()))) {
                    return true;
                }
            }
            return false;
        };
        if (!has_game_files(path) && fs::is_directory(path / "Documents", ec)) {
            path /= "Documents";
        }
        std::vector<fs::path> entries;
        for (fs::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec)) {
            entries.push_back(it->path());
        }
        if (ec) {
            return fail(why, "cannot read the folder " + path_utf8(path));
        }
        std::sort(entries.begin(), entries.end());
        for (const fs::path &p : entries) {
            const std::string name = path_utf8(p.filename());
            if (is_tmp(name) || name.empty() || name[0] == '.' || !fs::is_regular_file(p, ec)) {
                continue;
            }
            if (!is_game_file(name)) {
                ignored->push_back(name);
                continue;
            }
            auto bytes = read_file(p, kMaxSaveSourceFileBytes);
            if (!bytes) {
                return fail(why, "cannot read " + path_utf8(p) + " (or it is larger than " +
                                     std::to_string(kMaxSaveSourceFileBytes) + " bytes)");
            }
            files->push_back(SaveFile{name, std::move(*bytes)});
        }
    } else {
        const auto bytes = read_file(path, kMaxZipBytes);
        if (!bytes) {
            return fail(why, "cannot read " + source + " (or it is larger than " +
                                 std::to_string(kMaxZipBytes) + " bytes)");
        }
        std::vector<ZipEntry> entries;
        std::string zip_why;
        if (!zip_read(*bytes, kMaxSaveSourceFileBytes, &entries, &zip_why)) {
            return fail(why,
                        source + " is neither a folder nor a zip file this can read: " + zip_why);
        }
        for (ZipEntry &e : entries) {
            const std::size_t slash = e.name.rfind('/');
            const std::string name = slash == std::string::npos ? e.name : e.name.substr(slash + 1);
            const bool mac_junk =
                e.name.rfind("__MACOSX/", 0) == 0 || e.name.find("/__MACOSX/") != std::string::npos;
            if (mac_junk || name.empty() || name[0] == '.' || is_tmp(name)) {
                continue;
            }
            if (!is_game_file(name)) {
                ignored->push_back(e.name);
                continue;
            }
            if (find_file(*files, name) != nullptr) {
                return fail(why, source + " holds " + name + " twice (in two folders)");
            }
            files->push_back(SaveFile{name, std::move(e.bytes)});
        }
    }
    std::sort(files->begin(), files->end(),
              [](const SaveFile &a, const SaveFile &b) { return a.name < b.name; });
    return true;
}

std::vector<SaveProblem> check_save_set(const SaveSet &files) {
    std::vector<SaveProblem> problems;
    for (const SaveFile &f : files) {
        char why[256];
        const mt_saves_verdict v =
            mt_saves_check(f.name.c_str(), f.bytes.data(), f.bytes.size(), why, sizeof why);
        if (v != MT_SAVES_OK) {
            problems.push_back(SaveProblem{f.name, mt_saves_verdict_name(v), why});
        }
    }
    for (int slot = 0; slot < 3; slot++) {
        const std::string theif = "theif" + std::to_string(slot);
        const std::string quest = "quest" + std::to_string(slot);
        const bool t = find_file(files, theif) != nullptr;
        const bool q = find_file(files, quest) != nullptr;
        if (t != q) {
            problems.push_back(SaveProblem{"set", "bad_pair",
                                           (t ? theif : quest) + " comes without " +
                                               (t ? quest : theif) +
                                               "; a character is both files"});
            break;
        }
    }
    return problems;
}

// --- Export and import ---------------------------------------------------------------------------

bool export_saves(const SaveLocation &loc, const std::string &dest,
                  std::vector<std::string> *exported, std::string *why) {
    if (exported != nullptr) {
        exported->clear();
    }
    const SaveSet files = read_save_dir(loc.documents_dir);
    if (files.empty()) {
        return fail(why, "there are no saves in " + loc.documents_dir);
    }
    if (dest.empty()) {
        return fail(why, "no place to export to");
    }
    std::string lower = dest;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const fs::path out = utf8_path(dest);
    if (ends_with(lower, ".zip")) {
        std::vector<ZipEntry> entries;
        for (const SaveFile &f : files) {
            entries.push_back(ZipEntry{f.name, f.bytes});
        }
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &now);
#else
        localtime_r(&now, &tm);
#endif
        const DosTime when = dos_time(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                                      tm.tm_min, tm.tm_sec);
        if (!write_file(out, zip_store(entries, when))) {
            return fail(why, "cannot write " + dest);
        }
    } else {
        if (same_path(out, utf8_path(loc.documents_dir))) {
            return fail(why, dest + " is the save folder itself");
        }
        for (const SaveFile &f : files) {
            if (!write_file(out / utf8_path(f.name), f.bytes)) {
                return fail(why, "cannot write " + path_utf8(out / utf8_path(f.name)));
            }
        }
    }
    if (exported != nullptr) {
        for (const SaveFile &f : files) {
            exported->push_back(f.name);
        }
    }
    return true;
}

bool import_saves(const SaveLocation &loc, const std::string &source, const std::string &stamp,
                  ImportResult *result, std::string *why) {
    ImportResult local;
    ImportResult &r = result != nullptr ? *result : local;
    r = ImportResult{};
    SaveSet files;
    if (!read_save_source(source, &files, &r.ignored, why)) {
        return false;
    }
    if (files.empty()) {
        return fail(why, "there are no save files in " + source);
    }
    r.problems = check_save_set(files);
    if (!r.problems.empty()) {
        std::string what = r.problems.front().line();
        if (r.problems.size() > 1) {
            what += " (and " + std::to_string(r.problems.size() - 1) + " more)";
        }
        return fail(why, what + ": nothing was imported");
    }
    if (loc.documents_dir.empty()) {
        return fail(why, "there is no save folder to import into");
    }
    const fs::path docs = utf8_path(loc.documents_dir);
    if (const fs::path src = utf8_path(source);
        same_path(src, docs) || same_path(src / "Documents", docs)) {
        return fail(why, source + " is the save folder itself");
    }
    // What the import replaces, as it is now: backed up first, and put back if a write fails.
    SaveSet old;
    for (const SaveFile &f : files) {
        std::error_code ec;
        const fs::path p = docs / utf8_path(f.name);
        if (fs::exists(p, ec)) {
            auto bytes = read_file(p, kMaxSaveSourceFileBytes);
            if (!bytes) {
                return fail(why, "cannot read " + path_utf8(p) + " to back it up");
            }
            old.push_back(SaveFile{f.name, std::move(*bytes)});
        }
    }
    if (!old.empty()) {
        if (loc.backups_dir.empty()) {
            return fail(why, "there is no backups folder for the saves an import replaces");
        }
        const fs::path parent = utf8_path(loc.backups_dir) / kImportBackupsName;
        r.backup = write_backup(parent, stamp, old, why);
        if (r.backup.empty()) {
            return false;
        }
        rotate_backups(path_utf8(parent), std::max<std::uint32_t>(loc.keep, 1));
    }
    for (std::size_t i = 0; i < files.size(); i++) {
        if (write_file(docs / utf8_path(files[i].name), files[i].bytes)) {
            continue;
        }
        // Put back what was written: the old bytes, or no file where there was none.
        for (std::size_t j = 0; j < i; j++) {
            const fs::path p = docs / utf8_path(files[j].name);
            if (const SaveFile *before = find_file(old, files[j].name)) {
                write_file(p, before->bytes);
            } else {
                std::error_code ec;
                fs::remove(p, ec);
            }
        }
        return fail(why, "cannot write " + files[i].name + " in " + loc.documents_dir +
                             ": the saves are as they were");
    }
    for (const SaveFile &f : files) {
        r.imported.push_back(f.name);
    }
    return true;
}

// --- Backups -----------------------------------------------------------------------------------

std::vector<BackupInfo> list_backups(const SaveLocation &loc) {
    std::vector<BackupInfo> out;
    if (loc.backups_dir.empty()) {
        return out;
    }
    for (const char *kind : {"0", "1", "2", kImportBackupsName}) {
        for (const fs::path &dir : backup_dirs(utf8_path(loc.backups_dir) / kind)) {
            BackupInfo b;
            b.path = path_utf8(dir);
            b.kind = kind;
            b.stamp = path_utf8(dir.filename());
            for (const SaveFile &f : read_save_dir(b.path)) {
                b.files.push_back(f.name);
            }
            out.push_back(std::move(b));
        }
    }
    return out;
}

void rotate_backups(const std::string &dir, std::uint32_t keep) {
    const std::vector<fs::path> dirs = backup_dirs(utf8_path(dir));
    for (std::size_t i = keep; i < dirs.size(); i++) {
        std::error_code ec;
        fs::remove_all(dirs[i], ec);
    }
}

std::string backup_slot(const SaveLocation &loc, int slot, const std::string &stamp,
                        std::string *why) {
    if (loc.keep == 0 || loc.backups_dir.empty() || slot < 0 || slot > 2) {
        return "";
    }
    const fs::path docs = utf8_path(loc.documents_dir);
    SaveSet files;
    for (const char *kind : {"quest", "theif"}) {
        const std::string name = kind + std::to_string(slot);
        std::error_code ec;
        if (!fs::is_regular_file(docs / name, ec)) {
            continue;
        }
        if (auto bytes = read_file(docs / name, kMaxSaveSourceFileBytes)) {
            files.push_back(SaveFile{name, std::move(*bytes)});
        }
    }
    if (find_file(files, "theif" + std::to_string(slot)) == nullptr) {
        return ""; // no character in the slot
    }
    const fs::path parent = utf8_path(loc.backups_dir) / std::to_string(slot);
    const std::vector<fs::path> earlier = backup_dirs(parent);
    if (!earlier.empty() && read_save_dir(path_utf8(earlier.front())) == files) {
        return ""; // the newest backup holds these bytes already
    }
    const std::string made = write_backup(parent, stamp, files, why);
    if (!made.empty()) {
        rotate_backups(path_utf8(parent), loc.keep);
    }
    return made;
}

std::vector<std::string> backup_changed_slots(const SaveLocation &loc, const std::string &stamp) {
    std::vector<std::string> made;
    for (int slot = 0; slot < 3; slot++) {
        std::string why;
        const std::string dir = backup_slot(loc, slot, stamp, &why);
        if (!dir.empty()) {
            made.push_back(dir);
        } else if (!why.empty()) {
            SDL_Log("saves: no backup of slot %d: %s", slot, why.c_str());
        }
    }
    return made;
}

bool restore_backup(const SaveLocation &loc, const BackupInfo &backup, const std::string &stamp,
                    ImportResult *result, std::string *why) {
    return import_saves(loc, backup.path, stamp, result, why);
}

std::string backup_stamp(std::chrono::system_clock::time_point when) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(when.time_since_epoch());
    const std::time_t seconds = static_cast<std::time_t>(ms.count() / 1000);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &seconds);
#else
    gmtime_r(&seconds, &tm);
#endif
    char text[80]; // room for any int in each field: GCC's -Wformat-truncation counts them
    std::snprintf(text, sizeof text, "%04d%02d%02d-%02d%02d%02d-%03d", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<int>(ms.count() % 1000));
    return text;
}

std::string backup_stamp_now() { return backup_stamp(std::chrono::system_clock::now()); }

namespace {

// CDataMgr::SaveQuestData (data manager, hero): run it, then back up. The hook never reads or
// writes guest state; it only looks at the files the pair just wrote.
int save_quest_data_hook(mt_cpu *cpu, const mt_hook_call *call) {
    call->original(cpu);
    try {
        for (const std::string &dir : save_backups_after_save()) {
            SDL_Log("saves: backup %s", dir.c_str());
        }
    } catch (...) {
        SDL_Log("saves: the backup after a save failed"); // never into the generated code
    }
    return MT_HOOK_HANDLED;
}

} // namespace

std::vector<std::string> save_backups_after_save() {
    return backup_changed_slots(save_location(), backup_stamp_now());
}

void install_save_backups() {
    const SaveLocation loc = save_location();
    const bool on = loc.keep > 0 && !loc.backups_dir.empty();
    mt_hook_register(MT_HOOK_FN_SAVE_QUEST_DATA, &save_quest_data_hook, nullptr, on ? 1 : 0);
    if (on) {
        SDL_Log("saves: after each save, a backup of the character in %s (the newest %u kept)",
                loc.backups_dir.c_str(), static_cast<unsigned>(loc.keep));
    } else {
        SDL_Log("saves: no backups (save_backups = %u)", static_cast<unsigned>(loc.keep));
    }
}

// --- The settings menu's Saves actions ---------------------------------------------------------

bool export_saves(const std::string &dest, std::string *why) {
    return export_saves(save_location(), dest, nullptr, why);
}

bool import_saves(const std::string &source, std::string *why) {
    return import_saves(save_location(), source, backup_stamp_now(), nullptr, why);
}

std::vector<BackupInfo> list_backups() { return list_backups(save_location()); }

bool restore_backup(const BackupInfo &backup, std::string *why) {
    return restore_backup(save_location(), backup, backup_stamp_now(), nullptr, why);
}

} // namespace maplethief::frontend
