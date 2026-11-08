// Copying a player's data folder into another (ADR 0127's *Migration*): the first-start copy and
// --migrate-from (#262), and the Saves tab's Data location switch (#263).
//
// INTERIM (#263): #262 owns this module. This is the shared interface (wave-3 design note) with a
// minimal implementation so the switch can be built in parallel; #262's data_copy replaces both
// files when it lands on the epic branch.
#ifndef MAPLETHIEF_PLATFORM_DATA_COPY_H
#define MAPLETHIEF_PLATFORM_DATA_COPY_H

#include <filesystem>
#include <string>
#include <vector>

namespace maplethief::frontend {

struct DataCopyResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> copied;  // what was copied, relative to dst_dir
    std::vector<std::string> skipped; // what was left out (*.mt-tmp, unknown names)
};

// Copies the player data (ADR 0127's list: the save set through the import's checks,
// gamecenter.json, settings.ini, bindings.ini, window.ini, backups/ states/ screenshots/
// exports/ import/; the old layout with Documents/ or the new one with saves/) from `src_dir`
// into `dst_dir`, in the new layout, staged in "<dst>.tmp-<pid>" and then renamed into place.
// Never modifies `src_dir`. `dst_bundle_dir` (empty: skip): src's game/ is copied there when it
// is missing. `force`: --migrate-from's --force.
DataCopyResult copy_player_data(const std::filesystem::path &src_dir,
                                const std::filesystem::path &dst_dir,
                                const std::filesystem::path &dst_bundle_dir, bool force);

// Whether `dir` holds player data: saves/ (or Documents/) with game files, or settings.ini.
bool data_dir_has_player_data(const std::filesystem::path &dir);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_DATA_COPY_H
