// The packs screen (#370, ADR 0149); see packs.h.
#include "packs.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <system_error>
#include <utility>

#include "data_notice.h"
#include "maplethief/pack_manifest.h"
#include "pack_folders.h"
#include "port_text.h"

namespace maplethief::frontend {

namespace {

namespace fs = std::filesystem;

fs::path utf8_path(const std::string &s) { return fs::path(std::u8string(s.begin(), s.end())); }

const char *kind_label(PackKind kind) {
    switch (kind) {
    case PackKind::kTexture:
        return tr(Txt::packs_kind_texture);
    case PackKind::kAudio:
        return tr(Txt::packs_kind_audio);
    case PackKind::kText:
        return tr(Txt::packs_kind_text);
    }
    return "";
}

std::size_t index_of(PackKind kind) { return static_cast<std::size_t>(kind); }

// Two paths name one folder: the same text without trailing separators.
std::string trimmed(std::string_view p) {
    while (p.size() > 1 && (p.back() == '/' || p.back() == '\\')) {
        p.remove_suffix(1);
    }
    return std::string(p);
}

// How many of `packs` are on before index `i`, plus one: its place in the order.
std::size_t place(const std::vector<PacksScreen::Entry> &packs, std::size_t i) {
    std::size_t n = 1;
    for (std::size_t k = 0; k < i; k++) {
        n += packs[k].enabled ? 1 : 0;
    }
    return n;
}

} // namespace

const char *pack_kind_name(PackKind kind) {
    switch (kind) {
    case PackKind::kTexture:
        return "texture";
    case PackKind::kAudio:
        return "audio";
    case PackKind::kText:
        return "text";
    }
    return "";
}

const char *pack_kind_key(PackKind kind) {
    switch (kind) {
    case PackKind::kTexture:
        return "texture_pack";
    case PackKind::kAudio:
        return "audio_pack";
    case PackKind::kText:
        return "text_pack";
    }
    return "";
}

const char *pack_kind_folder(PackKind kind) {
    const PackFolder *f = find_pack_folder(pack_kind_key(kind));
    return f != nullptr ? f->folder : "";
}

std::string &pack_kind_value(Settings &s, PackKind kind) {
    switch (kind) {
    case PackKind::kTexture:
        return s.texture_pack;
    case PackKind::kAudio:
        return s.audio_pack;
    case PackKind::kText:
        break;
    }
    return s.text_pack;
}

const std::string &pack_kind_value(const Settings &s, PackKind kind) {
    return pack_kind_value(const_cast<Settings &>(s), kind);
}

std::optional<PackKind> pack_kind_of(std::string_view key) {
    for (const PackKind k : kPackKinds) {
        if (key == pack_kind_key(k)) {
            return k;
        }
    }
    return std::nullopt;
}

std::vector<std::string> split_pack_list(std::string_view value) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (at <= value.size()) {
        const std::size_t bar = std::min(value.find('|', at), value.size());
        if (bar > at) {
            out.emplace_back(value.substr(at, bar - at));
        }
        at = bar + 1;
    }
    return out;
}

std::string join_pack_list(const std::vector<std::string> &paths) {
    std::string out;
    for (const std::string &p : paths) {
        out += (out.empty() ? "" : "|") + p;
    }
    return out;
}

PackInfo read_pack(const std::string &path, PackKind kind, const std::string &port_version) {
    auto m = std::make_unique<mt_pack_manifest>();
    mt_pack_manifest_read(path.c_str(), pack_kind_name(kind), port_version.c_str(), m.get());
    PackInfo p;
    p.path = path;
    p.id = m->id;
    p.name = m->name;
    p.version = m->version;
    p.author = m->author;
    p.description = m->description;
    p.language = m->language;
    p.content = m->content;
    p.refused = m->refused;
    if (p.refused.empty() && path.find('|') != std::string::npos) {
        p.refused = "its folder's path holds '|', which a list of packs cannot";
    }
    return p;
}

std::vector<PackInfo> check_pack_list(const std::vector<std::string> &paths, PackKind kind,
                                      const std::string &port_version) {
    std::vector<PackInfo> out;
    std::vector<std::string> ids;
    std::string language;
    for (const std::string &path : paths) {
        PackInfo p = read_pack(path, kind, port_version);
        if (p.refused.empty() && out.size() >= kMaxPacksPerKind) {
            p.refused = "at most 16 packs of a kind are on at once";
        }
        if (p.refused.empty() && std::find(ids.begin(), ids.end(), p.id) != ids.end()) {
            p.refused = "an earlier pack in the list has its id, " + p.id;
        }
        if (p.refused.empty() && kind == PackKind::kText) {
            if (language.empty()) {
                language = p.language;
            } else if (p.language != language) {
                p.refused =
                    "language " + p.language + " differs from the first text pack's " + language;
            }
        }
        if (p.refused.empty()) {
            ids.push_back(p.id);
        }
        out.push_back(std::move(p));
    }
    return out;
}

