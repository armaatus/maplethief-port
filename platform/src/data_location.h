// The data folder (#261, ADR 0127): the one resolver that decides where everything the port
// writes for a player goes, the folder's layout, and the in-place migration of an old layout.
//
// It is the only code that reads MAPLETHIEF_DATA_DIR and the only caller of SDL_GetPrefPath
// (scripts/lint checks). The first rule that applies wins:
//
//   1. MAPLETHIEF_DATA_DIR (set and not empty)          location env,  reason env
//   2. --data-location game|user (this run)             location game|user, reason flag
//   3. <base>/user-data.txt                              location user, reason marker
//   4. macOS: <base> is /Applications or ~/Applications  location user, reason applications-folder
//   5. <base>/data, if the probe can create and remove a file in it
//                                                        location game, reason default
//   6. otherwise the per-user folder (SDL_GetPrefPath("", "MapleThief")), with the reason
//      fallback-no-base (no SDL_GetBasePath), fallback-translocated (macOS App Translocation,
//      decided by the path) or fallback-not-writable (the probe failed; `why` is its error)
//
// iOS: the app's per-user folder, the saves in its own Documents/ (ADR 0074): location ios,
// reason platform. MAPLETHIEF_DATA_DIR still wins there (the tests' data directory).
// The Nintendo Switch (ADR 0138, #238): <base> is kSwitchBaseDir on the SD card, the data folder
// its data/ and the bundle folder its game/: location switch, reason platform. Neither
// SDL_GetBasePath (romfs:/ there) nor SDL_GetPrefPath (the working directory) is asked, and no
// flag, marker or probe applies. MAPLETHIEF_DATA_DIR still wins.
//
// <base> is SDL_GetBasePath()'s folder: the folder of MapleThief.exe; the folder around
// MapleThief.app (never inside it); on Linux the game folder, the parent of bin/ when it holds
// MapleThief.sh. portable.txt there is accepted and does nothing (logged once).
//
// The layout under the data folder (ADR 0127's *Layout*):
//
//   settings.ini  bindings.ini  window.ini
//   saves/          the guest's <home>/Documents (the game's files) and gamecenter.json
//   backups/  states/  screenshots/  exports/  import/  logs/  crashes/
//
// The bundle folder (the IPA import's game/) is <base>/game for the location game, and the data
// folder's own game/ otherwise.
#ifndef MAPLETHIEF_PLATFORM_DATA_LOCATION_H
#define MAPLETHIEF_PLATFORM_DATA_LOCATION_H

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace maplethief::frontend {

inline constexpr const char *kDataDirName = "data";
inline constexpr const char *kSavesDirName = "saves";
inline constexpr const char *kOldDocumentsDirName = "Documents"; // the layout before ADR 0127
inline constexpr const char *kStatesDirName = "states";
inline constexpr const char *kScreenshotsDirName = "screenshots";
inline constexpr const char *kExportsDirName = "exports";
inline constexpr const char *kImportDirName = "import";
inline constexpr const char *kLogsDirName = "logs";
inline constexpr const char *kCrashesDirName = "crashes";
inline constexpr const char *kBundleDirName = "game";
inline constexpr const char *kGameCenterFileName = "gamecenter.json";
inline constexpr const char *kUserDataMarkerName = "user-data.txt";
inline constexpr const char *kPortableFileName = "portable.txt"; // accepted, does nothing
inline constexpr const char *kPrefPathApp = "MapleThief";        // the per-user folder's name
// The Nintendo Switch's <base> on the SD card (ADR 0138): MapleThief.nro, game/ and data/.
inline constexpr const char *kSwitchBaseDir = "sdmc:/switch/maplethief";

// Where the data folder is (ADR 0127's table).
enum class DataPlace { kGame, kUser, kEnv, kIos, kSwitch, kNone };
// Why it is there.
enum class DataReason {
    kDefault,
    kFlag,
    kMarker,
    kApplicationsFolder,
    kFallbackTranslocated,
    kFallbackNotWritable,
    kFallbackNoBase,
    kEnv,
    kPlatform,
    kNone,
};
// "game", "user", "env", "ios", "switch", "none".
const char *data_place_name(DataPlace place);
// "default", "flag", "marker", "applications-folder", "fallback-translocated",
// "fallback-not-writable", "fallback-no-base", "env", "platform", "none".
const char *data_reason_name(DataReason reason);
// --data-location's values: "game" or "user"; nullopt otherwise.
std::optional<DataPlace> parse_data_place(std::string_view text);

// The resolver's answer. Every path is UTF-8, without a trailing separator; "" when there is
// no data folder (location none).
struct DataLocation {
    std::string dir; // the data folder
    DataPlace place = DataPlace::kNone;
    DataReason reason = DataReason::kNone;
    std::string why;  // a fallback's cause (the probe's error text); "" otherwise
    std::string base; // <base> ("" if SDL_GetBasePath gave nothing, and on iOS)
    // The subfolders of `dir`.
    std::string saves; // the guest's Documents/ and gamecenter.json (save_dir moves it)
    std::string backups, states, screenshots, exports, import, logs, crashes;
    std::string bundle_dir;       // the bundle folder: where the IPA import puts game/
    std::string placed_bundle;    // <base>/game/MapleThief.app, a hand-placed bundle ("" no base)
    bool portable_marker = false; // <base>/portable.txt is there (it does nothing)

    // The folder whose game/ is bundle_dir (the IPA import's `parent_dir`).
    std::string bundle_parent() const;
    // The per-user folder was taken because the game folder could not be used.
    bool fallback() const;
    bool operator==(const DataLocation &) const = default;
};

// The layout of data folder `dir` at `place` for `reason`, with `base` (may be ""): every
// subfolder, and the bundle folder (<base>/game for kGame and kSwitch, <dir>/game otherwise). Tests
// and callers that already know the folder use it ("" `dir`: an empty location).
DataLocation data_location_at(const std::string &dir, DataPlace place = DataPlace::kEnv,
                              DataReason reason = DataReason::kEnv, const std::string &base = "");

// <base> from SDL_GetBasePath()'s answer: portable_base_dir's folder (the folder around
// MapleThief.app inside a bundle), or the parent of a folder named bin/ that holds
// MapleThief.sh (the Linux game folder; checked on the disk). "" for "".
std::string data_base_dir(std::string_view base_path);
// The folder around MapleThief.app (".../MapleThief.app/Contents/Resources/" or
// ".../Contents/MacOS/"), or the folder itself, without a trailing separator.
std::string portable_base_dir(std::string_view base_path);

// The probe (ADR 0127's *Fallback*): creates `data_dir` with its parents, then
// <data_dir>/.mt-probe-<pid> exclusively, writes one byte, flushes, closes and removes it.
// False with the error text in `why` (if not null) on any failure.
bool probe_data_dir(const std::string &data_dir, std::string *why);

// What the resolver reads. current_data_inputs() fills it from the process; tests build one.
struct DataInputs {
    enum class Os { kMacos, kWindows, kLinux, kIos, kSwitch, kOther };
    std::string env;               // MAPLETHIEF_DATA_DIR ("": not set or empty)
    std::optional<DataPlace> flag; // --data-location (kGame or kUser)
    std::string base_path;         // SDL_GetBasePath()'s answer ("": none)
    std::string home;              // the user's home folder (macOS ~/Applications)
    Os os = Os::kOther;
    std::string ios_documents; // iOS: the app's Documents/ (SDL_FOLDER_DOCUMENTS)
    // The per-user folder (SDL_GetPrefPath, which creates it); called only when it is used.
    std::function<std::string()> user_dir;
    // The probe; probe_data_dir when empty.
    std::function<bool(const std::string &data_dir, std::string *why)> probe;
};
DataInputs current_data_inputs();

// The rules above on `in`. Runs the probe (which creates <base>/data) when the location would
// be game; calls in.user_dir only when the location is user (or ios).
DataLocation resolve_data_location(const DataInputs &in);

// --data-location for this run (main.cpp sets it before the first data_location()).
void set_data_location_flag(std::optional<DataPlace> flag);
// This process's data folder: resolved once (current_data_inputs()), then the same answer.
// The first call logs "data: <dir> (<reason>)", a fallback's line and portable.txt's line.
const DataLocation &data_location();
// Test seam: makes data_location() return `loc` (nullopt: resolve again on the next call).
void set_data_location_for_test(std::optional<DataLocation> loc);

// The per-user folder SDL_GetPrefPath("", "MapleThief") names (the old default; #262 copies
// from it), without a trailing separator; "" if SDL gives none. SDL creates it to answer, so
// when `keep` is false and the folder is empty afterwards it is removed again (only that
// folder, never anything in it).
std::string user_data_dir(bool keep);

// The lines --print-data-dir prints: data, location, reason, why (a fallback only), saves and
// game, each "key: value\n".
std::string describe_data_location(const DataLocation &loc);
// The log lines of a resolution: "data: <dir> (<reason>)", then for a fallback "data: <base>/data
// can't be written (<why>); using the user folder <dir>", then portable.txt's line.
std::vector<std::string> data_location_log(const DataLocation &loc);

// The in-place migration of an old layout (ADR 0127's *Migration* (a)), before anything opens a
// file. Each step checks its own precondition, so a second run does nothing and a run after an
// interrupted one finishes the job:
//   1. <dir>/Documents and no <dir>/saves: Documents/ renamed to saves/ (both: saves/ is used,
//      Documents/ left alone, a warning);
//   3. location game: <base>/data/game and no <base>/game: renamed to <base>/game.
// Step 2, gamecenter.json into saves/, is migrate_gamecenter: it needs save_dir, which is in
// settings.ini, so configure_save_location runs it (still before the guest opens a file).
// Returns what it did and warned, one line each (logged by the caller).
std::vector<std::string> migrate_data_layout(const DataLocation &loc);
// Step 2: <data_dir>/gamecenter.json and no <saves_dir>/gamecenter.json: moved into saves_dir
// (a rename, or a copy through mt_saves_replace_file and a removal when the rename fails, e.g.
// across volumes). A line for the log, "" when there was nothing to do.
std::string migrate_gamecenter(const std::string &data_dir, const std::string &saves_dir);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_DATA_LOCATION_H
