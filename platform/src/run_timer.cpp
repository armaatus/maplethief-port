// The speedrun timer (#406, ADR 0156); see run_timer.h.
#include "run_timer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

namespace maplethief::frontend {

namespace fs = std::filesystem;

namespace {

fs::path utf8_path(const std::string &s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

// The text's lines (LF or CR LF), a leading byte order mark skipped.
std::vector<std::string_view> split_lines(std::string_view text) {
    if (text.substr(0, 3) == "\xEF\xBB\xBF") {
        text.remove_prefix(3);
    }
    std::vector<std::string_view> lines;
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        lines.push_back(text.substr(0, nl));
        if (nl == std::string_view::npos) {
            break;
        }
        text.remove_prefix(nl + 1);
    }
    return lines;
}

// The blank-separated words of `s`.
std::vector<std::string_view> words(std::string_view s) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
            i++;
        }
        const std::size_t from = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\t') {
            i++;
        }
        if (i > from) {
            out.push_back(s.substr(from, i - from));
        }
    }
    return out;
}

// Digits only, at most `digits` of them, at most `max`.
bool parse_number(std::string_view s, std::size_t digits, std::uint64_t max, std::uint64_t *out) {
    if (s.empty() || s.size() > digits) {
        return false;
    }
    std::uint64_t n = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
        n = n * 10 + static_cast<std::uint64_t>(c - '0');
    }
    if (n > max) {
        return false;
    }
    *out = n;
    return true;
}

bool parse_value(std::string_view s, std::int32_t *out) {
    std::uint64_t n = 0;
    if (!parse_number(s, 3, 255, &n)) {
        return false;
    }
    *out = static_cast<std::int32_t>(n);
    return true;
}

// A title: 1 to kMaxRunNameBytes bytes of printable ASCII (the overlay's font).
bool good_title(std::string_view s) {
    if (s.empty() || s.size() > kMaxRunNameBytes) {
        return false;
    }
    return std::all_of(s.begin(), s.end(), [](char c) { return c >= 0x20 && c <= 0x7E; });
}

// A category's name: 1 to kMaxRunCategoryBytes of a-z, 0-9 and '_'.
bool good_id(std::string_view s) {
    if (s.empty() || s.size() > kMaxRunCategoryBytes) {
        return false;
    }
    return std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

std::string line_problem(std::size_t n, const std::string &what) {
    return "line " + std::to_string(n) + ": " + what;
}

struct KindName {
    mt_game_event_kind kind;
    const char *name;
    int values; // 1: `a`; 2: `a b` (job advance: branch tier) or `a.b` (map change)
};
constexpr KindName kKinds[] = {
    {MT_GAME_EVENT_CHAPTER_END, "chapter_end", 1},
    {MT_GAME_EVENT_JOB_ADVANCE, "job_advance", 2},
    {MT_GAME_EVENT_BOSS_KILLED, "boss_killed", 1},
    {MT_GAME_EVENT_MAP_CHANGE, "map_change", 2},
};

const KindName *find_kind(std::string_view name) {
    for (const KindName &k : kKinds) {
        if (name == k.name) {
            return &k;
        }
    }
    return nullptr;
}

// A split's event from its words after `split` ("chapter_end", "1"); false with `why`.
bool parse_split_event(const std::vector<std::string_view> &w, RunSplit *out, std::string *why) {
    const KindName *k = w.empty() ? nullptr : find_kind(w[0]);
    if (k == nullptr) {
        *why = "a split's event is chapter_end, job_advance, boss_killed or map_change";
        return false;
    }
    out->kind = k->kind;
    out->b = 0;
    if (k->kind == MT_GAME_EVENT_MAP_CHANGE) {
        const std::size_t dot = w.size() == 2 ? w[1].find('.') : std::string_view::npos;
        if (dot == std::string_view::npos || !parse_value(w[1].substr(0, dot), &out->a) ||
            !parse_value(w[1].substr(dot + 1), &out->b)) {
            *why = "map_change takes AREA.MAP (0 to 255 each)";
            return false;
        }
        return true;
    }
    if (k->values == 2) {
        if (w.size() != 3 || !parse_value(w[1], &out->a) || !parse_value(w[2], &out->b)) {
            *why = "job_advance takes BRANCH TIER (0 to 255 each)";
            return false;
        }
        return true;
    }
    if (w.size() != 2 || !parse_value(w[1], &out->a)) {
        *why = std::string(k->name) + " takes one value (0 to 255)";
        return false;
    }
    return true;
}

const SettingInfo *run_variable_row(std::string_view key) {
    for (const char *v : kRunVariables) {
        if (key == v) {
            return find_setting(key);
        }
    }
    return nullptr;
}

// A run variable's neutral value: its Original column.
std::string original_value(const SettingInfo &row) {
    const char *column = preset_column(row, Preset::kOriginal);
    return column != nullptr ? column : setting_text(Settings{}, row);
}

std::string join(const std::vector<std::string> &v, std::string_view sep) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); i++) {
        out += (i == 0 ? "" : std::string(sep)) + v[i];
    }
    return out;
}

