// The data folder (#261, ADR 0127); see data_location.h. The only reader of MAPLETHIEF_DATA_DIR
// and the only caller of SDL_GetPrefPath (scripts/lint).
#include "data_location.h"

#include <SDL3/SDL.h>

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "maplethief/saves.h"

namespace maplethief::frontend {

namespace fs = std::filesystem;

namespace {

// A path from a UTF-8 string, and back (on Windows a narrow string is not UTF-8).
fs::path utf8_path(const std::string &s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string path_utf8(const fs::path &p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string strip_separators(std::string s) {
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) {
        s.pop_back();
    }
    return s;
}

std::string join(const std::string &dir, const char *name) {
    return dir.empty() ? std::string() : path_utf8(utf8_path(dir) / name);
}

unsigned long process_id() {
#if defined(_WIN32)
    return static_cast<unsigned long>(_getpid());
#else
    return static_cast<unsigned long>(getpid());
#endif
}

std::string errno_text(int err) { return std::generic_category().message(err); }

// The resolver's cache (data_location()) and --data-location.
struct State {
    std::optional<DataPlace> flag;
    std::optional<DataLocation> resolved;
};
State &state() {
    static State s;
    return s;
}

} // namespace

const char *data_place_name(DataPlace place) {
    switch (place) {
    case DataPlace::kGame:
        return "game";
    case DataPlace::kUser:
        return "user";
    case DataPlace::kEnv:
        return "env";
    case DataPlace::kIos:
        return "ios";
    case DataPlace::kSwitch:
        return "switch";
    case DataPlace::kNone:
        break;
    }
    return "none";
}

const char *data_reason_name(DataReason reason) {
    switch (reason) {
    case DataReason::kDefault:
        return "default";
    case DataReason::kFlag:
        return "flag";
    case DataReason::kMarker:
        return "marker";
    case DataReason::kApplicationsFolder:
        return "applications-folder";
    case DataReason::kFallbackTranslocated:
        return "fallback-translocated";
    case DataReason::kFallbackNotWritable:
        return "fallback-not-writable";
    case DataReason::kFallbackNoBase:
        return "fallback-no-base";
    case DataReason::kEnv:
        return "env";
    case DataReason::kPlatform:
        return "platform";
    case DataReason::kNone:
        break;
    }
    return "none";
}

std::optional<DataPlace> parse_data_place(std::string_view text) {
    if (text == "game") {
        return DataPlace::kGame;
    }
    if (text == "user") {
        return DataPlace::kUser;
    }
    return std::nullopt;
}

std::string DataLocation::bundle_parent() const {
    return bundle_dir.empty() ? std::string() : path_utf8(utf8_path(bundle_dir).parent_path());
}

bool DataLocation::fallback() const {
    return reason == DataReason::kFallbackTranslocated ||
           reason == DataReason::kFallbackNotWritable || reason == DataReason::kFallbackNoBase;
}

DataLocation data_location_at(const std::string &dir_in, DataPlace place, DataReason reason,
                              const std::string &base) {
    DataLocation loc;
    loc.place = place;
    loc.reason = reason;
    loc.base = base.empty() ? std::string() : strip_separators(base);
    loc.dir = dir_in.empty() ? std::string() : strip_separators(dir_in);
    loc.saves = join(loc.dir, kSavesDirName);
    loc.backups = join(loc.dir, "backups");
    loc.states = join(loc.dir, kStatesDirName);
    loc.screenshots = join(loc.dir, kScreenshotsDirName);
    loc.exports = join(loc.dir, kExportsDirName);
    loc.import = join(loc.dir, kImportDirName);
    loc.logs = join(loc.dir, kLogsDirName);
    loc.crashes = join(loc.dir, kCrashesDirName);
    loc.bundle_dir = place == DataPlace::kGame || place == DataPlace::kSwitch
                         ? join(loc.base, kBundleDirName)
                         : join(loc.dir, kBundleDirName);
    if (!loc.base.empty()) {
        loc.placed_bundle = path_utf8(utf8_path(loc.base) / kBundleDirName / "MapleThief.app");
    }
    return loc;
}

std::string portable_base_dir(std::string_view base_path) {
    std::string base = strip_separators(std::string(base_path));
    // Inside an app bundle: the folder around the .app.
    std::size_t app = base.rfind(".app/Contents");
    if (app == std::string::npos) {
        app = base.rfind(".app\\Contents"); // either separator (a test on Windows)
    }
    if (app != std::string::npos) {
        const std::size_t slash = base.find_last_of("/\\", app);
        return slash == std::string::npos ? std::string(".") : base.substr(0, slash);
    }
    return base;
}

std::string data_base_dir(std::string_view base_path) {
    if (base_path.empty()) {
        return "";
    }
    const std::string base = portable_base_dir(base_path);
    // The Linux game folder: MapleThief.sh beside bin/MapleThief (ADR 0065).
    const fs::path p = utf8_path(base);
    if (p.filename() == "bin") {
        std::error_code ec;
        if (fs::is_regular_file(p.parent_path() / "MapleThief.sh", ec)) {
            return path_utf8(p.parent_path());
        }
    }
    return base;
}

bool probe_data_dir(const std::string &data_dir, std::string *why) {
    const auto fail = [why](const std::string &what) {
        if (why != nullptr) {
            *why = what;
        }
        return false;
    };
    if (data_dir.empty()) {
        return fail("no folder");
    }
    std::error_code ec;
    const fs::path dir = utf8_path(data_dir);
    fs::create_directories(dir, ec);
    if (ec) {
        return fail("cannot create " + data_dir + ": " + ec.message());
    }
    if (!fs::is_directory(dir, ec)) {
        return fail(data_dir + " is not a folder");
    }
    char name[48];
    std::snprintf(name, sizeof name, ".mt-probe-%lu", process_id());
    const fs::path file = dir / name;
#if defined(_WIN32)
    int fd = -1;
    const errno_t open_err =
        _wsopen_s(&fd, file.c_str(), _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY, _SH_DENYNO,
                  _S_IREAD | _S_IWRITE);
    if (open_err != 0 || fd < 0) {
        return fail("cannot create a file in " + data_dir + ": " + errno_text(open_err));
    }
    const char byte = 'm';
    const bool wrote = _write(fd, &byte, 1) == 1 && _commit(fd) == 0;
    const int write_err = errno;
    const bool closed = _close(fd) == 0;
#else
    const int fd = ::open(file.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd < 0) {
        return fail("cannot create a file in " + data_dir + ": " + errno_text(errno));
    }
    const char byte = 'm';
    const bool wrote = ::write(fd, &byte, 1) == 1 && ::fsync(fd) == 0;
    const int write_err = errno;
    const bool closed = ::close(fd) == 0;
#endif
    if (!wrote || !closed) {
        fs::remove(file, ec);
        return fail("cannot write a file in " + data_dir + ": " + errno_text(write_err));
    }
    if (!fs::remove(file, ec) || ec) {
        return fail("cannot remove a file in " + data_dir + ": " + ec.message());
    }
    return true;
}

DataInputs current_data_inputs() {
    DataInputs in;
    if (const char *env = SDL_getenv("MAPLETHIEF_DATA_DIR"); env != nullptr && *env != '\0') {
        in.env = env;
    }
    in.flag = state().flag;
#if defined(SDL_PLATFORM_SWITCH)
    // The SD card's fixed folder (ADR 0138): SDL's base path is romfs:/ and its pref path the
    // working directory, neither of them the player's, so neither is asked (in.user_dir empty).
    in.os = DataInputs::Os::kSwitch;
#else
    if (const char *base = SDL_GetBasePath(); base != nullptr) {
        in.base_path = base;
    }
    if (const char *home = SDL_GetUserFolder(SDL_FOLDER_HOME); home != nullptr) {
        in.home = strip_separators(home);
    }
#if defined(SDL_PLATFORM_IOS)
    in.os = DataInputs::Os::kIos;
    if (const char *docs = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS); docs != nullptr) {
        in.ios_documents = strip_separators(docs);
    }
#elif defined(SDL_PLATFORM_MACOS)
    in.os = DataInputs::Os::kMacos;
#elif defined(SDL_PLATFORM_WINDOWS)
    in.os = DataInputs::Os::kWindows;
#elif defined(SDL_PLATFORM_LINUX)
    in.os = DataInputs::Os::kLinux;
#endif
    in.user_dir = [] { return user_data_dir(true); };
#endif
    return in;
}

DataLocation resolve_data_location(const DataInputs &in) {
    // 1. MAPLETHIEF_DATA_DIR, everywhere.
    if (!in.env.empty()) {
        return data_location_at(in.env, DataPlace::kEnv, DataReason::kEnv,
                                in.os == DataInputs::Os::kIos ? std::string()
                                                              : data_base_dir(in.base_path));
    }
    const auto user = [&in](DataReason reason, const std::string &base) {
        const std::string dir = in.user_dir ? in.user_dir() : std::string();
        if (dir.empty()) {
            DataLocation none = data_location_at("", DataPlace::kNone, DataReason::kNone, base);
            none.why = "SDL gives no per-user folder";
            return none;
        }
        return data_location_at(dir, DataPlace::kUser, reason, base);
    };
    // iOS: unchanged (ADR 0074): the app's folder, the saves in its own Documents/.
    if (in.os == DataInputs::Os::kIos) {
        const std::string dir = in.user_dir ? in.user_dir() : std::string();
        if (dir.empty()) {
            return data_location_at("", DataPlace::kNone, DataReason::kNone);
        }
        DataLocation loc = data_location_at(dir, DataPlace::kIos, DataReason::kPlatform);
        if (!in.ios_documents.empty()) {
            loc.saves = strip_separators(in.ios_documents);
        }
        return loc;
    }
    // The Nintendo Switch: the SD card's folder, always (ADR 0138).
    if (in.os == DataInputs::Os::kSwitch) {
        return data_location_at(join(kSwitchBaseDir, kDataDirName), DataPlace::kSwitch,
                                DataReason::kPlatform, kSwitchBaseDir);
    }
    const std::string base = data_base_dir(in.base_path);
    bool portable = false;
    bool marker = false;
    if (!base.empty()) {
        std::error_code ec;
        portable = fs::is_regular_file(utf8_path(base) / kPortableFileName, ec);
        marker = fs::exists(utf8_path(base) / kUserDataMarkerName, ec);
    }
    const auto with_portable = [portable](DataLocation loc) {
        loc.portable_marker = portable;
        return loc;
    };
    const std::string game_dir = join(base, kDataDirName);
    // The game folder, if the probe passes; else the fallback with its reason.
    const auto game = [&](DataReason reason) {
        if (base.empty()) {
            return with_portable(user(DataReason::kFallbackNoBase, base));
        }
        if (in.os == DataInputs::Os::kMacos &&
            in.base_path.find("/AppTranslocation/") != std::string::npos) {
            DataLocation loc = user(DataReason::kFallbackTranslocated, base);
            loc.why = "App Translocation runs the app from a read-only copy";
            return with_portable(loc);
        }
        std::string why;
        const bool ok = in.probe ? in.probe(game_dir, &why) : probe_data_dir(game_dir, &why);
        if (!ok) {
            DataLocation loc = user(DataReason::kFallbackNotWritable, base);
            loc.why = why;
            return with_portable(loc);
        }
        return with_portable(data_location_at(game_dir, DataPlace::kGame, reason, base));
    };
    // 2. --data-location for this run.
    if (in.flag == DataPlace::kGame) {
        return game(DataReason::kFlag);
    }
    if (in.flag == DataPlace::kUser) {
        return with_portable(user(DataReason::kFlag, base));
    }
    // 3. user-data.txt next to the game.
    if (marker) {
        return with_portable(user(DataReason::kMarker, base));
    }
    // 4. macOS: the app alone in an Applications folder.
    if (in.os == DataInputs::Os::kMacos && !base.empty() &&
        (base == "/Applications" || (!in.home.empty() && base == in.home + "/Applications"))) {
        return with_portable(user(DataReason::kApplicationsFolder, base));
    }
    // 5 and 6. The game folder, or the fallback.
    return game(DataReason::kDefault);
}

void set_data_location_flag(std::optional<DataPlace> flag) {
    state().flag = flag;
    state().resolved.reset();
}

const DataLocation &data_location() {
    State &s = state();
    if (!s.resolved) {
        s.resolved = resolve_data_location(current_data_inputs());
        for (const std::string &line : data_location_log(*s.resolved)) {
            SDL_Log("%s", line.c_str());
        }
    }
    return *s.resolved;
}

void set_data_location_for_test(std::optional<DataLocation> loc) {
    state().resolved = std::move(loc);
}

std::string user_data_dir(bool keep) {
    char *pref = SDL_GetPrefPath("", kPrefPathApp);
    if (pref == nullptr) {
        return "";
    }
    const std::string dir = strip_separators(pref);
    SDL_free(pref);
    if (!keep) {
        std::error_code ec;
        const fs::path p = utf8_path(dir);
        if (fs::is_directory(p, ec) && fs::is_empty(p, ec) && !ec) {
            fs::remove(p, ec);
        }
    }
    return dir;
}

std::string describe_data_location(const DataLocation &loc) {
    std::string text;
    text += "data: " + loc.dir + "\n";
    text += std::string("location: ") + data_place_name(loc.place) + "\n";
    text += std::string("reason: ") + data_reason_name(loc.reason) + "\n";
    if (!loc.why.empty()) {
        text += "why: " + loc.why + "\n";
    }
    text += "saves: " + loc.saves + "\n";
    text += "game: " + loc.bundle_dir + "\n";
    return text;
}

std::vector<std::string> data_location_log(const DataLocation &loc) {
    std::vector<std::string> lines;
    lines.push_back("data: " + (loc.dir.empty() ? std::string("none") : loc.dir) + " (" +
                    data_reason_name(loc.reason) + ")");
    if (loc.fallback()) {
        const std::string game_dir =
            loc.base.empty() ? std::string("<base>/data") : join(loc.base, kDataDirName);
        lines.push_back("data: " + game_dir + " can't be written (" + loc.why +
                        "); using the user folder " + loc.dir);
    } else if (loc.place == DataPlace::kNone && !loc.why.empty()) {
        lines.push_back("data: no data folder: " + loc.why);
    }
    if (loc.portable_marker) {
        lines.emplace_back("data: portable.txt is no longer needed: the data folder is next to "
                           "the game by default");
    }
    return lines;
}

std::vector<std::string> migrate_data_layout(const DataLocation &loc) {
    std::vector<std::string> lines;
    if (loc.dir.empty()) {
        return lines;
    }
    std::error_code ec;
    const fs::path dir = utf8_path(loc.dir);
    const fs::path documents = dir / kOldDocumentsDirName;
    const fs::path saves = dir / kSavesDirName;
    // 1. Documents/ -> saves/ (one rename in one folder).
    if (fs::is_directory(documents, ec)) {
        if (!fs::exists(saves, ec)) {
            fs::rename(documents, saves, ec);
            lines.push_back(
                ec ? "data: cannot rename " + path_utf8(documents) + " to saves/: " + ec.message()
                   : "data: migrated " + path_utf8(documents) + " to " + path_utf8(saves));
        } else {
            lines.push_back("data: both " + path_utf8(documents) + " and " + path_utf8(saves) +
                            " exist: saves/ is used, Documents/ is left alone");
        }
    }
    // 3. Location game: <base>/data/game -> <base>/game.
    if (loc.place == DataPlace::kGame && !loc.base.empty()) {
        const fs::path old_game = dir / kBundleDirName;
        const fs::path new_game = utf8_path(loc.base) / kBundleDirName;
        ec.clear();
        if (fs::is_directory(old_game, ec) && !fs::exists(new_game, ec)) {
            fs::rename(old_game, new_game, ec);
            lines.push_back(ec ? "data: cannot move " + path_utf8(old_game) + " to " +
                                     path_utf8(new_game) + ": " + ec.message()
                               : "data: moved the imported game " + path_utf8(old_game) + " to " +
                                     path_utf8(new_game));
        }
    }
    return lines;
}

std::string migrate_gamecenter(const std::string &data_dir, const std::string &saves_dir) {
    if (data_dir.empty() || saves_dir.empty()) {
        return "";
    }
    std::error_code ec;
    const fs::path from = utf8_path(data_dir) / kGameCenterFileName;
    const fs::path to = utf8_path(saves_dir) / kGameCenterFileName;
    if (!fs::is_regular_file(from, ec) || fs::equivalent(from, to, ec)) {
        return "";
    }
    const auto read = [](const fs::path &p, std::string *bytes) {
        std::ifstream in(p, std::ios::binary);
        bytes->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return !in.bad() && in.is_open();
    };
    std::string bytes;
    if (fs::exists(to, ec)) {
        // A copy that was interrupted before the source went: the same bytes, so finish it.
        std::string there;
        if (read(from, &bytes) && read(to, &there) && bytes == there) {
            fs::remove(from, ec);
            return "data: removed " + path_utf8(from) + ", already copied into " + saves_dir;
        }
        return "";
    }
    fs::create_directories(to.parent_path(), ec);
    ec.clear();
    fs::rename(from, to, ec);
    if (!ec) {
        return "data: moved " + path_utf8(from) + " into " + saves_dir;
    }
    // Across volumes (a save_dir elsewhere): an atomic copy, then the source removed.
    if (!read(from, &bytes)) {
        return "data: cannot read " + path_utf8(from);
    }
    if (mt_saves_replace_file(path_utf8(to).c_str(), bytes.data(), bytes.size()) != 0) {
        return "data: cannot copy " + path_utf8(from) + " into " + saves_dir;
    }
    fs::remove(from, ec);
    return "data: copied " + path_utf8(from) + " into " + saves_dir;
}

} // namespace maplethief::frontend