std::string pack_to_open(const Settings &s, PackKind kind, const std::string &port_version) {
    std::string first;
    for (const PackInfo &p :
         check_pack_list(split_pack_list(pack_kind_value(s, kind)), kind, port_version)) {
        if (!p.refused.empty()) {
            SDL_Log("packs: %s pack %s refused: %s", pack_kind_name(kind), p.path.c_str(),
                    p.refused.c_str());
            queue_data_notice(tr(Txt::notice_pack_refused, {pack_title(p), p.refused}));
        } else if (first.empty()) {
            first = p.path;
        } else {
            SDL_Log("packs: %s pack %s not opened: one pack of a kind opens at a time, the "
                    "first (%s)",
                    pack_kind_name(kind), p.path.c_str(), first.c_str());
        }
    }
    return first;
}

std::string pack_title(const PackInfo &p) {
    return p.version.empty() ? p.name : p.name + " " + p.version;
}

std::string pack_list_summary(std::string_view value, const char *none) {
    const std::vector<std::string> paths = split_pack_list(value);
    if (paths.empty()) {
        return none;
    }
    const std::string first = pack_value_text(paths.front(), none);
    return paths.size() == 1 ? first : first + " + " + std::to_string(paths.size() - 1);
}

PacksScreen::PacksScreen(std::string base, std::string port_version)
    : base_(std::move(base)), port_(std::move(port_version)) {}

void PacksScreen::scan(const Settings &s) {
    rows_.clear();
    for (const PackKind kind : kPackKinds) {
        std::vector<Entry> &packs = kinds_[index_of(kind)];
        packs.clear();
        // The setting's list first, in its order, with the list's own checks.
        const std::vector<std::string> on = split_pack_list(pack_kind_value(s, kind));
        for (PackInfo &p : check_pack_list(on, kind, port_)) {
            packs.push_back(Entry{std::move(p), true});
        }
        // Then the pack folder's others, by name.
        std::vector<std::string> found;
        if (!base_.empty()) {
            const std::string dir =
                base_ + (base_.back() == '/' ? "" : "/") + pack_kind_folder(kind);
            std::error_code ec;
            fs::create_directories(utf8_path(dir), ec);
            if (const PackFolder *f = find_pack_folder(pack_kind_key(kind)); f != nullptr) {
                found = list_pack_folder(dir, *f);
            }
        }
        std::vector<Entry> off;
        for (const std::string &path : found) {
            const bool listed = std::any_of(on.begin(), on.end(), [&](const std::string &o) {
                return trimmed(o) == trimmed(path);
            });
            if (!listed) {
                off.push_back(Entry{read_pack(path, kind, port_), false});
            }
        }
        std::stable_sort(off.begin(), off.end(),
                         [](const Entry &a, const Entry &b) { return a.info.name < b.info.name; });
        packs.insert(packs.end(), off.begin(), off.end());
        if (packs.empty()) {
            rows_.push_back(Row{kind, 0, true});
        }
        for (std::size_t i = 0; i < packs.size(); i++) {
            rows_.push_back(Row{kind, i, false});
        }
    }
}

std::size_t PacksScreen::first_row(PackKind kind) const {
    for (std::size_t i = 0; i < rows_.size(); i++) {
        if (rows_[i].kind == kind) {
            return i;
        }
    }
    return 0;
}

std::vector<MenuListRow> PacksScreen::list_rows() const {
    std::vector<MenuListRow> out;
    for (const Row &r : rows_) {
        if (r.empty) {
            out.push_back(MenuListRow{tr(Txt::packs_none_label, {kind_label(r.kind)}), ""});
            continue;
        }
        const std::vector<Entry> &packs = kinds_[index_of(r.kind)];
        const Entry &e = packs[r.pack];
        const std::string n = std::to_string(place(packs, r.pack));
        std::string value;
        if (e.enabled) {
            value =
                e.info.refused.empty() ? tr(Txt::packs_on, {n}) : tr(Txt::packs_on_refused, {n});
        } else {
            value = e.info.refused.empty() ? tr(Txt::packs_off) : tr(Txt::packs_refused);
        }
        out.push_back(
            MenuListRow{std::string(kind_label(r.kind)) + ": " + pack_title(e.info), value});
    }
    return out;
}

