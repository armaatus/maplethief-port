// The settings menu's Saves tab (#192); see save_menu.h.
#include "save_menu.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string_view>
#include <system_error>
#include <utility>

#include "live_settings.h"
#include "platform_caps.h"
#include "port_text.h"

namespace maplethief::frontend {

namespace {

namespace fs = std::filesystem;

fs::path utf8_path(const std::string &s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string path_utf8(const fs::path &p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

bool ends_with_zip(const std::string &path) {
    if (path.size() < 4) {
        return false;
    }
    std::string ext = path.substr(path.size() - 4);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return ext == ".zip";
}

std::string file_name(const std::string &path) { return path_utf8(utf8_path(path).filename()); }

// The files of an import's backup that are a slot's theif<n>, by slot.
std::vector<int> slots_in(const BackupInfo &b) {
    std::vector<int> slots;
    for (const std::string &f : b.files) {
        if (f.size() == 6 && f.rfind("theif", 0) == 0 && f[5] >= '0' && f[5] <= '2') {
            slots.push_back(f[5] - '0');
        }
    }
    std::sort(slots.begin(), slots.end());
    return slots;
}

std::string theif_character(const BackupInfo &b, int slot) {
    const SaveSet set = read_save_dir(b.path);
    const std::string name = "theif" + std::to_string(slot);
    for (const SaveFile &f : set) {
        if (f.name == name) {
            return character_text(f.bytes);
        }
    }
    return "";
}

} // namespace

std::string job_name(int branch, int tier) {
    if (tier <= 0) {
        return tr(Txt::job_beginner);
    }
    if (tier == 1 || branch < 1 || branch > 2) {
        return tr(Txt::job_rogue);
    }
    static const Txt kAssassin[] = {Txt::job_assassin, Txt::job_hermit, Txt::job_night_lord};
    static const Txt kBandit[] = {Txt::job_bandit, Txt::job_chief_bandit, Txt::job_shadower};
    const int i = std::min(tier, 4) - 2;
    return tr(branch == 1 ? kAssassin[i] : kBandit[i]);
}

std::string character_text(const std::vector<std::uint8_t> &theif) {
    if (theif.size() < 3) {
        return "";
    }
    const int branch = theif[0], tier = theif[1], level = theif[2];
    if (branch > 2 || tier > 4 || level < 1 || level > 100) {
        return "";
    }
    return tr(Txt::saves_character, {std::to_string(level), job_name(branch, tier)});
}

std::string backup_time_text(const std::string &stamp, bool local) {
    // "YYYYMMDD-HHMMSS-mmm", then nothing or "-N".
    const auto digits = [&stamp](std::size_t at, std::size_t n, int *out) {
        int v = 0;
        for (std::size_t i = at; i < at + n; i++) {
            if (i >= stamp.size() || stamp[i] < '0' || stamp[i] > '9') {
                return false;
            }
            v = v * 10 + (stamp[i] - '0');
        }
        *out = v;
        return true;
    };
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, ms = 0;
    const bool shaped = stamp.size() >= 19 && stamp[8] == '-' && stamp[15] == '-' &&
                        (stamp.size() == 19 || stamp[19] == '-');
    if (!shaped || !digits(0, 4, &y) || !digits(4, 2, &mo) || !digits(6, 2, &d) ||
        !digits(9, 2, &h) || !digits(11, 2, &mi) || !digits(13, 2, &s) || !digits(16, 3, &ms)) {
        return stamp;
    }
    SDL_DateTime dt{};
    dt.year = y, dt.month = mo, dt.day = d, dt.hour = h, dt.minute = mi, dt.second = s;
    dt.nanosecond = ms * 1000000;
    dt.utc_offset = 0;
    SDL_Time t = 0;
    if (!SDL_DateTimeToTime(&dt, &t)) {
        return stamp;
    }
    SDL_DateTime out{};
    if (!SDL_TimeToDateTime(t, &out, local)) {
        return stamp;
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d", out.year, out.month, out.day,
                  out.hour, out.minute, out.second);
    return buf;
}

std::vector<BackupRow> backup_rows(const SaveLocation &loc, bool local_time) {
    std::vector<BackupRow> rows;
    for (const BackupInfo &b : list_backups(loc)) {
        BackupRow r;
        r.backup = b;
        r.time = backup_time_text(b.stamp, local_time);
        if (b.kind != "import") {
            r.slot = b.kind[0] - '0';
            r.character = theif_character(b, r.slot);
            r.label = tr(Txt::saves_slot, {std::to_string(r.slot + 1)}) +
                      (r.character.empty() ? "" : "  " + r.character);
        } else {
            const std::vector<int> slots = slots_in(b);
            if (slots.empty()) {
                r.label = tr(Txt::saves_replaced_other);
            } else if (slots.size() == 1) {
                r.character = theif_character(b, slots[0]);
                r.label = tr(Txt::saves_replaced_slot, {std::to_string(slots[0] + 1)}) +
                          (r.character.empty() ? "" : ", " + r.character);
            } else {
                r.label = tr(Txt::saves_replaced_slots);
                for (std::size_t i = 0; i < slots.size(); i++) {
                    r.label += (i == 0 ? " " : ", ") + std::to_string(slots[i] + 1);
                }
            }
        }
        rows.push_back(std::move(r));
    }
    // Newest first across the kinds; the stamps sort by age (and a slot before an import made in
    // the same millisecond, as list_backups lists them).
    std::stable_sort(rows.begin(), rows.end(), [](const BackupRow &a, const BackupRow &b) {
        return a.backup.stamp > b.backup.stamp;
    });
    return rows;
}

std::string restore_question(const BackupRow &row) {
    if (row.slot >= 0) {
        return tr(Txt::saves_restore_question, {std::to_string(row.slot + 1), row.time});
    }
    return tr(Txt::saves_put_back_question, {row.time});
}

std::string restore_backup_action(const SaveLocation &loc, const BackupRow &row, std::int32_t panel,
                                  const std::string &stamp) {
    if (panel >= 0) {
        SDL_Log("saves: restore of %s refused: a character is loaded (panel %d)",
                row.backup.path.c_str(), panel);
        return tr(Txt::saves_restore_loaded);
    }
    ImportResult result;
    std::string why;
    if (!restore_backup(loc, row.backup, stamp, &result, &why)) {
        SDL_Log("saves: restore of %s: %s", row.backup.path.c_str(), why.c_str());
        for (const SaveProblem &p : result.problems) {
            SDL_Log("saves:   %s", p.line().c_str());
        }
        return tr(Txt::saves_not_restored,
                  {result.problems.empty() ? why : result.problems.front().line()});
    }
    SDL_Log("saves: restored %zu files from %s (the files replaced: %s)", result.imported.size(),
            row.backup.path.c_str(), result.backup.empty() ? "none" : result.backup.c_str());
    const std::string what = row.slot >= 0
                                 ? tr(Txt::saves_restored_slot, {std::to_string(row.slot + 1)})
                                 : std::string(tr(Txt::saves_restored_files));
    return tr(result.backup.empty() ? Txt::saves_restored : Txt::saves_restored_backed_up,
              {what, row.time});
}

MenuList backups_list(const SaveLocation &loc, std::function<std::int32_t()> panel,
                      std::function<std::string()> stamp, bool local_time) {
    auto rows = std::make_shared<std::vector<BackupRow>>(backup_rows(loc, local_time));
    MenuList list;
    list.title = tr(Txt::saves_list_title);
    list.help = tr(shows_key_hints() ? Txt::saves_list_help : Txt::saves_list_help_pad);
    for (const BackupRow &r : *rows) {
        list.rows.push_back(MenuListRow{r.label, r.time});
    }
    list.question = [rows, panel](std::size_t i) {
        if (i >= rows->size() || (panel && panel() >= 0)) {
            return std::string(); // refused at once (restore_backup_action says why)
        }
        return restore_question((*rows)[i]);
    };
    list.choose = [rows, panel, stamp, loc](std::size_t i) {
        if (i >= rows->size()) {
            return std::string();
        }
        return restore_backup_action(loc, (*rows)[i], panel ? panel() : -1,
                                     stamp ? stamp() : backup_stamp_now());
    };
    return list;
}

std::string import_saves_status(const SaveLocation &loc, const std::string &source,
                                const std::string &stamp) {
    ImportResult result;
    std::string why;
    if (!import_saves(loc, source, stamp, &result, &why)) {
        SDL_Log("saves: import from %s: %s", source.c_str(), why.c_str());
        for (const SaveProblem &p : result.problems) {
            SDL_Log("saves:   %s", p.line().c_str());
        }
        return tr(Txt::saves_not_imported,
                  {result.problems.empty() ? why : result.problems.front().line()});
    }
    SDL_Log("saves: %zu files imported from %s", result.imported.size(), source.c_str());
    return tr(Txt::saves_imported, {std::to_string(result.imported.size()), file_name(source)});
}

std::string export_saves_dialog(const SavesDialogs &d, const SaveLocation &loc,
                                const std::string &stamp) {
    if (d.dialogs == nullptr || d.route == DialogRoute::kFolder || loc.data_dir.empty()) {
        return export_saves_action(loc, stamp);
    }
    if (d.dialogs->busy()) {
        return tr(Txt::saves_dialog_busy);
    }
    const fs::path dir = utf8_path(loc.data_dir) / "exports";
    std::error_code ec;
    fs::create_directories(dir, ec);
    FileDialogRequest req;
    req.kind = FileDialogRequest::Kind::kSaveFile;
    req.title = tr(Txt::saves_export_dialog);
    req.default_location = path_utf8(dir / ("MapleThief-saves-" + stamp + ".zip"));
    req.filters = {{tr(Txt::saves_zip_filter), "zip"}};
    const auto say = d.say;
    std::string why;
    const bool shown = d.dialogs->show(
        d.window, req,
        [loc, stamp, say](const FileDialogResult &r) {
            std::string status;
            if (r.outcome == FileDialogResult::Outcome::kCancelled) {
                status = tr(Txt::saves_export_cancelled);
            } else if (r.outcome == FileDialogResult::Outcome::kFailed) {
                status = export_saves_action(loc, stamp) + tr(Txt::saves_no_dialog);
            } else {
                const std::string dest = ends_with_zip(r.path) ? r.path : r.path + ".zip";
                std::vector<std::string> exported;
                std::string err;
                if (export_saves(loc, dest, &exported, &err)) {
                    SDL_Log("saves: %zu files exported to %s", exported.size(), dest.c_str());
                    status = tr(Txt::saves_exported_to, {file_name(dest)});
                } else {
                    SDL_Log("saves: export: %s", err.c_str());
                    status = tr(Txt::saves_not_exported, {err});
                }
            }
            if (say) {
                say(status);
            }
        },
        &why);
    if (!shown) {
        return export_saves_action(loc, stamp) + tr(Txt::saves_no_dialog);
    }
    return tr(Txt::saves_export_choose);
}

std::string import_saves_dialog(const SavesDialogs &d, const SaveLocation &loc,
                                std::function<std::int32_t()> panel,
                                std::function<std::string()> stamp) {
    const std::int32_t now = panel ? panel() : -1;
    const auto next_stamp = [&stamp] { return stamp ? stamp() : backup_stamp_now(); };
    if (d.dialogs == nullptr || d.route == DialogRoute::kFolder || loc.data_dir.empty() ||
        now >= 0) {
        return import_saves_action(loc, now, next_stamp());
    }
    if (d.dialogs->busy()) {
        return tr(Txt::saves_dialog_busy);
    }
    const fs::path dir = utf8_path(loc.data_dir) / "import";
    std::error_code ec;
    fs::create_directories(dir, ec);
    FileDialogRequest req;
    req.kind = FileDialogRequest::Kind::kOpenFile;
    req.title = tr(Txt::saves_import_dialog);
    req.default_location = path_utf8(dir);
    req.filters = {{tr(Txt::saves_zip_filter), "zip"}};
    const auto say = d.say;
    std::string why;
    const bool shown = d.dialogs->show(
        d.window, req,
        [loc, panel, stamp, say](const FileDialogResult &r) {
            const std::int32_t at = panel ? panel() : -1;
            const std::string s2 = stamp ? stamp() : backup_stamp_now();
            std::string status;
            if (r.outcome == FileDialogResult::Outcome::kCancelled) {
                status = tr(Txt::saves_import_cancelled);
            } else if (r.outcome == FileDialogResult::Outcome::kFailed) {
                status = import_saves_action(loc, at, s2) + tr(Txt::saves_no_dialog);
            } else if (at >= 0) {
                status = tr(Txt::saves_import_loaded);
            } else {
                status = import_saves_status(loc, r.path, s2);
            }
            if (say) {
                say(status);
            }
        },
        &why);
    if (!shown) {
        return import_saves_action(loc, now, next_stamp()) + tr(Txt::saves_no_dialog);
    }
    return tr(Txt::saves_import_choose);
}

// --- Import from a list (#241)
// ---------------------------------------------------------------------

std::vector<ImportSource> import_sources(const SaveLocation &loc) {
    struct Found {
        ImportSource source;
        fs::file_time_type time;
    };
    std::vector<Found> found;
    if (loc.data_dir.empty()) {
        return {};
    }
    for (const char *folder : {"import", "exports"}) {
        const bool folders_too = std::string_view(folder) == "import";
        std::error_code ec;
        for (const fs::directory_entry &e :
             fs::directory_iterator(utf8_path(loc.data_dir) / folder, ec)) {
            const std::string name = path_utf8(e.path().filename());
            if (name.empty() || name.front() == '.') {
                continue;
            }
            std::error_code fec;
            const bool zip = e.is_regular_file(fec) && ends_with_zip(name);
            const bool dir = folders_too && e.is_directory(fec);
            if (!zip && !dir) {
                continue;
            }
            Found f;
            f.source = ImportSource{path_utf8(e.path()), name, folder};
            f.time = e.last_write_time(fec);
            found.push_back(std::move(f));
        }
    }
    std::sort(found.begin(), found.end(), [](const Found &a, const Found &b) {
        if (a.time != b.time) {
            return a.time > b.time;
        }
        return a.source.name > b.source.name; // the stamped names sort by age too
    });
    std::vector<ImportSource> out;
    for (Found &f : found) {
        out.push_back(std::move(f.source));
    }
    return out;
}

std::string import_question(const ImportSource &source) {
    return tr(Txt::saves_import_question, {source.name});
}

MenuList import_list(const SaveLocation &loc, std::vector<ImportSource> sources,
                     std::function<std::int32_t()> panel, std::function<std::string()> stamp) {
    auto rows = std::make_shared<std::vector<ImportSource>>(std::move(sources));
    MenuList list;
    list.title = tr(Txt::saves_import_list_title);
    list.help = tr(Txt::saves_import_list_help);
    for (const ImportSource &s : *rows) {
        list.rows.push_back(MenuListRow{s.name, s.folder + "/"});
    }
    list.question = [rows, panel](std::size_t i) {
        if (i >= rows->size() || (panel && panel() >= 0)) {
            return std::string(); // refused at once (choose says why)
        }
        return import_question((*rows)[i]);
    };
    list.choose = [rows, panel, stamp, loc](std::size_t i) {
        if (i >= rows->size()) {
            return std::string();
        }
        if (panel && panel() >= 0) {
            return std::string(tr(Txt::saves_import_loaded));
        }
        return import_saves_status(loc, (*rows)[i].path, stamp ? stamp() : backup_stamp_now());
    };
    return list;
}

std::string import_saves_list_action(const SaveLocation &loc,
                                     const std::function<std::int32_t()> &panel,
                                     const std::function<std::string()> &stamp,
                                     const std::function<void(MenuList)> &open) {
    if (loc.data_dir.empty()) {
        return tr(Txt::saves_no_folder_import);
    }
    if (panel && panel() >= 0) {
        return tr(Txt::saves_import_loaded);
    }
    std::error_code ec;
    fs::create_directories(utf8_path(loc.data_dir) / "import", ec);
    std::vector<ImportSource> sources = import_sources(loc);
    if (sources.empty()) {
        return tr(Txt::saves_import_none);
    }
    if (open) {
        open(import_list(loc, std::move(sources), panel, stamp));
    }
    return std::string();
}

std::string open_data_folder_action(FileDialogs &dialogs, const SaveLocation &loc) {
    if (loc.data_dir.empty()) {
        return tr(Txt::saves_no_data_folder);
    }
    std::error_code ec;
    fs::create_directories(utf8_path(loc.data_dir), ec);
    std::string why;
    if (!dialogs.open_folder(loc.data_dir, &why)) {
        return tr(Txt::saves_cannot_open, {why});
    }
    return tr(Txt::saves_opened, {loc.data_dir});
}

std::string open_logs_folder_action(FileDialogs &dialogs, const std::string &logs_dir) {
    if (logs_dir.empty()) {
        return tr(Txt::saves_no_data_folder);
    }
    std::error_code ec;
    fs::create_directories(utf8_path(logs_dir), ec);
    std::string why;
    if (!dialogs.open_folder(logs_dir, &why)) {
        return tr(Txt::saves_cannot_open_logs, {why});
    }
    return tr(Txt::saves_opened, {logs_dir});
}

} // namespace maplethief::frontend