std::vector<std::string> split_list(std::string_view s) {
    std::vector<std::string> out;
    while (true) {
        const std::size_t comma = s.find(',');
        out.emplace_back(trim(s.substr(0, comma)));
        if (comma == std::string_view::npos) {
            return out;
        }
        s.remove_prefix(comma + 1);
    }
}

std::string all_settings_text(const Settings &s) {
    std::string out;
    for (const SettingInfo &row : settings_registry()) {
        out += (out.empty() ? "" : "; ") + std::string(row.key) + " = " + setting_text(s, row);
    }
    return out;
}

bool read_file(const fs::path &path, std::size_t max, std::string *out) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec || size > max) {
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// Writes `text` to `path` through a temporary file beside it, so a reader never sees half.
bool write_file(const fs::path &path, const std::string &text, std::string *why) {
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) {
            *why = "cannot write " + tmp.string();
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        *why = "cannot write " + path.string();
        return false;
    }
    return true;
}

bool parse_time(std::string_view s, RunTime *out) {
    // "<ms> ms <ticks> ticks"
    const std::vector<std::string_view> w = words(s);
    std::uint64_t ms = 0, ticks = 0;
    if (w.size() != 4 || w[1] != "ms" || w[3] != "ticks" ||
        !parse_number(w[0], 12, 999999999999ULL, &ms) ||
        !parse_number(w[2], 12, 999999999999ULL, &ticks)) {
        return false;
    }
    out->ms = static_cast<std::int64_t>(ms);
    out->ticks = ticks;
    return true;
}

std::string time_text(const RunTime &t) {
    return std::to_string(t.ms) + " ms " + std::to_string(t.ticks) + " ticks";
}

// The time from `from` to `to` (none below zero).
RunTime span(const RunInstant &from, const RunInstant &to) {
    RunTime t;
    t.ms = std::max<std::int64_t>(0, std::llround((to.seconds - from.seconds) * 1000.0));
    t.ticks = to.ticks >= from.ticks ? to.ticks - from.ticks : 0;
    return t;
}

// `b` less `a` (none below zero).
RunTime minus(const RunTime &b, const RunTime &a) {
    RunTime t;
    t.ms = std::max<std::int64_t>(0, b.ms - a.ms);
    t.ticks = b.ticks >= a.ticks ? b.ticks - a.ticks : 0;
    return t;
}

// The one Assist a practice needs: its segments still count with it on (#409).
constexpr std::string_view kPracticeAssist = "save_states";

} // namespace

// --- Categories ---------------------------------------------------------------------------------

bool RunSplit::matches(const mt_game_event &e) const {
    if (e.kind != static_cast<std::uint32_t>(kind) || e.value != a) {
        return false;
    }
    return (kind != MT_GAME_EVENT_JOB_ADVANCE && kind != MT_GAME_EVENT_MAP_CHANGE) ||
           e.more[0] == b;
}

const char *run_split_kind_name(mt_game_event_kind kind) {
    for (const KindName &k : kKinds) {
        if (k.kind == kind) {
            return k.name;
        }
    }
    return nullptr;
}

std::string run_split_event_text(const RunSplit &s) {
    const char *name = run_split_kind_name(s.kind);
    std::string out = name != nullptr ? name : "?";
    out += " " + std::to_string(s.a);
    if (s.kind == MT_GAME_EVENT_JOB_ADVANCE) {
        out += " " + std::to_string(s.b);
    } else if (s.kind == MT_GAME_EVENT_MAP_CHANGE) {
        out += "." + std::to_string(s.b);
    }
    return out;
}

std::vector<std::string> RunCategory::allowed(std::string_view key) const {
    for (const RunVariableRule &r : allow) {
        if (r.key == key) {
            return r.values;
        }
    }
    const SettingInfo *row = run_variable_row(key);
    return row != nullptr ? std::vector<std::string>{original_value(*row)}
                          : std::vector<std::string>{};
}

