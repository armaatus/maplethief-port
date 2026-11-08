// Copying a player's data folder into another (#262, ADR 0127's *Migration* (b) and
// `--migrate-from`; #263's *Data location* switch uses it too).
//
// What is copied, from the old layout (Documents/, gamecenter.json at the top) or the new one
// (saves/ with gamecenter.json), into the new layout (data_location.h):
//
//   the save set           saves/ (or Documents/): the game's files (mt_saves_is_game_file),
//                          read and checked first as an import checks them (check_save_set:
//                          mt_saves_check per file and the theif/quest pairs). A set that fails,
//                          or a game file that is a link, too large or unreadable, means nothing
//                          at all is copied.
//   gamecenter.json        into saves/
//   settings.ini  bindings.ini  window.ini
//   backups/ states/ screenshots/ exports/ import/    byte for byte
//
// *.mt-tmp files, links, other non-regular files and names the port doesn't know are skipped
// (listed in `skipped`); logs/, crashes/ and the bundle's game/ are not player data.
//
// The staging: what the destination already holds is copied into "<dst>.tmp-<id>" first, then
// the source over it; a file the source replaces (other bytes) goes to backups/import/<time>/
// in the staging folder first, as an import's would, so nothing is lost. Then <dst> goes aside
// to "<dst>.old-<id>", the staging folder is renamed to <dst>, and the old one removed (put back
// if the rename fails). At the start of each copy, leftover "<dst>.tmp-*" and "<dst>.old-*"
// folders of an interrupted one are removed; an "<dst>.old-*" is renamed back first when <dst>
// itself is missing. The source is never changed.
//
// The bundle: when `dst_bundle_dir` is given and holds no imported game, and the source has one
// (<src>/game, or <src>/../game when the source is a folder named data, a kit's data/), its
// MapleThief executable's SHA-256 is checked against 1.5's (ipa_import.h) and the folder is
// copied, staged as "game.tmp-<id>" beside `dst_bundle_dir` (the IPA import's staging name, so
// either cleans the other's leftovers) and renamed into place. A bundle that can't be copied
// doesn't fail the data copy: the reason is in `skipped`, and the player imports the IPA.
#ifndef MAPLETHIEF_PLATFORM_DATA_COPY_H
#define MAPLETHIEF_PLATFORM_DATA_COPY_H

#include <filesystem>
#include <string>
#include <vector>

#include "data_location.h"
#include "ipa_import.h"

namespace maplethief::frontend {

struct DataCopyResult {
    bool ok = false;
    std::string error;                 // why nothing was copied ("" when ok)
    std::vector<std::string> copied;   // paths below dst written ("saves/theif0"; "game/")
    std::vector<std::string> skipped;  // source paths left out, each "<path>: <why>"
    std::vector<std::string> replaced; // paths below dst whose old bytes went to backups/import/
    std::string backup;                // that backups/import/<time>/ folder ("" if none)
};

// Copies the player data (above) from src_dir into dst_dir, and the bundle into dst_bundle_dir
// (empty: no bundle copy). Without `force`, a dst_dir that already holds player data
// (data_dir_has_player_data) is refused and nothing is written; with it, what is there is kept
// except what the source replaces, which is backed up first (`--migrate-from DIR --force`).
// Never modifies src_dir. Paths are UTF-8 (std::filesystem::path from u8 strings on Windows).
DataCopyResult copy_player_data(const std::filesystem::path &src_dir,
                                const std::filesystem::path &dst_dir,
                                const std::filesystem::path &dst_bundle_dir, bool force);
// The same with the executable's expected SHA-256 given (tests: a synthetic bundle).
DataCopyResult copy_player_data(const std::filesystem::path &src_dir,
                                const std::filesystem::path &dst_dir,
                                const std::filesystem::path &dst_bundle_dir, bool force,
                                const Sha256 &executable_sha256);

// Whether `dir` holds player data: a file in saves/ (or the old Documents/) other than a
// *.mt-tmp, or settings.ini (ADR 0127's condition for the first-start copy and the switch).
bool data_dir_has_player_data(const std::filesystem::path &dir);

// The first-start copy (ADR 0127's *Migration* (b)): when `loc` is the game folder (location
// game) without player data and `old_dir` (the per-user folder, user_data_dir(false)) is another
// folder with player data, copy_player_data(old_dir, loc.dir, loc.bundle_dir, false). The
// result's notice is queued (data_notice.h, which logs it): the copy, or why it was refused
// (tried again on the next start, since the new folder stays empty). Returns whether it ran
// (copied or refused); false when the conditions don't hold. `result` (if not null) gets it.
bool first_start_copy(const DataLocation &loc, const std::string &old_dir,
                      DataCopyResult *result = nullptr);
// The notices' words.
std::string first_start_copied_text(const std::string &old_dir, const std::string &new_dir);
std::string first_start_refused_text(const std::string &old_dir, const std::string &why);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_DATA_COPY_H
