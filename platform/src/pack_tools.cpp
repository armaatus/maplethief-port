// The pack tools (#371, ADR 0205); see pack_tools.h.
#include "pack_tools.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <filesystem>
#include <set>
#include <string_view>
#include <system_error>

#include "maplethief/packcheck.h"
#include "maplethief/saves.h"
#include "maplethief/texpack.h"
#include "pack_folders.h"
#include "port_text.h"
#include "settings_menu.h"

namespace maplethief::frontend {

namespace {

namespace fs = std::filesystem;

fs::path utf8_path(const std::string &s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string path_utf8(const fs::path &p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::string join(const std::string &a, const char *b) {
    return a.empty() ? std::string() : path_utf8(utf8_path(a) / b);
}

// Each line, counted by its level (a check's report replaces the counts with the runtime's).
void collect(void *user, int level, const char *line) {
    auto *r = static_cast<PackToolResult *>(user);
    r->lines.push_back({level, line});
    ++(level == MT_PACKCHECK_REJECTED  ? r->report.rejected
       : level == MT_PACKCHECK_IGNORED ? r->report.ignored
                                       : r->report.notes);
}

const char *level_word(int level) {
    switch (level) {
    case MT_PACKCHECK_REJECTED:
        return "rejected";
    case MT_PACKCHECK_IGNORED:
        return "ignored";
    default:
        return "note";
    }
}

std::string quoted(const std::string &s) {
    std::string q(mt_textpack_quote(s.c_str(), nullptr, 0) + 1u, '\0');
    mt_textpack_quote(s.c_str(), q.data(), static_cast<std::uint32_t>(q.size()));
    q.pop_back();
    return q;
}

// The port's English by key, for Port.strings and its check: built once.
struct PortEnglish {
    std::vector<std::pair<std::string, std::string>> rows; // in the export's order
    std::set<std::string> keys;
    std::vector<std::pair<std::string, std::string>> sorted; // for lookups
};

const PortEnglish &port_english() {
    static const PortEnglish table = [] {
        PortEnglish t;
        const auto add = [&t](std::string key, const char *english) {
            if (english != nullptr && t.keys.insert(key).second) {
                t.rows.emplace_back(std::move(key), english);
            }
        };
        for (std::size_t i = 0; i < static_cast<std::size_t>(Txt::kCount); ++i) {
            add(port_text_key(static_cast<Txt>(i)), port_text_english(static_cast<Txt>(i)));
        }
        for (const SettingInfo &r : settings_registry()) {
            add(std::string("setting.") + r.key + ".label", r.label);
            add(std::string("setting.") + r.key + ".help", r.help);
            if (r.help_pad != nullptr) {
                add(std::string("setting.") + r.key + ".help_pad", r.help_pad);
            }
        }
        for (const MenuActionInfo &a : menu_actions()) {
            const std::string base = std::string("action.") + a.key + ".";
            add(base + "label", a.label);
            add(base + "help", a.help);
            if (a.help_pad != nullptr) {
                add(base + "help_pad", a.help_pad);
            }
            add(base + "button", a.button);
            if (a.confirm != nullptr) {
                add(base + "confirm", a.confirm);
            }
        }
        t.sorted = t.rows;
        std::sort(t.sorted.begin(), t.sorted.end());
        return t;
    }();
    return table;
}

const char *port_lookup(void *, const char *key) {
    const auto &s = port_english().sorted;
    const auto it = std::lower_bound(s.begin(), s.end(), std::string_view(key),
                                     [](const std::pair<std::string, std::string> &row,
                                        std::string_view k) { return row.first < k; });
    return it != s.end() && it->first == key ? it->second.c_str() : nullptr;
}

// The first line of `r` at `level` or above, "" if none.
std::string first_problem(const PackToolResult &r) {
    for (const int level : {MT_PACKCHECK_REJECTED, MT_PACKCHECK_IGNORED}) {
        for (const PackToolLine &l : r.lines) {
            if (l.level == level) {
                return l.text;
            }
        }
    }
    return "";
}

std::string kind_text(const std::string &kind) {
    return kind == "texture" ? tr(Txt::packs_kind_texture)
           : kind == "audio" ? tr(Txt::packs_kind_audio)
                             : tr(Txt::packs_kind_text);
}

} // namespace

PackToolPlaces pack_tool_places(const std::string &parent, Language language) {
    PackToolPlaces p;
    if (parent.empty()) {
        return p;
    }
    p.dump = join(join(parent, kTextureDumpsDir), "dump");
    const std::string k = "dump-" + std::to_string(kMenuPackScale) + "x";
    p.built_pack = join(join(parent, "texture-packs"), k.c_str());
    const std::string t = std::string("text-") + kLanguageNames[static_cast<int>(language)];
    p.text_export = join(join(parent, "text-packs"), t.c_str());
    return p;
}

PackToolResult check_pack(const std::string &dir, const std::string &bundle, const char *kind) {
    install_port_text_check();
    PackToolResult r;
    r.report = mt_pack_check(dir.c_str(), kind, bundle.c_str(), collect, &r);
    return r;
}

PackToolResult build_texture_pack(const std::string &dump, const std::string &pack, unsigned k,
                                  int scaler, bool ui) {
    PackToolResult r;
    r.report.kind = "texture";
    r.count = mt_texpack_build(dump.c_str(), pack.c_str(), k, scaler, ui ? 1 : 0, collect, &r);
    r.report.opens = r.count >= 0 ? 1 : 0;
    r.report.good = r.count > 0 ? static_cast<std::uint32_t>(r.count) : 0u;
    return r;
}

std::string port_strings_text() {
    std::string out = "/* Port: the port's menus. Keep each {0}, {1}, ... (the menus show plain "
                      "letters only: accents are dropped). */\n\n";
    for (const auto &[key, english] : port_english().rows) {
        out += quoted(key) + " = " + quoted(english) + ";\n";
    }
    return out;
}

void install_port_text_check() { mt_packcheck_set_port_text(port_lookup, nullptr); }

PackToolResult export_text(const std::string &bundle, Language language, const std::string &dir) {
    PackToolResult r;
    r.count = mt_textpack_export(bundle.c_str(), kLanguageNames[static_cast<int>(language)],
                                 dir.c_str(), collect, &r);
    if (r.count < 0) {
        return r;
    }
    const std::string text = port_strings_text();
    const std::string path = path_utf8(utf8_path(dir) / "Port.strings");
    if (mt_saves_replace_file(path.c_str(), text.data(), text.size()) != 0) {
        r.lines.push_back({MT_PACKCHECK_REJECTED, path + " cannot be written"});
        r.count = -1;
        return r;
    }
    r.count += static_cast<int>(port_english().rows.size());
    r.lines.push_back({MT_PACKCHECK_NOTE,
                       "Port.strings: " + std::to_string(port_english().rows.size()) + " entries"});
    return r;
}

std::string pack_tool_text(const std::string &dir, const PackToolResult &r) {
    std::string out;
    for (const PackToolLine &l : r.lines) {
        const bool refused = l.level == MT_PACKCHECK_REJECTED && r.report.opens == 0 &&
                             l.text.find("refused") != std::string::npos;
        out += std::string(refused ? "refused" : level_word(l.level)) + ": " + l.text + "\n";
    }
    const std::string kind = r.report.kind != nullptr ? r.report.kind : "";
    out += dir + ": " + (kind.empty() ? std::string("no") : kind) +
           " pack: " + pack_check_verdict(r) + "\n";
    return out;
}

std::string pack_check_verdict(const PackToolResult &r) {
    if (r.report.opens == 0) {
        return tr(Txt::packs_verdict_refused);
    }
    if (r.report.rejected != 0) {
        return tr(Txt::packs_verdict_problems,
                  {std::to_string(r.report.good), std::to_string(r.report.rejected)});
    }
    return tr(Txt::packs_verdict_opens, {std::to_string(r.report.good)});
}

std::vector<PackToCheck> packs_to_check(const Settings &s, const std::string &parent) {
    std::vector<PackToCheck> out;
    std::set<std::string> seen;
    const auto add = [&out, &seen](const char *kind, const std::string &dir) {
        std::error_code ec;
        const fs::path p = utf8_path(dir);
        const std::string key = path_utf8(fs::weakly_canonical(p, ec));
        if (!dir.empty() && seen.insert(ec ? dir : key).second) {
            out.push_back({kind, dir});
        }
    };
    const struct {
        const char *kind;
        const std::string *value;
    } rows[] = {{"texture", &s.texture_pack}, {"audio", &s.audio_pack}, {"text", &s.text_pack}};
    for (const auto &row : rows) {
        // ADR 0149: a row may list several folders, separated by '|', first first.
        std::string_view v = *row.value;
        while (!v.empty()) {
            const std::size_t bar = v.find('|');
            add(row.kind, std::string(v.substr(0, bar)));
            v = bar == std::string_view::npos ? std::string_view() : v.substr(bar + 1);
        }
    }
    if (!parent.empty()) {
        for (const PackFolder &f : kPackFolders) {
            if (f.ext != nullptr) {
                continue; // borders: not a pack
            }
            const char *kind = std::string_view(f.key) == "texture_pack" ? "texture"
                               : std::string_view(f.key) == "audio_pack" ? "audio"
                                                                         : "text";
            for (const std::string &dir : list_pack_folder(join(parent, f.folder), f)) {
                add(kind, dir);
            }
        }
    }
    return out;
}

bool dumping_textures() { return mt_texpack_dump_is_open() != 0; }

std::string dump_textures_action(const std::string &dir) {
    if (dir.empty()) {
        return tr(Txt::packs_no_folder);
    }
    if (dumping_textures()) {
        const mt_texpack_stats s = mt_texpack_get_stats();
        mt_texpack_dump_close();
        SDL_Log("texpack: the menu's dump stopped: %llu textures, %llu UIKit images new in %s",
                static_cast<unsigned long long>(s.dumped),
                static_cast<unsigned long long>(s.dumped_ui), dir.c_str());
        return tr(Txt::packs_dump_stopped, {std::to_string(s.dumped + s.dumped_ui), dir});
    }
    if (mt_texpack_dump_open(dir.c_str()) != 0) {
        return tr(Txt::packs_dump_failed, {dir});
    }
    SDL_Log("texpack: the menu's dump: every texture the game uploads from now on goes to %s",
            dir.c_str());
    return tr(Txt::packs_dump_started, {dir});
}

std::string build_texture_pack_action(const std::string &dump, const std::string &pack) {
    if (dump.empty()) {
        return tr(Txt::packs_no_folder);
    }
    std::error_code ec;
    if (!fs::is_regular_file(utf8_path(dump) / "manifest.jsonl", ec)) {
        return tr(Txt::packs_no_dump);
    }
    const PackToolResult r =
        build_texture_pack(dump, pack, kMenuPackScale, MT_TEXPACK_SCALER_SCALE, true);
    for (const PackToolLine &l : r.lines) {
        SDL_Log("texpack: build: %s: %s", level_word(l.level), l.text.c_str());
    }
    if (r.count < 0) {
        return tr(Txt::packs_not_built, {first_problem(r)});
    }
    return tr(Txt::packs_built, {std::to_string(r.count), std::to_string(kMenuPackScale), pack});
}

std::string export_text_action(const std::string &bundle, Language language,
                               const std::string &dir) {
    if (dir.empty()) {
        return tr(Txt::packs_no_folder);
    }
    const PackToolResult r = export_text(bundle, language, dir);
    for (const PackToolLine &l : r.lines) {
        SDL_Log("textpack: export: %s: %s", level_word(l.level), l.text.c_str());
    }
    if (r.count < 0) {
        return tr(Txt::packs_not_exported, {first_problem(r)});
    }
    return tr(Txt::packs_exported, {std::to_string(r.count), dir});
}

MenuList check_packs_list(const Settings &s, const std::string &parent, const std::string &bundle) {
    MenuList list;
    list.title = tr(Txt::packs_check_title);
    list.help = tr(Txt::packs_check_help);
    std::vector<std::string> problems;
    for (const PackToCheck &p : packs_to_check(s, parent)) {
        const PackToolResult r = check_pack(p.dir, bundle, p.kind.c_str());
        SDL_Log("packcheck: %s", pack_tool_text(p.dir, r).c_str());
        const std::string name = path_utf8(utf8_path(p.dir).filename());
        list.rows.push_back({kind_text(p.kind) + ": " + name, pack_check_verdict(r)});
        const std::string first = first_problem(r);
        problems.push_back(first.empty() ? tr(Txt::packs_check_fine, {name}) : first);
    }
    list.choose = [problems](std::size_t row) {
        return row < problems.size() ? problems[row] : std::string();
    };
    return list;
}

} // namespace maplethief::frontend