std::optional<RunCategory> parse_run_category(std::string_view id, std::string_view text,
                                              std::vector<std::string> *problems) {
    const std::size_t before = problems->size();
    if (!good_id(id)) {
        problems->push_back("the name '" + printable(id) + "' is not 1 to 32 of a-z, 0-9 and _");
        return std::nullopt;
    }
    if (text.size() > kMaxRunCategoryFileBytes) {
        problems->push_back("larger than " + std::to_string(kMaxRunCategoryFileBytes) + " bytes");
        return std::nullopt;
    }
    RunCategory c;
    c.id = std::string(id);
    bool titled = false;
    const std::vector<std::string_view> lines = split_lines(text);
    for (std::size_t i = 0; i < lines.size(); i++) {
        const std::size_t n = i + 1;
        const std::string_view line = trim(lines[i]);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            problems->push_back(line_problem(n, "no '=' in '" + printable(line) + "'"));
            continue;
        }
        const std::vector<std::string_view> left = words(line.substr(0, eq));
        const std::string_view right = trim(line.substr(eq + 1));
        if (left.empty()) {
            problems->push_back(line_problem(n, "nothing before '='"));
            continue;
        }
        if (left[0] == "title") {
            if (left.size() != 1) {
                problems->push_back(line_problem(n, "title takes nothing before '='"));
            } else if (titled) {
                problems->push_back(line_problem(n, "a second title"));
            } else if (!good_title(right)) {
                problems->push_back(
                    line_problem(n, "the title is not 1 to 32 characters of printable ASCII"));
            } else {
                c.title = std::string(right);
                titled = true;
            }
        } else if (left[0] == "allow") {
            const SettingInfo *row = left.size() == 2 ? run_variable_row(left[1]) : nullptr;
            if (row == nullptr) {
                problems->push_back(line_problem(n, "allow names a run variable: "
                                                    "fix_original_bugs or skip_intros"));
                continue;
            }
            const auto same = [&](const RunVariableRule &r) { return r.key == left[1]; };
            if (std::any_of(c.allow.begin(), c.allow.end(), same)) {
                problems->push_back(line_problem(n, "a second allow for " + std::string(left[1])));
                continue;
            }
            RunVariableRule rule{std::string(left[1]), {}};
            bool ok = true;
            for (const std::string &v : split_list(right)) {
                Settings s;
                if (!row->value.parse(row->value, v, &s)) {
                    problems->push_back(line_problem(n, rule.key + ": '" + printable(v) +
                                                            "' is not " + setting_expects(*row)));
                    ok = false;
                    break;
                }
                const std::string value = setting_text(s, *row);
                if (std::find(rule.values.begin(), rule.values.end(), value) == rule.values.end()) {
                    rule.values.push_back(value);
                }
            }
            if (ok) {
                c.allow.push_back(std::move(rule));
            }
        } else if (left[0] == "split") {
            RunSplit split;
            std::string why;
            if (!parse_split_event({left.begin() + 1, left.end()}, &split, &why)) {
                problems->push_back(line_problem(n, why));
            } else if (!good_title(right)) {
                problems->push_back(
                    line_problem(n, "a split's title is 1 to 32 characters of printable ASCII"));
            } else if (c.splits.size() >= kMaxRunSplits) {
                problems->push_back(
                    line_problem(n, "more than " + std::to_string(kMaxRunSplits) + " splits"));
            } else {
                split.title = std::string(right);
                c.splits.push_back(std::move(split));
            }
        } else {
            problems->push_back(
                line_problem(n, "unknown line '" + printable(line) + "' (title, allow or split)"));
        }
    }
    if (problems->size() != before) {
        return std::nullopt;
    }
    if (!titled) {
        problems->push_back("no title line");
    }
    if (c.splits.empty()) {
        problems->push_back("no split line");
    }
    if (problems->size() != before) {
        return std::nullopt;
    }
    return c;
}

std::string run_category_text(const RunCategory &c) {
    std::string out = "# MapleThief run category (docs/formats/run-category.md)\n";
    out += "title = " + c.title + "\n";
    for (const RunVariableRule &r : c.allow) {
        out += "allow " + r.key + " = " + join(r.values, ", ") + "\n";
    }
    for (const RunSplit &s : c.splits) {
        out += "split " + run_split_event_text(s) + " = " + s.title + "\n";
    }
    return out;
}

std::vector<RunCategory> load_run_categories(const std::string &data_dir,
                                             std::vector<std::string> *problems) {
    std::vector<RunCategory> out;
    const auto add = [&](RunCategory c) {
        const auto same = [&](const RunCategory &o) { return o.id == c.id; };
        out.erase(std::remove_if(out.begin(), out.end(), same), out.end());
        out.push_back(std::move(c));
    };
    for (const auto &[name, text] : shipped_run_category_files()) {
        std::vector<std::string> p;
        if (std::optional<RunCategory> c = parse_run_category(name, text, &p)) {
            add(std::move(*c));
        }
        for (const std::string &m : p) {
            problems->push_back("shipped " + name + kRunCategoryExtension + ": " + m);
        }
    }
    std::error_code ec;
    const fs::path dir = utf8_path(data_dir) / "categories";
    if (!data_dir.empty() && fs::is_directory(dir, ec)) {
        std::vector<fs::path> files;
        for (const fs::directory_entry &e : fs::directory_iterator(dir, ec)) {
            if (e.path().extension() == kRunCategoryExtension && e.is_regular_file(ec)) {
                files.push_back(e.path());
            }
        }
        std::sort(files.begin(), files.end());
        for (const fs::path &f : files) {
            const std::u8string stem8 = f.stem().u8string();
            const std::string stem(stem8.begin(), stem8.end());
            const std::string where = "categories/" + stem + kRunCategoryExtension + ": ";
            std::string text;
            if (!read_file(f, kMaxRunCategoryFileBytes, &text)) {
                problems->push_back(where + "cannot be read (or larger than " +
                                    std::to_string(kMaxRunCategoryFileBytes) + " bytes)");
                continue;
            }
            std::vector<std::string> p;
            if (std::optional<RunCategory> c = parse_run_category(stem, text, &p)) {
                add(std::move(*c));
            }
            for (const std::string &m : p) {
                problems->push_back(where + m);
            }
        }
    }
    std::sort(out.begin(), out.end(),
              [](const RunCategory &x, const RunCategory &y) { return x.id < y.id; });
    return out;
}

