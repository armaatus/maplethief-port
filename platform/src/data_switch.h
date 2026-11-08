// The data folder as the player sees and changes it (#263, ADR 0127): the settings menu's Saves
// tab row *Data location*, the switch between the game folder and the per-user folder (the row
// and --switch-data-location), and the one-time title notice of a fallback.
//
//   The row      value "Game folder" or "User folder"; the Saves tab's note line shows the
//                folder (shortened in the middle to fit; the start log has it in full), the help
//                lines what chose it and, for a fallback, how to fix it.
//   The switch   only with no character loaded (mt_input_panel() < 0). The question "Copy your
//                data to <dest> and restart?" (No focused). The frontend first writes its own
//                files (settings.ini, window.ini; bindings.ini is written as it changes). Then
//                the player data is copied (data_copy.h, staged, the source never changed) and
//                the bundle when the destination has none; then <base>/user-data.txt is written
//                (to the user folder) or removed (to the game folder). "Done. MapleThief will
//                close; start it again to use <dest>." and the game quits.
//                A destination that already has data (saves/ with game files, or settings.ini)
//                is never overwritten: "<dest> already has data (saved <time>). Switch and use
//                that data? Your data here stays here." Yes writes or removes the marker only.
//                Refused under MAPLETHIEF_DATA_DIR, and to the game folder under a fallback or
//                the Applications rule, with the reason.
//   The notice   a fallback's words (data_fallback_text) once per reason: settings.ini in the
//                folder in use keeps "data_fallback_seen = <reason>" (settings.h's
//                kFrontendOnlyKeys), so it does not show on every start.
#ifndef MAPLETHIEF_PLATFORM_DATA_SWITCH_H
#define MAPLETHIEF_PLATFORM_DATA_SWITCH_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "data_location.h"

namespace maplethief::frontend {

inline constexpr const char *kFallbackSeenKey = "data_fallback_seen";

// `path` in at most `max` characters: whole when it fits, else its start and its end with
// "..." between (the end, the folder's own name, is kept longer).
std::string shorten_path_middle(const std::string &path, std::size_t max);

// "Game folder", "User folder"; "MAPLETHIEF_DATA_DIR" for env; "App folder" (iOS), "SD card"
// (the Switch), "None".
const char *data_place_title(DataPlace place);

// The Saves tab's view of `loc`: the row's value, the tab's note line (the folder, at most
// `max_note` characters) and the help lines (what chose the folder; a fallback's cause and fix).
struct DataLocationRow {
    std::string value;
    std::string note;
    std::string help;
};
DataLocationRow data_location_row(const DataLocation &loc, std::size_t max_note);

// A fallback's words for the title notice and the log (ADR 0127's *What the player is told*);
// "" when `loc` is no fallback.
std::string data_fallback_text(const DataLocation &loc);

// --- The switch --------------------------------------------------------------------------------

struct DataSwitchInputs {
    // The per-user folder (user_data_dir(false): only named; the copy creates it); tests pass
    // their own.
    std::function<std::string()> user_dir;
    // The probe of the game folder's data/ (probe_data_dir when empty).
    std::function<bool(const std::string &data_dir, std::string *why)> probe;
    bool local_time = true; // the "saved <time>" of a destination's data
};

struct DataSwitchPlan {
    DataPlace to = DataPlace::kNone;
    std::string refused;         // not "": why it can't switch (the status line, the CLI)
    std::string dest;            // the destination data folder
    std::string dest_bundle_dir; // its bundle folder
    std::string marker;          // <base>/user-data.txt
    bool dest_has_data = false;  // then nothing is copied: the destination is used as it is
    std::string dest_saved;      // its newest save or settings.ini, "YYYY-MM-DD HH:MM:SS"
    std::string question;        // the menu's question ("" when refused)
};

// The switch from `cur` to the other folder (`to`: kGame or kUser). Asks the per-user folder
// only when switching to it; touches nothing on the disk otherwise.
DataSwitchPlan plan_data_switch(const DataLocation &cur, DataPlace to, const DataSwitchInputs &in);
// The other folder than cur's: kUser from kGame, kGame otherwise.
DataPlace data_switch_target(const DataLocation &cur);

struct DataSwitchResult {
    bool ok = false;
    std::string message;          // the status line, or what the CLI prints
    std::vector<std::string> log; // one line each: what was copied, written, removed
};
// Carries `plan` out from `cur` (see the top of this file). Never changes cur.dir or its
// bundle. With a refused plan, does nothing.
DataSwitchResult run_data_switch(const DataLocation &cur, const DataSwitchPlan &plan,
                                 const DataSwitchInputs &in);

// The text written to user-data.txt.
std::string user_data_marker_text(const std::string &user_dir);

// --- The Saves tab's row -------------------------------------------------------------------

// The row's value and help for the menu (SettingsMenu::ActionView), drawn every frame: no disk.
struct DataLocationView {
    std::string value, help;
};
DataLocationView data_location_view(const DataLocation &cur);
// The question when the row is chosen at `panel` (mt_input_panel(): < 0 no character loaded):
// plan_data_switch's, "" when it is refused or a character is loaded (the action then says why).
std::string data_location_question(const DataLocation &cur, std::int32_t panel,
                                   const DataSwitchInputs &in);
// The row chosen (after Yes, or at once when there is no question): refused while a character
// is loaded, then plan + run. `flush` writes the frontend's own files first (settings.ini,
// window.ini). The status line is the result's message.
DataSwitchResult data_location_action(const DataLocation &cur, std::int32_t panel,
                                      const DataSwitchInputs &in,
                                      const std::function<void()> &flush);

// --switch-data-location game|user: plan + run; a destination with data is used as it is (the
// menu's Yes). The lines to print, the exit status (0 done or already there, 1 refused).
int switch_data_location_command(const DataLocation &cur, DataPlace to, const DataSwitchInputs &in,
                                 std::vector<std::string> *lines);

// --- The fallback notice
// ---------------------------------------------------------------------------

// `key`'s value in <dir>/settings.ini ("" when there is none), and writing it there (the line
// replaced, or appended; the file created if missing). Only for kFrontendOnlyKeys.
std::string read_frontend_key(const std::string &dir, const char *key);
bool write_frontend_key(const std::string &dir, const char *key, const std::string &value);
// A fallback whose reason <loc.dir>/settings.ini does not have as data_fallback_seen: queues
// data_fallback_text (data_notice.h) and writes the reason. True if it queued.
bool post_data_fallback_notice(const DataLocation &loc);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_DATA_SWITCH_H
