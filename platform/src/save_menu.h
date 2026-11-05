// The settings menu's Saves tab (#192, ADR 0078's addendum), over the save tools (save_tools.h)
// and the file dialogs (file_dialog.h):
//
//   Backups           a list (SettingsMenu::open_list) of every backup, newest first: the slot,
//                     the character in it (job and level from its theif<n>) and the local time.
//                     Choosing one asks "Restore slot N from <time>? ..." (No focused), then
//                     restores it (restore_backup: checked, the current files backed up first).
//                     Only from the title or the main menu, as an import (no character loaded:
//                     mt_input_panel() < 0); elsewhere it says so without asking.
//   Export saves...   the system's save dialog, starting at exports/MapleThief-saves-<time>.zip
//   Import saves...   the system's open dialog for a zip, starting at import/ (from the title)
//                     Both fall back to the folders (live_settings.h's export_saves_action and
//                     import_saves_action) where no dialog can show (dialog_route) or SDL could
//                     not show one; a cancelled dialog does nothing.
//   Open data folder  the data folder in Finder, Explorer or the file manager (not on iOS).
//
// The game's handlers (game.cpp's start_menu) are one call each; everything here takes the
// location, the panel and the stamp, so the tests drive it on synthetic saves.
#ifndef MAPLETHIEF_PLATFORM_SAVE_MENU_H
#define MAPLETHIEF_PLATFORM_SAVE_MENU_H

#include <SDL3/SDL.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "file_dialog.h"
#include "save_tools.h"
#include "settings_menu.h"

namespace maplethief::frontend {

// --- Backups -----------------------------------------------------------------------------------

struct BackupRow {
    BackupInfo backup;
    int slot = -1;         // 0-2 for a slot's backup, -1 for an import's (the files it replaced)
    std::string character; // "Lv 23 Hermit"; "" without a readable theif<n>
    std::string time;      // "2026-11-05 14:03:12"
    std::string label;     // the list's row: "Slot 1  Lv 23 Hermit", "Replaced: slot 2, Lv 9 ..."
};

// The job's name for theif's branch (0 Rogue, 1 Assassin, 2 Bandit) and job tier (0-4):
// Beginner, Rogue, then Assassin, Hermit, Night Lord or Bandit, Chief Bandit, Shadower.
std::string job_name(int branch, int tier);
// "Lv <level> <job>" from a theif<n> file's first bytes (docs/formats/save.md); "" if it is too
// short or out of the checks' range.
std::string character_text(const std::vector<std::uint8_t> &theif);
// A backup folder's name (backup_stamp: UTC "YYYYMMDD-HHMMSS-mmm", maybe "-N") as
// "YYYY-MM-DD HH:MM:SS", in local time if `local` (else UTC); the name itself if it is not one.
std::string backup_time_text(const std::string &stamp, bool local);
// Every backup (list_backups), newest first across the slots and the imports.
std::vector<BackupRow> backup_rows(const SaveLocation &loc, bool local_time = true);
// "Restore slot 1 from <time>? The current save is backed up first." (an import's: "Put back
// the files replaced at <time>? ...").
std::string restore_question(const BackupRow &row);
// Restores the row's backup at the title (`panel` < 0); the menu's status line. Refused with a
// character loaded (`panel` >= 0), as an import is.
std::string restore_backup_action(const SaveLocation &loc, const BackupRow &row, std::int32_t panel,
                                  const std::string &stamp);
// The menu's Backups list for `loc`; `panel` and `stamp` are asked when a row is chosen (the
// game's mt_input_panel and backup_stamp_now). No question while a character is loaded: the
// refusal shows at once. Empty rows when there is no backup (open_list then opens nothing).
MenuList backups_list(const SaveLocation &loc, std::function<std::int32_t()> panel,
                      std::function<std::string()> stamp, bool local_time = true);

// --- Export, import and the folder ---------------------------------------------------------------

struct SavesDialogs {
    FileDialogs *dialogs = nullptr; // null: the folders
    SDL_Window *window = nullptr;   // the dialog's parent
    DialogRoute route = DialogRoute::kDialog;
    std::function<void(std::string)> say; // the status line, when a dialog answers later
};

// Export saves...: the save dialog (the answer exports there, ".zip" added when the name has no
// such ending), or exports/ (export_saves_action). The status line now.
std::string export_saves_dialog(const SavesDialogs &d, const SaveLocation &loc,
                                const std::string &stamp);
// Import saves...: refused with a character loaded; the open dialog for a zip (the answer is
// imported if the panel still allows it), or import/ (import_saves_action).
std::string import_saves_dialog(const SavesDialogs &d, const SaveLocation &loc,
                                std::function<std::int32_t()> panel,
                                std::function<std::string()> stamp);
// The import of `source` (a zip or folder) and the status line ("Imported 7 files from x.zip").
std::string import_saves_status(const SaveLocation &loc, const std::string &source,
                                const std::string &stamp);
// Open data folder: the data folder (made if missing) in the file manager.
std::string open_data_folder_action(FileDialogs &dialogs, const SaveLocation &loc);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_SAVE_MENU_H