// --- Run logs ------------------------------------------------------------------------------------

std::string run_log_text(const RunRecord &r) {
    std::string out = "# MapleThief run log (docs/formats/run-category.md#the-run-log)\n";
    out += "category = " + r.category + "\n";
    out += "title = " + r.category_title + "\n";
    out += "started = " + r.started + "\n";
    out += std::string("result = ") + (r.finished ? "finished" : "reset") + "\n";
    out += "end = " + time_text(r.end) + "\n";
    out += "assisted = " + (r.assists.empty() ? std::string("none") : join(r.assists, ", ")) + "\n";
    for (const auto &[key, values] : r.variables) {
        out += "variable " + key + " = " + values + "\n";
    }
    out += std::string("in_category = ") + (r.in_category ? "yes" : "no") + "\n";
    for (const RunSplit &s : r.splits) {
        out += "split " + run_split_event_text(s) + " = " + s.title + "\n";
    }
    for (std::size_t i = 0; i < r.times.size(); i++) {
        out += "time " + std::to_string(i + 1) + " = " + time_text(r.times[i]) + "\n";
    }
    out += "settings = " + r.settings + "\n";
    return out;
}

std::optional<RunRecord> parse_run_log(std::string_view text, std::vector<std::string> *problems) {
    const std::size_t before = problems->size();
    RunRecord r;
    bool have_category = false, have_result = false, have_end = false;
    const std::vector<std::string_view> lines = split_lines(text);
    for (std::size_t i = 0; i < lines.size(); i++) {
        const std::size_t n = i + 1;
        const std::string_view line = trim(lines[i]);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const std::size_t eq = line.find('=');
        const std::vector<std::string_view> left =
            words(line.substr(0, eq == std::string_view::npos ? 0 : eq));
        const std::string_view right =
            eq == std::string_view::npos ? std::string_view() : trim(line.substr(eq + 1));
        if (left.empty()) {
            problems->push_back(line_problem(n, "not 'key = value'"));
            continue;
        }
        const std::string_view key = left[0];
        if (key == "category" && left.size() == 1 && good_id(right)) {
            r.category = std::string(right);
            have_category = true;
        } else if (key == "title" && left.size() == 1 && good_title(right)) {
            r.category_title = std::string(right);
        } else if (key == "started" && left.size() == 1) {
            r.started = std::string(right);
        } else if (key == "result" && left.size() == 1 &&
                   (right == "finished" || right == "reset")) {
            r.finished = right == "finished";
            have_result = true;
        } else if (key == "end" && left.size() == 1 && parse_time(right, &r.end)) {
            have_end = true;
        } else if (key == "assisted" && left.size() == 1) {
            if (right != "none") {
                r.assists = split_list(right);
            }
        } else if (key == "variable" && left.size() == 2 && run_variable_row(left[1]) != nullptr) {
            r.variables.emplace_back(std::string(left[1]), std::string(right));
        } else if (key == "in_category" && left.size() == 1 && (right == "yes" || right == "no")) {
            r.in_category = right == "yes";
        } else if (key == "split") {
            RunSplit s;
            std::string why;
            if (!parse_split_event({left.begin() + 1, left.end()}, &s, &why) ||
                !good_title(right)) {
                problems->push_back(line_problem(n, "a bad split line"));
                continue;
            }
            s.title = std::string(right);
            r.splits.push_back(std::move(s));
        } else if (key == "time" && left.size() == 2) {
            std::uint64_t index = 0;
            RunTime t;
            if (!parse_number(left[1], 3, kMaxRunSplits, &index) || index != r.times.size() + 1 ||
                !parse_time(right, &t)) {
                problems->push_back(line_problem(n, "a bad time line (time N = MS ms TICKS "
                                                    "ticks, N from 1 in order)"));
                continue;
            }
            r.times.push_back(t);
        } else if (key == "settings" && left.size() == 1) {
            r.settings = std::string(right);
        } else {
            problems->push_back(line_problem(n, "unknown or bad line '" + printable(line) + "'"));
        }
    }
    if (problems->size() == before) {
        if (!have_category || !have_result || !have_end) {
            problems->push_back("no category, result or end line");
        } else if (r.times.size() > r.splits.size() ||
                   (r.finished && r.times.size() != r.splits.size())) {
            problems->push_back("more times than splits, or a finished run without every split");
        }
    }
    if (problems->size() != before) {
        return std::nullopt;
    }
    return r;
}

