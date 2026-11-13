// The save tools (#148, ADR 0078): where the saves live (save_dir), export,
// import, rotating backups and the checks a set of save files gets before it replaces the
// player's. The game's own files (docs/formats/save.md) are only ever written through
// mt_saves_replace_file (maplethief/saves.h, ADR 0074): a crash leaves each file whole.
//
// Folders, under the data folder (data_location.h, ADR 0127: MAPLETHIEF_DATA_DIR, else
// <base>/data next to the game, else the per-user folder):
//
//   saves/                     the game's files and gamecenter.json (save_dir moves this folder)
//   backups/<slot>/<time>/     theif<slot> and quest<slot>, after the game saved them (slot 0-2)
//   backups/import/<time>/     the files an import or a restore replaced
//
// <time> is UTC, "YYYYMMDD-HHMMSS-mmm" (a "-N" suffix when two fall in one millisecond), so a
// folder's name sorts by age. Only folders named like that are ever rotated away.
//
// Two layers: functions that take a SaveLocation (tests, the command line) and the menu's
// (#89) entry points below them, which use the location configure_save_location set.
//
// Not thread-safe; the guest thread (the backup hook) and the frontend thread (the menu) must
// not run them at once. The menu runs on the frontend thread between turns.
#ifndef MAPLETHIEF_PLATFORM_SAVE_TOOLS_H
#define MAPLETHIEF_PLATFORM_SAVE_TOOLS_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "data_location.h"
#include "settings.h"

namespace maplethief::frontend {

inline constexpr const char *kBackupsDirName = "backups";
inline constexpr const char *kImportBackupsName = "import";
// The largest file a save set may hold (anything near it is refused by the checks anyway).
inline constexpr std::size_t kMaxSaveSourceFileBytes = 16u * 1024u * 1024u;
// The most a zip's save files may hold together, checked before any is inflated (#324).
inline constexpr std::size_t kMaxSaveSetBytes = 64u * 1024u * 1024u;

// --- Where -------------------------------------------------------------------------------------

struct SaveLocation {
    std::string data_dir;      // "" if there is none
    std::string documents_dir; // the game's Documents/ (mt_saves_documents_dir): saves/
    std::string backups_dir;   // <data dir>/backups; "" without a data directory
    std::uint32_t keep = 10;   // save_backups

    bool operator==(const SaveLocation &) const = default;
};

// Points the runtime at the saves for these settings, and remembers the location for the menu's
// functions and the backup hook: the data folder `data.dir` (mt_stubs_set_data_dir), the
// guest's Documents/ at settings.save_dir, else `data.saves` (mt_saves_set_documents_dir), and
// gamecenter.json in that folder (mt_stubs_set_gamecenter_path; on iOS in the data folder, ADR
// 0074). First moves an old <data dir>/gamecenter.json there (migrate_gamecenter, ADR 0127).
// Returns the location.
SaveLocation configure_save_location(const Settings &settings, const DataLocation &data);
// The location configure_save_location set (the runtime's defaults before it is called).
SaveLocation save_location();

// --- Sets of save files ------------------------------------------------------------------------

struct SaveFile {
    std::string name; // a game file name: theif0, config, ... (mt_saves_is_game_file)
    std::vector<std::uint8_t> bytes;

    bool operator==(const SaveFile &) const = default;
};
using SaveSet = std::vector<SaveFile>; // by name

// One file's (or the set's) broken rule.
struct SaveProblem {
    std::string file; // the file's name, or "set" (bad_pair)
    std::string code; // mt_saves_verdict_name's, or "bad_pair"
    std::string why;  // plain words

