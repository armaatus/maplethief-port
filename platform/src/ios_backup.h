// The game's saves out of an iPhone or iPod backup, read by the game itself (#374, ADR 0078's
// addendum; docs/formats/ios-backup.md). The C++ counterpart of tools/mtport/iosbackup.py's
// read_app_documents, with the same rules, so the Saves tab imports a backup without Python:
//
//   - the folder: Manifest.plist (IsEncrypted), Info.plist (Device Name, Product Version),
//     Status.plist (SnapshotState), and the file list in Manifest.db (iOS 10+, SQLite, read by
//     sqlite_read.h) or Manifest.mbdb (iOS 4-9). Manifest.db wins when both are there.
//   - the game's files are those of AppDomain-com.nexonmobile.maplestory.thief whose
//     relativePath is exactly Documents/<name> with <name> one of the game's
//     (mt_saves_is_game_file); a file's data is <id[:2]>/<id> (db) or <id> (mbdb), <id> the
//     SHA-1 of "<domain>-<relativePath>".
//   - refused: an encrypted backup (IsEncrypted, a Manifest.db that is not SQLite, an mbdb
//     record with a key); a game file listed twice, or as a folder or link; a file ID that is
//     not 40 lowercase hex digits; a data file (or its two-digit folder) that is a link, is not
//     a regular file, is over kMaxSaveSourceFileBytes or (mbdb) does not have the record's size.
//     Other names in Documents/ are listed as ignored, never written.
//
// The files found go to the import (save_tools.h's import_save_set), which checks them again
// (check_save_set) and backs up what they replace, as every import does.
#ifndef MAPLETHIEF_PLATFORM_IOS_BACKUP_H
#define MAPLETHIEF_PLATFORM_IOS_BACKUP_H

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "save_tools.h"

namespace maplethief::frontend {

inline constexpr const char *kIosAppId = "com.nexonmobile.maplestory.thief"; // ANALYSIS.md
inline constexpr std::size_t kMaxIosPlistBytes = 64u * 1024u * 1024u;
inline constexpr std::size_t kMaxIosMbdbBytes = 512u * 1024u * 1024u;
inline constexpr std::size_t kMaxIosAppRows = 100000; // rows of the game's domain

// What a backup's folder says about itself.
struct IosBackupInfo {
    std::string path;
    std::string manifest; // "db", "mbdb" or "" (not a backup this reads)
    bool encrypted = false;
    std::string device;      // Info.plist's Device Name ("" if none)
    std::string ios;         // Info.plist's Product Version
    bool unfinished = false; // Status.plist's SnapshotState is there and not "finished"
    std::string problem;     // why it cannot be read, when known up front
    std::int64_t time = 0;   // the manifest's modification time, seconds since 1970 (0: unknown)
};

// The game's files found in a backup.
struct IosBackupFiles {
    IosBackupInfo info;
    SaveSet files;                     // by name
    std::vector<std::string> ignored;  // other names in the game's Documents/
    std::vector<std::string> warnings; // "Status.plist says this backup did not finish"
};

// SHA-1 of `bytes` (a backup's file IDs).
std::array<std::uint8_t, 20> sha1_of(std::span<const std::uint8_t> bytes);
// A backup's name for a file's data: SHA-1 of "<domain>-<relative path>", 40 lowercase hex.
std::string ios_file_id(std::string_view domain, std::string_view relative_path);

// Whether `dir` holds Manifest.db, Manifest.mbdb or Manifest.plist.
bool looks_like_ios_backup(const std::string &dir);
// What the backup in `dir` is; problem says why when its plists cannot be read or it has no
// manifest.
IosBackupInfo ios_backup_info(const std::string &dir);
// Every backup in `roots`: a root that is one, or each folder in it that is one (not links).
std::vector<IosBackupInfo> find_ios_backups(const std::vector<std::string> &roots);
// Where Finder, iTunes and Apple Devices keep backups on this computer (macOS, Windows; none
// elsewhere). `home` and `appdata` are $HOME / %USERPROFILE% and %APPDATA% ("" read from the
// environment).
std::vector<std::string> default_ios_backup_roots(const std::string &home = "",
                                                  const std::string &appdata = "");

// The game's files in the backup at `dir`; false, with `why` in plain words, if the backup
// cannot be used (see above). The files are not checked here.
bool read_ios_backup(const std::string &dir, IosBackupFiles *out, std::string *why);

// A backup's name for the menu and the command line: "<device>, iOS <version>" ("a device"
// without Info.plist's name).
std::string ios_backup_label(const IosBackupInfo &b);

// The words for an encrypted backup: how to make an unencrypted one.
std::string ios_encrypted_text();

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_IOS_BACKUP_H