std::string format_run_time(std::int64_t ms, bool sign) {
    const bool negative = ms < 0;
    const std::int64_t a = negative ? -ms : ms;
    const std::int64_t tenths = (a / 100) % 10;
    const std::int64_t s = (a / 1000) % 60;
    const std::int64_t m = (a / 60000) % 60;
    const std::int64_t h = a / 3600000;
    char b[48];
    if (h > 0) {
        std::snprintf(b, sizeof b, "%lld:%02lld:%02lld.%lld", static_cast<long long>(h),
                      static_cast<long long>(m), static_cast<long long>(s),
                      static_cast<long long>(tenths));
    } else {
        std::snprintf(b, sizeof b, "%lld:%02lld.%lld", static_cast<long long>(m),
                      static_cast<long long>(s), static_cast<long long>(tenths));
    }
    return (negative ? "-" : sign ? "+" : "") + std::string(b);
}

std::string run_log_name(const RunRecord &r) {
    std::string stamp;
    for (const char c : r.started) {
        if (c >= '0' && c <= '9') {
            stamp += c;
        }
    }
    if (stamp.size() == 14) { // YYYYMMDDhhmmss
        stamp.insert(8, "-");
    }
    return r.category + "-" + (stamp.empty() ? std::string("run") : stamp) + ".log";
}

bool write_run(const std::string &data_dir, const RunRecord &r, const RunRecord *best,
               std::string *log_path, bool *new_best, std::string *why) {
    *new_best = false;
    std::error_code ec;
    const fs::path dir = utf8_path(data_dir) / "runs";
    fs::create_directories(dir, ec);
    if (!fs::is_directory(dir, ec)) {
        *why = "cannot make " + dir.string();
        return false;
    }
    const std::string name = run_log_name(r);
    fs::path path = dir / name;
    for (int i = 2; fs::exists(path, ec) && i < 1000; i++) {
        path = dir / (name.substr(0, name.size() - 4) + "-" + std::to_string(i) + ".log");
    }
    const std::string text = run_log_text(r);
    if (!write_file(path, text, why)) {
        return false;
    }
    const std::u8string p8 = path.u8string();
    *log_path = std::string(p8.begin(), p8.end());
    const bool beats =
        best == nullptr || !best->counts() || best->splits != r.splits || r.end.ms < best->end.ms;
    if (r.counts() && beats) {
        if (!write_file(dir / (r.category + ".best"), text, why)) {
            return false;
        }
        *new_best = true;
    }
    return true;
}

std::optional<RunRecord> load_run_best(const std::string &data_dir, std::string_view category) {
    if (!good_id(category)) {
        return std::nullopt;
    }
    std::string text;
    if (!read_file(utf8_path(data_dir) / "runs" / (std::string(category) + ".best"),
                   kMaxRunCategoryFileBytes * 4, &text)) {
        return std::nullopt;
    }
    std::vector<std::string> problems;
    std::optional<RunRecord> r = parse_run_log(text, &problems);
    if (!r || r->category != category || !r->counts()) {
        return std::nullopt;
    }
    return r;
}

// --- Best segments -------------------------------------------------------------------------------

std::optional<RunTime> RunSegments::sum_of_best() const {
    if (best.empty()) {
        return std::nullopt;
    }
    RunTime sum;
    for (const std::optional<RunTime> &b : best) {
        if (!b) {
            return std::nullopt;
        }
        sum.ms += b->ms;
        sum.ticks += b->ticks;
    }
    return sum;
}

std::size_t RunSegments::known() const {
    return static_cast<std::size_t>(
        std::count_if(best.begin(), best.end(), [](const auto &b) { return b.has_value(); }));
}

bool RunSegments::offer(std::size_t i, const RunTime &t) {
    if (i >= best.size()) {
        return false;
    }
    if (const std::optional<RunTime> &b = best[i]; b.has_value() && b->ms <= t.ms) {
        return false;
    }
    best[i] = t;
    return true;
}

RunSegments run_segments_for(const RunCategory &c) {
    RunSegments s;
    s.category = c.id;
    s.splits = c.splits;
    s.best.resize(c.splits.size());
    return s;
}

std::string run_segments_text(const RunSegments &s) {
    std::string out = "# MapleThief best segments (docs/formats/run-category.md#best-segments)\n";
    out += "category = " + s.category + "\n";
    for (const RunSplit &split : s.splits) {
        out += "split " + run_split_event_text(split) + " = " + split.title + "\n";
    }
    for (std::size_t i = 0; i < s.best.size(); i++) {
        if (const std::optional<RunTime> &b = s.best[i]; b.has_value()) {
            out += "segment " + std::to_string(i + 1) + " = " + time_text(*b) + "\n";
        }
    }
    return out;
}

