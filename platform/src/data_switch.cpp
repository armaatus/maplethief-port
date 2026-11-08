// The data folder's row, switch and fallback notice (#263, ADR 0127); see data_switch.h.
#include "data_switch.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "data_copy.h"
#include "data_notice.h"
#include "ipa_import.h"
#include "port_text.h"
#include "save_menu.h"
#include "save_tools.h"
#include "settings.h"

namespace maplethief::frontend {

namespace fs = std::filesystem;

namespace {

fs::path utf8_path(const std::string &s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string path_utf8(const fs::path &p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

unsigned long process_id() {
#if defined(_WIN32)
    return static_cast<unsigned long>(_getpid());
#else
    return static_cast<unsigned long>(getpid());
#endif
}

// A byte that starts a UTF-8 character (not a continuation byte).
bool char_start(const std::string &s, std::size_t i) {
    return i >= s.size() || (static_cast<unsigned char>(s[i]) & 0xC0) != 0x80;
}

// The question's and the status line's folder: two help lines hold about 140 characters.
constexpr std::size_t kQuestionPath = 70;

// The newest of the save files and settings.ini in `dir`, as "YYYY-MM-DD HH:MM:SS"; "" if none.
std::string newest_data_time(const std::string &dir, bool local) {
    std::error_code ec;
    const fs::path d = utf8_path(dir);
    std::optional<fs::file_time_type> newest;
    const auto see = [&](const fs::path &p) {
        std::error_code e;
        const fs::file_time_type t = fs::last_write_time(p, e);
        if (!e && (!newest || t > *newest)) {
            newest = t;
        }
    };
    if (fs::is_regular_file(d / kSettingsFileName, ec)) {
        see(d / kSettingsFileName);
    }
    for (const char *name : {kSavesDirName, kOldDocumentsDirName}) {
        for (const SaveFile &f : read_save_dir(path_utf8(d / name))) {
            see(d / name / f.name);
        }
    }
    if (!newest) {
        return "";
    }
    const auto when = std::chrono::system_clock::now() +
                      std::chrono::duration_cast<std::chrono::system_clock::duration>(
                          *newest - fs::file_time_type::clock::now());
    return backup_time_text(backup_stamp(when), local);
}

// cur's bundle (an imported game/) into `dest_bundle_dir` when that has none: staged in
// "<dest>.tmp-<pid>", then renamed. "" or the error.
std::string copy_bundle(const DataLocation &cur, const std::string &dest_bundle_dir,
                        std::vector<std::string> *log) {
    if (cur.bundle_dir.empty() || dest_bundle_dir.empty() ||
        !has_imported_game(cur.bundle_parent())) {
        return "";
    }
    const fs::path dest = utf8_path(dest_bundle_dir);
    if (has_imported_game(path_utf8(dest.parent_path()))) {
        return "";
    }
    std::error_code ec;
    if (fs::exists(dest, ec)) {
        log->push_back("data: switch: " + dest_bundle_dir + " is there without a game: not copied");
        return "";
    }
    fs::path stage = dest;
    stage += ".tmp-" + std::to_string(process_id());
    fs::remove_all(stage, ec);
    fs::copy(utf8_path(cur.bundle_dir), stage, fs::copy_options::recursive, ec);
    if (!ec) {
        fs::rename(stage, dest, ec);
    }
    if (ec) {
        std::error_code ignore;
        fs::remove_all(stage, ignore);
        return dest_bundle_dir + ": " + ec.message();
    }
    log->push_back("data: switch: copied the game " + cur.bundle_dir + " to " + dest_bundle_dir);
    return "";
}

} // namespace

std::string shorten_path_middle(const std::string &path, std::size_t max) {
    if (path.size() <= max) {
        return path;
    }
    if (max <= 3) {
        return std::string(max, '.');
    }
    const std::size_t room = max - 3;
    std::size_t head = room / 3;
    std::size_t tail_at = path.size() - (room - head);
    while (head > 0 && !char_start(path, head)) {
        head--;
    }
    while (tail_at < path.size() && !char_start(path, tail_at)) {
        tail_at++;
    }
    return path.substr(0, head) + "..." + path.substr(tail_at);
}

const char *data_place_title(DataPlace place) {
    switch (place) {
    case DataPlace::kGame:
        return tr(Txt::data_game_folder);
    case DataPlace::kUser:
        return tr(Txt::data_user_folder);
    case DataPlace::kEnv:
        return tr(Txt::data_env_folder);
    case DataPlace::kIos:
        return tr(Txt::data_app_folder);
    case DataPlace::kSwitch:
        return tr(Txt::data_sd_card);
    case DataPlace::kNone:
        break;
    }
    return tr(Txt::data_none);
}

DataLocationRow data_location_row(const DataLocation &loc, std::size_t max_note) {
    DataLocationRow row;
    row.value = data_place_title(loc.place);
    if (loc.dir.empty()) {
        row.note = tr(Txt::data_no_folder);
    } else {
        const std::string prefix = tr(Txt::data_note, {""});
        const std::size_t room = max_note > prefix.size() ? max_note - prefix.size() : 0;
        row.note = tr(Txt::data_note, {shorten_path_middle(loc.dir, room)});
    }
    switch (loc.reason) {
    case DataReason::kDefault:
        row.help = tr(Txt::data_help_default);
        break;
    case DataReason::kFlag:
        row.help =
            tr(loc.place == DataPlace::kGame ? Txt::data_help_flag_game : Txt::data_help_flag_user);
        break;
    case DataReason::kMarker:
        row.help = tr(Txt::data_help_marker);
        break;
    case DataReason::kApplicationsFolder:
        row.help = tr(Txt::data_help_applications);
        break;
    case DataReason::kFallbackNotWritable:
        row.help = tr(Txt::data_help_not_writable, {loc.why});
        break;
    case DataReason::kFallbackTranslocated:
        row.help = tr(Txt::data_help_translocated);
        break;
    case DataReason::kFallbackNoBase:
        row.help = tr(Txt::data_help_no_base);
        break;
    case DataReason::kEnv:
        row.help = tr(Txt::data_help_env);
        break;
    case DataReason::kPlatform:
    case DataReason::kNone:
        row.help = tr(Txt::data_help_platform);
        break;
    }
    return row;
}

std::string data_fallback_text(const DataLocation &loc) {
    switch (loc.reason) {
    case DataReason::kFallbackNotWritable:
        return tr(Txt::data_notice_not_writable, {loc.base, loc.why, loc.dir});
    case DataReason::kFallbackTranslocated:
        return tr(Txt::data_notice_translocated, {loc.dir});
    case DataReason::kFallbackNoBase:
        return tr(Txt::data_notice_no_base, {loc.dir});
    default:
        return "";
    }
}

// --- The switch --------------------------------------------------------------------------------

DataPlace data_switch_target(const DataLocation &cur) {
    return cur.place == DataPlace::kGame ? DataPlace::kUser : DataPlace::kGame;
}

DataSwitchPlan plan_data_switch(const DataLocation &cur, DataPlace to, const DataSwitchInputs &in) {
    DataSwitchPlan plan;
    plan.to = to;
    if (cur.place == DataPlace::kEnv) {
        plan.refused = tr(Txt::data_refused_env);
        return plan;
    }
    if ((cur.place != DataPlace::kGame && cur.place != DataPlace::kUser) ||
        (to != DataPlace::kGame && to != DataPlace::kUser)) {
        plan.refused = tr(Txt::data_refused_platform);
        return plan;
    }
    if (to == DataPlace::kGame && cur.fallback()) {
        const DataLocationRow row = data_location_row(cur, 0);
        plan.refused = cur.reason == DataReason::kFallbackNotWritable
                           ? tr(Txt::data_refused_fallback, {cur.why})
                           : row.help;
        return plan;
    }
    if (cur.base.empty()) {
        plan.refused = tr(Txt::data_refused_no_base);
        return plan;
    }
    if (cur.place == to) {
        plan.refused = tr(Txt::data_already, {cur.dir});
        return plan;
    }
    if (to == DataPlace::kGame && cur.reason == DataReason::kApplicationsFolder) {
        plan.refused = tr(Txt::data_refused_applications);
        return plan;
    }
    plan.marker = path_utf8(utf8_path(cur.base) / kUserDataMarkerName);
    DataLocation dest;
    if (to == DataPlace::kGame) {
        dest = data_location_at(path_utf8(utf8_path(cur.base) / kDataDirName), DataPlace::kGame,
                                DataReason::kDefault, cur.base);
    } else {
        const std::string user = in.user_dir ? in.user_dir() : std::string();
        if (user.empty()) {
            plan.refused = tr(Txt::data_refused_no_user);
            return plan;
        }
        dest = data_location_at(user, DataPlace::kUser, DataReason::kMarker, cur.base);
    }
    plan.dest = dest.dir;
    plan.dest_bundle_dir = dest.bundle_dir;
    plan.dest_has_data = data_dir_has_player_data(utf8_path(plan.dest));
    const std::string shown = shorten_path_middle(plan.dest, kQuestionPath);
    if (plan.dest_has_data) {
        plan.dest_saved = newest_data_time(plan.dest, in.local_time);
        plan.question = plan.dest_saved.empty()
                            ? tr(Txt::data_question_has_data_undated, {shown})
                            : tr(Txt::data_question_has_data, {shown, plan.dest_saved});
    } else {
        plan.question = tr(Txt::data_question, {shown});
    }
    return plan;
}

std::string user_data_marker_text(const std::string &user_dir) {
    return "MapleThief keeps its data in your user folder while this file is here:\n" + user_dir +
           "\nDelete this file, or choose Saves > Data location in the settings menu, to keep the "
           "data next to the game again (in data/).\n";
}

DataSwitchResult run_data_switch(const DataLocation &cur, const DataSwitchPlan &plan,
                                 const DataSwitchInputs &in) {
    DataSwitchResult r;
    if (!plan.refused.empty()) {
        r.message = plan.refused;
        return r;
    }
    const auto refuse = [&r](const std::string &why) {
        r.message = tr(Txt::data_not_switched, {why});
        r.log.push_back("data: switch: " + r.message);
        return r;
    };
    if (!plan.dest_has_data) {
        if (plan.to == DataPlace::kGame) {
            std::string why;
            const bool ok = in.probe ? in.probe(plan.dest, &why) : probe_data_dir(plan.dest, &why);
            if (!ok) {
                return refuse(tr(Txt::data_refused_not_writable, {plan.dest, why}));
            }
        }
        const DataCopyResult copy =
            copy_player_data(utf8_path(cur.dir), utf8_path(plan.dest), fs::path(), false);
        if (!copy.ok) {
            return refuse(copy.error);
        }
        r.log.push_back("data: switch: copied " + std::to_string(copy.copied.size()) +
                        " files from " + cur.dir + " to " + plan.dest);
        for (const std::string &s : copy.skipped) {
            r.log.push_back("data: switch: skipped " + s);
        }
        if (const std::string why = copy_bundle(cur, plan.dest_bundle_dir, &r.log); !why.empty()) {
            r.log.push_back("data: switch: the game was not copied: " + why +
                            " (the import screen asks for the IPA)");
        }
    } else {
        r.log.push_back("data: switch: " + plan.dest + " already has data (saved " +
                        plan.dest_saved + "): used as it is, nothing copied");
    }
    std::error_code ec;
    const fs::path marker = utf8_path(plan.marker);
    if (plan.to == DataPlace::kUser) {
        std::ofstream out(marker, std::ios::binary | std::ios::trunc);
        const std::string text = user_data_marker_text(plan.dest);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        if (!out) {
            return refuse(tr(Txt::data_refused_not_writable, {plan.marker, "cannot write"}));
        }
        r.log.push_back("data: switch: wrote " + plan.marker);
    } else if (fs::exists(marker, ec)) {
        if (!fs::remove(marker, ec)) {
            return refuse(tr(Txt::data_refused_not_writable, {plan.marker, ec.message()}));
        }
        r.log.push_back("data: switch: removed " + plan.marker);
    }
    r.ok = true;
    r.message = tr(Txt::data_done, {shorten_path_middle(plan.dest, kQuestionPath)});
    r.log.push_back("data: switch: done; the next start uses " + plan.dest);
    return r;
}

// --- The Saves tab's row -------------------------------------------------------------------

DataLocationView data_location_view(const DataLocation &cur) {
    const DataLocationRow row = data_location_row(cur, 0);
    return DataLocationView{row.value, row.help};
}

std::string data_location_question(const DataLocation &cur, std::int32_t panel,
                                   const DataSwitchInputs &in) {
    if (panel >= 0) {
        return "";
    }
    const DataSwitchPlan plan = plan_data_switch(cur, data_switch_target(cur), in);
    if (!plan.question.empty()) {
        SDL_Log("data: switch: asks to use %s (now %s)", plan.dest.c_str(), cur.dir.c_str());
    }
    return plan.question;
}

DataSwitchResult data_location_action(const DataLocation &cur, std::int32_t panel,
                                      const DataSwitchInputs &in,
                                      const std::function<void()> &flush) {
    if (panel >= 0) {
        DataSwitchResult r;
        r.message = tr(Txt::data_refused_loaded);
        return r;
    }
    const DataSwitchPlan plan = plan_data_switch(cur, data_switch_target(cur), in);
    if (plan.refused.empty() && flush) {
        flush();
    }
    DataSwitchResult r = run_data_switch(cur, plan, in);
    for (const std::string &line : r.log) {
        SDL_Log("%s", line.c_str());
    }
    if (!r.ok) {
        SDL_Log("data: switch to the %s folder: %s", data_place_name(plan.to), r.message.c_str());
    }
    return r;
}

int switch_data_location_command(const DataLocation &cur, DataPlace to, const DataSwitchInputs &in,
                                 std::vector<std::string> *lines) {
    if (cur.place == to && cur.place != DataPlace::kEnv) {
        lines->push_back(tr(Txt::data_already, {cur.dir}));
        return 0;
    }
    const DataSwitchPlan plan = plan_data_switch(cur, to, in);
    const DataSwitchResult r = run_data_switch(cur, plan, in);
    lines->insert(lines->end(), r.log.begin(), r.log.end());
    if (!r.ok) {
        lines->push_back(r.message);
        return 1;
    }
    lines->push_back("data: " + plan.dest);
    lines->push_back("location: " + std::string(data_place_name(plan.to)));
    return 0;
}

// --- The fallback notice
// ---------------------------------------------------------------------------

std::string read_frontend_key(const std::string &dir, const char *key) {
    if (dir.empty()) {
        return "";
    }
    std::ifstream in(utf8_path(dir) / kSettingsFileName, std::ios::binary);
    std::string line;
    std::string value;
    while (std::getline(in, line)) {
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const auto trim = [](std::string s) {
            const char *blank = " \t\r";
            s.erase(0, s.find_first_not_of(blank));
            const std::size_t end = s.find_last_not_of(blank);
            s.erase(end == std::string::npos ? 0 : end + 1);
            return s;
        };
        if (trim(line.substr(0, eq)) == key) {
            value = trim(line.substr(eq + 1));
        }
    }
    return value;
}

bool write_frontend_key(const std::string &dir, const char *key, const std::string &value) {
    if (dir.empty()) {
        return false;
    }
    const fs::path file = utf8_path(dir) / kSettingsFileName;
    std::error_code ec;
    std::string text;
    if (fs::is_regular_file(file, ec)) {
        std::ifstream in(file, std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    std::string out;
    bool done = false;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t nl = text.find('\n', at);
        nl = nl == std::string::npos ? text.size() : nl + 1;
        std::string line = text.substr(at, nl - at);
        at = nl;
        const std::size_t eq = line.find('=');
        if (!done && eq != std::string::npos) {
            std::string k = line.substr(0, eq);
            k.erase(0, k.find_first_not_of(" \t"));
            k.erase(k.find_last_not_of(" \t") + 1);
            if (k == key) {
                const bool crlf = line.size() >= 2 && line[line.size() - 2] == '\r';
                line = std::string(key) + " = " + value + (crlf ? "\r\n" : "\n");
                done = true;
            }
        }
        out += line;
    }
    if (!done) {
        if (!out.empty() && out.back() != '\n') {
            out += '\n';
        }
        out += "# the data folder fallback whose notice was shown (the frontend's, ADR 0127)\n";
        out += std::string(key) + " = " + value + "\n";
    }
    fs::create_directories(file.parent_path(), ec);
    fs::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        o.write(out.data(), static_cast<std::streamsize>(out.size()));
        o.close();
        if (!o) {
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, file, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

bool post_data_fallback_notice(const DataLocation &loc) {
    if (!loc.fallback()) {
        return false;
    }
    const char *reason = data_reason_name(loc.reason);
    if (read_frontend_key(loc.dir, kFallbackSeenKey) == reason) {
        return false;
    }
    queue_data_notice(data_fallback_text(loc));
    if (!write_frontend_key(loc.dir, kFallbackSeenKey, reason)) {
        SDL_Log("data: cannot write %s to %s/%s: the notice shows again", kFallbackSeenKey,
                loc.dir.c_str(), kSettingsFileName);
    }
    return true;
}

} // namespace maplethief::frontend