    // "theif0: bad_size: theif0 has 1000 bytes; ..."
    std::string line() const;
};

// The game's files in `source`: a folder (its Documents/ folder if it holds none itself), or a
// zip file (entries in any folder, matched by their file name; __MACOSX and dot files left out).
// Other names go to `ignored`, and so does a symbolic link in a folder ("<name> (a symbolic
// link: not followed)", logged; #413); *.mt-tmp files are skipped silently. False, with `why`, if
// `source` cannot be read, is neither, or a zip names one file twice.
bool read_save_source(const std::string &source, SaveSet *files, std::vector<std::string> *ignored,
                      std::string *why);
// The game's files in the folder `dir` (no Documents/ fallback; "" or missing: none).
SaveSet read_save_dir(const std::string &dir);

// Every broken rule of the set: mt_saves_check per file (by name), then theif<n> and quest<n>
// as pairs ("set", "bad_pair"). Empty if the game can load every file.
std::vector<SaveProblem> check_save_set(const SaveSet &files);

// --- Export and import ---------------------------------------------------------------------------

// Copies every game file in loc.documents_dir to `dest`: a zip file if `dest` ends in ".zip"
// (any case; stored, the names at its root), else a folder (created; files of the same names
// replaced, others left). Exports the files as they are, without checking them. False, with
// `why`, if there is nothing to export, `dest` is Documents/ itself, or a write fails.
bool export_saves(const SaveLocation &loc, const std::string &dest,
                  std::vector<std::string> *exported, std::string *why);

struct ImportResult {
    std::vector<std::string> imported; // the files written
    std::vector<std::string> ignored;  // names in the source that are not the game's
    std::string backup;                // the folder with the files it replaced ("" if none)
    std::vector<SaveProblem> problems; // why it was refused
    std::vector<std::string> removed;  // stale cash shop counters taken away (see below)
};

// Imports a set of save files (read_save_source) into loc.documents_dir. Nothing is written
// unless every file passes check_save_set and the files it replaces were first copied to
// backups/import/<stamp>/ (the newest max(loc.keep, 1) such folders are kept). The files it
// holds replace those of the same names; the others stay, except a slot's cash shop counters
// (maplethief-cashshop<n>, #156) when theif<n> comes without them: those belonged to the
// character replaced, so they are backed up and removed. A failed write puts back what was
// written. False, with `why` (and result->problems for a refused set), otherwise.
bool import_saves(const SaveLocation &loc, const std::string &source, const std::string &stamp,
                  ImportResult *result, std::string *why);

// --- Backups -----------------------------------------------------------------------------------

struct BackupInfo {
    std::string path;  // the folder
    std::string kind;  // "0", "1", "2" (a slot's) or "import"
    std::string stamp; // the folder's name
    std::vector<std::string> files;

    bool operator==(const BackupInfo &) const = default;
};

// Every backup, newest first within each kind (slots 0-2, then imports).
std::vector<BackupInfo> list_backups(const SaveLocation &loc);

// Copies theif<slot> and quest<slot> to backups/<slot>/<stamp>/ unless the slot's newest backup
// holds the same bytes, then keeps the newest loc.keep. Nothing if keep is 0, there is no
// backups folder or theif<slot> does not exist. Returns the folder made ("" if none); `why` on
// a failed write.
std::string backup_slot(const SaveLocation &loc, int slot, const std::string &stamp,
                        std::string *why);
// backup_slot for every slot; the folders made.
std::vector<std::string> backup_changed_slots(const SaveLocation &loc, const std::string &stamp);

// Removes all but the newest `keep` backup folders (named like a stamp) in `dir`.
void rotate_backups(const std::string &dir, std::uint32_t keep);

// Puts a backup's files back: an import of its folder (checked, and what it replaces backed up).
bool restore_backup(const SaveLocation &loc, const BackupInfo &backup, const std::string &stamp,
                    ImportResult *result, std::string *why);

// A backup folder's name for a moment: UTC "YYYYMMDD-HHMMSS-mmm".
std::string backup_stamp(std::chrono::system_clock::time_point when);
// backup_stamp of now.
std::string backup_stamp_now();

// The backup hook (ADR 0069): wraps CDataMgr::SaveQuestData 0x38F55, the second of the game's
// save pair, and after it returns backs up every slot whose files changed (backup_slot) at the
// configured location. Registered on (save_backups > 0 and a backups folder) or off. Also
// registers the refused-save hook (save_refused_text).
void install_save_backups();
// The hook's work after the pair: backup_changed_slots(save_location(), now) (tests).
std::vector<std::string> save_backups_after_save();

// A theif<N> or quest<N> the runtime refused as the game read it (#413, ADR 0154 (a),
// maplethief/saves.h): the notice's words. install_save_backups registers the runtime's hook,
// which queues them once per refused file (data_notice.h); the game shows them during play.
std::string save_refused_text(const std::string &name, const std::string &why,
                              const std::string &moved_to);
// The hook itself (mt_saves_refused_fn): queues save_refused_text.
void save_refused_hook(const char *name, const char *why, const char *moved_to, void *user);

// --- The settings menu's Saves actions (#89) on the configured location ----------------------

// Export to a zip file or a folder; import (checked, backed up first); list and restore backups.
// `why` says what went wrong in a line the menu can show.
bool export_saves(const std::string &dest, std::string *why);
bool import_saves(const std::string &source, std::string *why);
std::vector<BackupInfo> list_backups();
bool restore_backup(const BackupInfo &backup, std::string *why);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_SAVE_TOOLS_H