std::optional<RunSegments> parse_run_segments(std::string_view text,
                                              std::vector<std::string> *problems) {
    const std::size_t before = problems->size();
    RunSegments s;
    bool have_category = false;
    std::vector<std::pair<std::size_t, RunTime>> times; // (split index, time), checked at the end
    const std::vector<std::string_view> lines = split_lines(text);
    for (std::size_t i = 0; i < lines.size(); i++) {
        const std::size_t n = i + 1;
        const std::string_view line = trim(lines[i]);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const std::size_t eq = line.find('=');
        const std::vector<std::string_view> left =
            words(line.substr(0, eq == std::string_view::npos ? 0 : eq));
        const std::string_view right =
            eq == std::string_view::npos ? std::string_view() : trim(line.substr(eq + 1));
        if (left.empty()) {
            problems->push_back(line_problem(n, "not 'key = value'"));
            continue;
        }
        const std::string_view key = left[0];
        if (key == "category" && left.size() == 1 && good_id(right) && !have_category) {
            s.category = std::string(right);
            have_category = true;
        } else if (key == "split") {
            RunSplit split;
            std::string why;
            if (s.splits.size() >= kMaxRunSplits ||
                !parse_split_event({left.begin() + 1, left.end()}, &split, &why) ||
                !good_title(right)) {
                problems->push_back(line_problem(n, "a bad split line"));
                continue;
            }
            split.title = std::string(right);
            s.splits.push_back(std::move(split));
        } else if (key == "segment" && left.size() == 2) {
            std::uint64_t index = 0;
            RunTime t;
            if (!parse_number(left[1], 3, kMaxRunSplits, &index) || index == 0 ||
                !parse_time(right, &t) ||
                std::any_of(times.begin(), times.end(),
                            [index](const auto &p) { return p.first == index - 1; })) {
                problems->push_back(line_problem(
                    n, "a bad segment line (segment N = MS ms TICKS ticks, N from 1, once each)"));
                continue;
            }
            times.emplace_back(static_cast<std::size_t>(index - 1), t);
        } else {
            problems->push_back(line_problem(n, "unknown or bad line '" + printable(line) + "'"));
        }
    }
    if (problems->size() == before) {
        if (!have_category || s.splits.empty()) {
            problems->push_back("no category or split line");
        } else {
            s.best.resize(s.splits.size());
            for (const auto &[index, t] : times) {
                if (index >= s.best.size()) {
                    problems->push_back("segment " + std::to_string(index + 1) +
                                        " of a category with " + std::to_string(s.splits.size()) +
                                        " splits");
                    break;
                }
                s.best[index] = t;
            }
        }
    }
    if (problems->size() != before) {
        return std::nullopt;
    }
    return s;
}

bool write_run_segments(const std::string &data_dir, const RunSegments &s, std::string *why) {
    if (!good_id(s.category)) {
        *why = "no category";
        return false;
    }
    std::error_code ec;
    const fs::path dir = utf8_path(data_dir) / "runs";
    fs::create_directories(dir, ec);
    if (!fs::is_directory(dir, ec)) {
        *why = "cannot make " + dir.string();
        return false;
    }
    return write_file(dir / (s.category + ".segments"), run_segments_text(s), why);
}

std::optional<RunSegments> load_run_segments(const std::string &data_dir,
                                             std::string_view category) {
    if (!good_id(category)) {
        return std::nullopt;
    }
    std::string text;
    if (!read_file(utf8_path(data_dir) / "runs" / (std::string(category) + ".segments"),
                   kMaxRunCategoryFileBytes * 4, &text)) {
        return std::nullopt;
    }
    std::vector<std::string> problems;
    std::optional<RunSegments> s = parse_run_segments(text, &problems);
    if (!s || s->category != category) {
        return std::nullopt;
    }
    return s;
}

// --- The timer -----------------------------------------------------------------------------------

const Settings &RunTimer::settings() const {
    static const Settings defaults;
    return settings_ != nullptr ? *settings_ : defaults;
}

RunTime RunTimer::since_start(const RunInstant &now) const { return span(start_, now); }

void RunTimer::set_category(RunCategory category, std::optional<RunRecord> best,
                            std::optional<RunSegments> segments) {
    if (state_ == State::kRunning && !practice_) {
        end_run(false, clock_.now());
    }
    clear_finished();
    category_ = std::move(category);
    best_ = std::move(best);
    state_ = State::kWaiting;
    times_.clear();
    practice_ = false;
    last_segment_.reset();
    reached_.reset();
    picked_.reset();
    segments_changed_ = false;
    if (segments && segments->category == category_.id && segments->splits == category_.splits &&
        segments->best.size() == category_.splits.size()) {
        segments_ = std::move(*segments);
    } else {
        segments_ = run_segments_for(category_);
    }
}

void RunTimer::signal(RunSignal s, const RunTime &t) const {
    if (listener_) {
        listener_(s, t);
    }
}

void RunTimer::clear_finished() {
    if (state_ == State::kFinished) {
        state_ = State::kWaiting;
        times_.clear();
        signal(RunSignal::kReset, end_);
    }
}