std::string PacksScreen::row_help(std::size_t row) const {
    if (row >= rows_.size()) {
        return "";
    }
    const Row &r = rows_[row];
    if (r.empty) {
        return tr(Txt::packs_none_help, {pack_kind_folder(r.kind)});
    }
    const PackInfo &p = kinds_[index_of(r.kind)][r.pack].info;
    if (!p.refused.empty()) {
        return tr(Txt::packs_refused_help, {p.refused});
    }
    std::string help = p.author.empty() ? std::string() : tr(Txt::packs_by, {p.author});
    const std::string more = p.description.empty() ? p.path : p.description;
    return help.empty() ? more : help + " " + more;
}

PacksScreen::Change PacksScreen::toggle(std::size_t row) const {
    Change c;
    c.row = row;
    if (row >= rows_.size() || rows_[row].empty) {
        c.status = tr(Txt::packs_nothing);
        return c;
    }
    const Row &r = rows_[row];
    const std::vector<Entry> &packs = kinds_[index_of(r.kind)];
    const Entry &e = packs[r.pack];
    std::vector<std::string> on;
    for (const Entry &x : packs) {
        if (x.enabled && &x != &e) {
            on.push_back(x.info.path);
        }
    }
    c.key = pack_kind_key(r.kind);
    if (e.enabled) {
        // Off: the row moves below the packs still on, among the others by name.
        c.value = join_pack_list(on);
        c.status = tr(Txt::packs_turned_off, {pack_title(e.info)});
        std::size_t offs_before = 0;
        for (const Entry &x : packs) {
            offs_before += !x.enabled && x.info.name < e.info.name ? 1 : 0;
        }
        c.row = first_row(r.kind) + on.size() + offs_before;
        return c;
    }
    if (!e.info.refused.empty()) {
        c.key.clear();
        c.status = tr(Txt::packs_not_on, {pack_title(e.info), e.info.refused});
        return c;
    }
    if (on.size() >= kMaxPacksPerKind) {
        c.key.clear();
        c.status = tr(Txt::packs_full);
        return c;
    }
    on.push_back(e.info.path);
    c.value = join_pack_list(on);
    c.status = tr(Txt::packs_turned_on, {pack_title(e.info), std::to_string(on.size())});
    c.row = first_row(r.kind) + on.size() - 1;
    return c;
}

PacksScreen::Change PacksScreen::move(std::size_t row, int delta) const {
    Change c;
    c.row = row;
    if (row >= rows_.size() || rows_[row].empty) {
        c.status = tr(Txt::packs_nothing);
        return c;
    }
    const Row &r = rows_[row];
    const std::vector<Entry> &packs = kinds_[index_of(r.kind)];
    const Entry &e = packs[r.pack];
    if (!e.enabled) {
        c.status = tr(Txt::packs_move_off, {pack_title(e.info)});
        return c;
    }
    std::vector<std::string> on;
    for (const Entry &x : packs) {
        if (x.enabled) {
            on.push_back(x.info.path);
        }
    }
    // Enabled packs come first in a kind's rows, so the pack's index is its place.
    const std::size_t at = r.pack;
    if (delta < 0 && at == 0) {
        c.status = tr(Txt::packs_first, {pack_title(e.info)});
        return c;
    }
    if (delta > 0 && at + 1 >= on.size()) {
        c.status = tr(Txt::packs_last, {pack_title(e.info)});
        return c;
    }
    const std::size_t to = delta < 0 ? at - 1 : at + 1;
    std::swap(on[at], on[to]);
    c.key = pack_kind_key(r.kind);
    c.value = join_pack_list(on);
    c.status = tr(Txt::packs_moved, {pack_title(e.info), std::to_string(to + 1)});
    c.row = first_row(r.kind) + to;
    return c;
}

MenuList packs_menu_list(std::shared_ptr<PacksScreen> screen, PackKind kind, PacksApply apply) {
    MenuList list;
    list.title = tr(Txt::packs_title);
    list.rows = screen->list_rows();
    list.start_row = screen->first_row(kind);
    list.row_help = [screen](std::size_t row) { return screen->row_help(row); };
    list.refresh = [screen] { return screen->list_rows(); };
    const auto run = [screen, apply](const PacksScreen::Change &c, std::size_t &row) {
        if (c.key.empty()) {
            return c.status;
        }
        const Settings *after = apply ? apply(c.key, c.value) : nullptr;
        if (after == nullptr) {
            return std::string(tr(Txt::packs_not_saved));
        }
        screen->scan(*after);
        row = c.row;
        return c.status;
    };
    list.toggle = [screen, run](std::size_t &row) { return run(screen->toggle(row), row); };
    list.step = [screen, run](std::size_t &row, int delta) {
        return run(screen->move(row, delta), row);
    };
    return list;
}

} // namespace maplethief::frontend