void RunTimer::on_event(const mt_game_event &e) {
    if (e.kind == MT_GAME_EVENT_NEW_CHARACTER) {
        start();
        return;
    }
    const std::size_t next = next_split();
    if (state_ != State::kRunning || next >= category_.splits.size() ||
        !category_.splits[next].matches(e)) {
        return;
    }
    const RunInstant now = clock_.now();
    if (practice_) {
        practice_split(now);
        return;
    }
    times_.push_back(since_start(now));
    check();
    const std::size_t i = times_.size() - 1;
    offer_segment(i, i == 0 ? times_[0] : minus(times_[i], times_[i - 1]));
    reached_ = times_.size();
    signal(RunSignal::kSplit, times_.back());
    if (times_.size() == category_.splits.size()) {
        end_run(true, now);
    }
}

void RunTimer::practice_split(const RunInstant &now) {
    check();
    const RunTime t = span(segment_start_, now);
    last_segment_ = t;
    offer_segment(segment_, t);
    segment_++;
    reached_ = segment_;
    segment_start_ = now;
    if (segment_ == category_.splits.size()) {
        state_ = State::kFinished;
    }
}

void RunTimer::offer_segment(std::size_t i, const RunTime &t) {
    if (segment_counts_ && segments_.offer(i, t)) {
        segments_changed_ = true;
    }
    segment_counts_ = true;
    check(); // the next segment starts with the settings in effect now
}

bool RunTimer::start_practice(std::size_t split) {
    const std::size_t n = category_.splits.size();
    if (split == 0 || split >= n) {
        return false;
    }
    const RunInstant now = clock_.now();
    if (state_ == State::kRunning && !practice_) {
        end_run(false, now);
    }
    clear_finished();
    state_ = State::kRunning;
    practice_ = true;
    practice_from_ = split;
    segment_ = split;
    segment_start_ = now;
    last_segment_.reset();
    times_.clear();
    end_ = {};
    assists_.clear();
    variables_.clear();
    for (const char *key : kRunVariables) {
        variables_.emplace_back(key, std::vector<std::string>{});
    }
    in_category_ = true;
    segment_counts_ = true;
    check();
    return true;
}

std::optional<std::size_t> RunTimer::take_reached() {
    std::optional<std::size_t> out;
    out.swap(reached_);
    return out;
}

bool RunTimer::take_segments_changed() {
    const bool changed = segments_changed_;
    segments_changed_ = false;
    return changed;
}

void RunTimer::turn() {
    if (state_ == State::kRunning) {
        check();
    }
}

void RunTimer::start() {
    if (category_.splits.empty()) {
        return; // no category: nothing to time
    }
    const RunInstant now = clock_.now();
    if (state_ == State::kRunning && !practice_) {
        end_run(false, now);
    }
    clear_finished();
    practice_ = false;
    last_segment_.reset();
    segment_counts_ = true;
    state_ = State::kRunning;
    start_ = now;
    started_ = clock_.stamp ? clock_.stamp() : std::string();
    start_settings_ = all_settings_text(settings());
    times_.clear();
    end_ = {};
    assists_.clear();
    variables_.clear();
    for (const char *key : kRunVariables) {
        variables_.emplace_back(key, std::vector<std::string>{});
    }
    in_category_ = true;
    check();
    signal(RunSignal::kStart, RunTime{});
}

void RunTimer::reset() {
    if (practice_) {
        practice_ = false; // a practice is never a run: nothing to record
        last_segment_.reset();
        state_ = State::kWaiting;
        times_.clear();
    } else if (state_ == State::kRunning) {
        end_run(false, clock_.now());
    } else {
        clear_finished();
        state_ = State::kWaiting;
        times_.clear();
    }
}

void RunTimer::check() {
    const Settings &s = settings();
    for (const SettingInfo &row : settings_registry()) {
        if (row.group != SettingGroup::kAssists) {
            continue;
        }
        if (setting_text(s, row) == original_value(row)) {
            continue;
        }
        if (std::find(assists_.begin(), assists_.end(), row.key) == assists_.end()) {
            assists_.emplace_back(row.key);
        }
        if (row.key != kPracticeAssist) {
            segment_counts_ = false;
        }
    }
    for (auto &[key, values] : variables_) {
        const SettingInfo *row = find_setting(key);
        if (row == nullptr) {
            continue;
        }
        const std::string v = setting_text(s, *row);
        if (values.empty() || values.back() != v) {
            values.push_back(v);
        }
        const std::vector<std::string> ok = category_.allowed(key);
        if (std::find(ok.begin(), ok.end(), v) == ok.end()) {
            in_category_ = false;
            segment_counts_ = false;
        }
    }
}

void RunTimer::end_run(bool finished, const RunInstant &now) {
    if (finished) {
        check();
    }
    end_ = finished && !times_.empty() ? times_.back() : since_start(now);
    RunRecord r;
    r.category = category_.id;
    r.category_title = category_.title;
    r.started = started_;
    r.finished = finished;
    r.splits = category_.splits;
    r.times = times_;
    r.end = end_;
    r.assists = assists_;
    r.variables = variables();
    r.in_category = in_category_;
    r.settings = start_settings_;
    records_.push_back(std::move(r));
    if (finished) {
        state_ = State::kFinished;
    } else {
        state_ = State::kWaiting;
        times_.clear();
        signal(RunSignal::kReset, end_);
    }
}

RunTime RunTimer::elapsed() const {
    switch (state_) {
    case State::kRunning:
        return practice_ ? span(segment_start_, clock_.now()) : since_start(clock_.now());
    case State::kFinished:
        return practice_ ? last_segment_.value_or(RunTime{}) : end_;
    case State::kWaiting:
        break;
    }
    return {};
}

std::vector<std::pair<std::string, std::string>> RunTimer::variables() const {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto &[key, values] : variables_) {
        out.emplace_back(key, join(values, ","));
    }
    return out;
}

std::optional<std::int64_t> RunTimer::difference() const {
    if (!best_ || times_.empty() || best_->splits != category_.splits ||
        best_->times.size() < times_.size()) {
        return std::nullopt;
    }
    const std::size_t i = times_.size() - 1;
    return times_[i].ms - best_->times[i].ms;
}

std::vector<RunRecord> RunTimer::take_records() {
    std::vector<RunRecord> out;
    out.swap(records_);
    return out;
}

std::vector<std::string> RunTimer::overlay_lines() const {
    if (practice_) {
        return practice_lines();
    }
    std::vector<std::string> out;
    const std::string title = category_.title.empty() ? category_.id : category_.title;
    const std::size_t n = category_.splits.size();
    out.push_back(title + "  " + format_run_time(elapsed().ms));
    if (picked_ && *picked_ >= 1 && *picked_ <= n) {
        out.push_back("practice from: " + category_.splits[*picked_ - 1].title + " (" +
                      std::to_string(*picked_) + "/" + std::to_string(n) + ")");
    }
    if (state_ == State::kWaiting) {
        out.emplace_back(n == 0 ? "no category" : "a new character starts the run");
        return out;
    }
    if (state_ == State::kRunning && times_.size() < n) {
        out.push_back("next: " + category_.splits[times_.size()].title + " (" +
                      std::to_string(times_.size() + 1) + "/" + std::to_string(n) + ")");
    } else {
        out.push_back("finished (" + std::to_string(n) + "/" + std::to_string(n) + ")");
    }
    if (const std::optional<std::int64_t> d = difference()) {
        out.push_back(category_.splits[times_.size() - 1].title + " " + format_run_time(*d, true) +
                      " to the best");
    } else if (!best_) {
        out.emplace_back("no personal best yet");
    }
    out.push_back(assists_.empty() ? std::string("clean") : "assisted: " + join(assists_, ", "));
    if (!in_category_) {
        out.emplace_back("outside the category's run variables");
    }
    std::string vars;
    for (const auto &[key, values] : variables_) {
        vars += (vars.empty() ? "" : "  ") + key + " " + (values.empty() ? "?" : values.back());
    }
    out.push_back(vars);
    return out;
}

// A practice's overlay (#409): the segment's time, the split it was loaded from, the next split,
// its best segment and the last one timed, the sum of best.
std::vector<std::string> RunTimer::practice_lines() const {
    std::vector<std::string> out;
    const std::string title = category_.title.empty() ? category_.id : category_.title;
    const std::size_t n = category_.splits.size();
    const auto of = [n](std::size_t i) {
        return " (" + std::to_string(i) + "/" + std::to_string(n) + ")";
    };
    out.push_back(title + " practice  " + format_run_time(elapsed().ms));
    out.push_back("loaded: " + category_.splits[practice_from_ - 1].title + of(practice_from_));
    if (picked_ && *picked_ != practice_from_ && *picked_ >= 1 && *picked_ <= n) {
        out.push_back("practice from: " + category_.splits[*picked_ - 1].title + of(*picked_));
    }
    if (state_ == State::kRunning && segment_ < n) {
        out.push_back("next: " + category_.splits[segment_].title + of(segment_ + 1));
        const std::optional<RunTime> &b = segments_.best[segment_];
        out.push_back(b ? "best segment " + format_run_time(b->ms)
                        : std::string("no best segment yet"));
    } else {
        out.push_back("finished" + of(n));
    }
    if (last_segment_ && segment_ >= 1) {
        const std::optional<RunTime> &b = segments_.best[segment_ - 1];
        std::string line = "last segment " + format_run_time(last_segment_->ms);
        if (b) {
            line += " (" + format_run_time(last_segment_->ms - b->ms, true) + ")";
        }
        out.push_back(line);
    }
    if (const std::optional<RunTime> sob = segments_.sum_of_best()) {
        out.push_back("sum of best " + format_run_time(sob->ms));
    } else {
        out.push_back("sum of best: " + std::to_string(segments_.known()) + " of " +
                      std::to_string(n) + " segments");
    }
    out.emplace_back(segment_counts_ ? "practice, never a run"
                                     : "practice: this segment does not count");
    return out;
}

} // namespace maplethief::frontend
