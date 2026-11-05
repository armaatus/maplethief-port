// Text packs (#189, ADR 0108, maplethief/textpack.h, textpack_internal.h).
//
// The pack's files are untrusted data: each is read only after its size is checked, parsed by
// strings_file.cpp's strict rules, and refused whole (one log line) if it breaks one. The
// manifest must be there and say format "1", a name and a language, or the pack is not opened.
//
// What the pack changes while it is open (docs/re/text.md has the evidence):
//   - the dialogue and quest text: the game's LocalizedCString 0x6DD1 asks NSBundle, which asks
//     mt_textpack_lookup first;
//   - the data tables' names and descriptions: written into the table file's bytes as
//     CFURLCreateDataAndPropertiesFromResource reads it (patch_table), at most the field's
//     width - 1 bytes;
//   - the Korean encodings read valid UTF-8 as UTF-8 and write UTF-8 (cf/encoding.cpp), so the
//     game's EUC-KR-first conversions (TextCache_DrawText 0x9031) show the pack's text;
//   - Text_CharCount 0x22989 and Text_SubChars 0x22A1D step by UTF-8 lengths (their own step
//     is 3 bytes for any byte >= 0x80), so Text_Wrap 0x22EC5 never cuts inside "ö".
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include "cf/cf_objects.h"
#include "diag/diag_internal.h"
#include "maplethief/hle/abi.h"
#include "maplethief/hooks.h"
#include "maplethief/memory.h"
#include "maplethief/textpack.h"
#include "textpack/strings_file.h"
#include "textpack/textpack_internal.h"

namespace mt_textpack {
namespace {

namespace fs = std::filesystem;
using u32 = std::uint32_t;

void logf(const char *format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof line, format, args);
    va_end(args);
    mt_diag_log(line);
}

fs::path host_path(const std::string &utf8) {
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string utf8_of(const fs::path &p) {
    const std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

std::string lower(std::string s) {
    for (char &c : s) {
        c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return s;
}

// The data tables' text columns (docs/formats/data-tables.md; tools/mtport/datatables.py).
constexpr TextColumn k_columns[] = {
    {"mob", "name", 1, 16, 17, 50},
    {"npc", "name", 1, 32, 2, 33},
    {"item_equip", "name", 1, 32, 24, 73},
    {"item_etc", "name", 1, 64, 14, 218},
    {"item_etc", "description", 90, 128, 14, 218},
    {"skill", "name", 1, 16, 19, 297},
    {"skill", "description", 41, 128, 19, 297},
    {"skill", "detail", 169, 128, 19, 297},
};

// The files a pack may have, by their name in lower case, and the table each serves.
constexpr const char *k_files[][2] = {
    {"textpack.strings", "textpack"},
    {"scripts.strings", "Scripts"},
    {"scriptextra.strings", "ScriptExtra"},
    {"tables.strings", "Tables"},
    {"port.strings", "Port"},
};

struct Field {
    const TextColumn *column;
    u32 row;
    std::string value; // UTF-8
    std::string key;   // as the pack wrote it, for the log
};

struct Pack {
    bool open = false;
    std::string dir;
    std::map<std::string, std::map<std::string, std::string>> tables; // "Scripts" -> entries
    std::map<std::string, std::vector<Field>> fields;                 // "item_etc" -> fields
    std::set<std::string> overflow_logged;
};

Pack g_pack;
mt_textpack_stats g_stats{};

// Reads the file at `path` into `out` (at most MT_TEXTPACK_MAX_FILE bytes, checked first).
bool read_file(const fs::path &path, std::vector<std::uint8_t> *out, std::string *why) {
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec) {
        *why = "cannot be read";
        return false;
    }
    if (size > MT_TEXTPACK_MAX_FILE) {
        *why = "larger than " + std::to_string(MT_TEXTPACK_MAX_FILE >> 20) + " MiB";
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    out->resize(static_cast<std::size_t>(size));
    if (!in || (size > 0 && !in.read(reinterpret_cast<char *>(out->data()),
                                     static_cast<std::streamsize>(size)))) {
        *why = "cannot be read";
        return false;
    }
    return true;
}

bool valid_language(const std::string &tag) {
    if (tag.empty() || tag.size() > 35) {
        return false;
    }
    for (const char c : tag) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

// Why the manifest is refused, or "".
std::string check_manifest(const std::map<std::string, std::string> &m) {
    const auto get = [&m](const char *k) {
        const auto it = m.find(k);
        return it != m.end() ? it->second : std::string();
    };
    if (get("format") != "1") {
        return "its \"format\" is not \"1\"";
    }
    if (get("name").empty()) {
        return "it has no \"name\"";
    }
    if (!valid_language(get("language"))) {
        return "its \"language\" is not a language tag (letters, digits and '-', such as \"de\" "
               "or \"pt-BR\")";
    }
    return "";
}

// Splits "item_etc.12.name" into its text column and row; false (and *why) if it names none.
bool parse_field_key(const std::string &key, const TextColumn **column, u32 *row,
                     std::string *why) {
    const std::size_t a = key.find('.');
    const std::size_t b = a == std::string::npos ? a : key.find('.', a + 1);
    if (b == std::string::npos) {
        *why = "not <table>.<row>.<column>";
        return false;
    }
    const std::string table = key.substr(0, a), digits = key.substr(a + 1, b - a - 1),
                      col = key.substr(b + 1);
    if (digits.empty() || digits.size() > 5 ||
        digits.find_first_not_of("0123456789") != std::string::npos) {
        *why = "the row is not a number";
        return false;
    }
    for (const TextColumn &c : k_columns) {
        if (table == c.table && col == c.column) {
            *column = &c;
            *row = static_cast<u32>(std::stoul(digits));
            return true;
        }
    }
    *why = "no table text column " + table + "." + col;
    return false;
}

// --- the Text_Wrap hooks ---------------------------------------------------------------------

// The guest C string at `addr` (bounds-checked: a bad pointer faults as the original's read).
std::string guest_string(mt_cpu *cpu, u32 addr) { return std::string(mt_g2h_str(cpu, addr)); }

// Text_CharCount (owner, text) -> characters.
int hook_char_count(mt_cpu *cpu, const mt_hook_call *) {
    mt_args a = mt_args_begin(cpu);
    (void)mt_arg_u32(&a);
    const u32 text = mt_arg_u32(&a);
    mt_ret_u32(cpu, char_count(guest_string(cpu, text)));
    return MT_HOOK_HANDLED;
}

// Text_SubChars (owner, text, from, to) -> a new[] block with characters [from, to), NUL-ended.
int hook_sub_chars(mt_cpu *cpu, const mt_hook_call *) {
    mt_args a = mt_args_begin(cpu);
    (void)mt_arg_u32(&a);
    const u32 text = mt_arg_u32(&a);
    const auto from = static_cast<std::int32_t>(mt_arg_u32(&a));
    const auto to = static_cast<std::int32_t>(mt_arg_u32(&a));
    const std::string part = sub_chars(guest_string(cpu, text), from, to);
    mt_mem *mem = mt_mem_from_cpu(cpu);
    const u32 n = static_cast<u32>(part.size()) + 1u;
    const u32 p = mem != nullptr ? mt_heap_malloc(mem, n) : 0u;
    if (p == 0u) {
        logf("textpack: Text_SubChars: the guest heap is exhausted (%u bytes): aborting", n);
        mt_trap(cpu, MT_TRAP_ABORT, cpu->r[MT_LR], n);
    }
    std::memcpy(mt_g2h(cpu, p, n), part.c_str(), n);
    mt_ret_u32(cpu, p);
    return MT_HOOK_HANDLED;
}

void install_hooks(bool on) {
    if (on) {
        mt_hook_register(MT_HOOK_FN_TEXT_CHAR_COUNT, hook_char_count, nullptr, 1);
        mt_hook_register(MT_HOOK_FN_TEXT_SUB_CHARS, hook_sub_chars, nullptr, 1);
    } else {
        mt_hook_unregister(MT_HOOK_FN_TEXT_CHAR_COUNT);
        mt_hook_unregister(MT_HOOK_FN_TEXT_SUB_CHARS);
    }
}

} // namespace

const TextColumn *text_columns(std::size_t *count) {
    *count = sizeof k_columns / sizeof k_columns[0];
    return k_columns;
}

std::uint32_t char_count(const std::string &s) {
    std::uint32_t n = 0;
    for (std::size_t i = 0; i < s.size(); i += utf8_length(static_cast<std::uint8_t>(s[i]))) {
        n++;
    }
    return n;
}

std::string sub_chars(const std::string &s, std::int32_t from, std::int32_t to) {
    std::size_t i = 0;
    for (std::int32_t k = 0; k < from && i < s.size(); k++) {
        i += utf8_length(static_cast<std::uint8_t>(s[i]));
    }
    std::size_t start = i < s.size() ? i : s.size();
    std::size_t end = start;
    for (std::int32_t k = from; k < to && end < s.size(); k++) {
        end += utf8_length(static_cast<std::uint8_t>(s[end]));
    }
    end = end < s.size() ? end : s.size();
    return s.substr(start, end - start);
}

std::uint32_t patch_table(const std::string &rel, std::vector<std::uint8_t> &bytes) {
    if (!g_pack.open || g_pack.fields.empty()) {
        return 0;
    }
    // <Language>/data/<table>.dat
    const std::size_t slash = rel.find('/');
    if (slash == std::string::npos || rel.compare(slash, 6, "/data/") != 0 ||
        rel.size() < slash + 6 + 5 || rel.compare(rel.size() - 4, 4, ".dat") != 0) {
        return 0;
    }
    const std::string lang = rel.substr(0, slash);
    if (lang != "English" && lang != "Korean" && lang != "Japanese") {
        return 0;
    }
    const std::string table = rel.substr(slash + 6, rel.size() - slash - 6 - 4);
    const auto it = g_pack.fields.find(table);
    if (it == g_pack.fields.end()) {
        return 0;
    }
    const TextColumn &any = *it->second.front().column;
    std::uint16_t header[3] = {0, 0, 0};
    if (bytes.size() >= 6) {
        std::memcpy(header, bytes.data(), 6);
    }
    const std::uint64_t want = 6u + static_cast<std::uint64_t>(header[1]) * any.stride;
    if (bytes.size() < 6 || header[0] != any.columns || header[2] != any.stride ||
        bytes.size() != want) {
        logf("textpack: %s is not the table the game reads (%u columns, %u bytes a row, %zu "
             "bytes): the pack's %s text not written",
             rel.c_str(), static_cast<unsigned>(header[0]), static_cast<unsigned>(header[2]),
             bytes.size(), table.c_str());
        return 0;
    }
    std::uint32_t written = 0;
    for (const Field &f : it->second) {
        if (f.row >= header[1]) {
            if (g_pack.overflow_logged.insert("row:" + f.key).second) {
                logf("textpack: Tables.strings: \"%s\": %s has %u rows: ignored", f.key.c_str(),
                     table.c_str(), static_cast<unsigned>(header[1]));
            }
            continue;
        }
        const std::size_t room = f.column->width - 1u;
        const std::string v = utf8_cut(f.value, room);
        if (v.size() < f.value.size() && g_pack.overflow_logged.insert(f.key).second) {
            g_stats.overflows++;
            logf("textpack: overflow: Tables.strings \"%s\" is %zu bytes, its field holds %zu: "
                 "cut to \"%s\"",
                 f.key.c_str(), f.value.size(), room, v.c_str());
        }
        std::uint8_t *field = bytes.data() + 6u + f.row * f.column->stride + f.column->offset;
        std::memset(field, 0, f.column->width);
        std::memcpy(field, v.data(), v.size());
        written++;
    }
    g_stats.fields += written;
    return written;
}

void note_overflow(const std::string &utf8, float width, float limit) {
    if (!g_pack.open || width <= limit) {
        return;
    }
    if (g_pack.overflow_logged.size() < 4096 &&
        g_pack.overflow_logged.insert("text:" + utf8).second) {
        g_stats.overflows++;
        logf("textpack: overflow: \"%s\" is %.0f points wide, its texture %.0f: cut", utf8.c_str(),
             static_cast<double>(width), static_cast<double>(limit));
    }
}

} // namespace mt_textpack

using namespace mt_textpack;

extern "C" int mt_textpack_open(const char *dir) noexcept {
    mt_textpack_close();
    if (dir == nullptr || dir[0] == 0) {
        return -1;
    }
    try {
        std::error_code ec;
        const fs::path root = host_path(dir);
        if (!fs::is_directory(root, ec)) {
            logf("textpack: %s is no directory: no text pack", dir);
            return -1;
        }
        // The pack's files by their role, matched in any case.
        std::map<std::string, fs::path> found;
        for (const auto &entry : fs::directory_iterator(root, ec)) {
            const std::string name = lower(utf8_of(entry.path().filename()));
            for (const auto &f : k_files) {
                if (name == f[0] && entry.is_regular_file(ec)) {
                    found[f[1]] = entry.path();
                }
            }
        }
        Pack pack;
        pack.dir = dir;
        mt_textpack_stats stats{};
        std::map<std::string, std::string> manifest;
        for (const auto &f : k_files) {
            const auto it = found.find(f[1]);
            if (it == found.end()) {
                continue;
            }
            const std::string path = utf8_of(it->second);
            std::vector<std::uint8_t> bytes;
            std::string why;
            StringsFile file;
            if (!read_file(it->second, &bytes, &why) ||
                !parse_strings(bytes.data(), bytes.size(), &file, &why)) {
                stats.refused++;
                logf("textpack: %s: %s: refused", path.c_str(), why.c_str());
                continue;
            }
            stats.files++;
            if (file.duplicates != 0) {
                logf("textpack: %s: %u keys given more than once: the last value of each kept",
                     path.c_str(), file.duplicates);
            }
            const std::string table = f[1];
            if (table == "textpack") {
                manifest = std::move(file.entries);
                continue;
            }
            stats.entries += static_cast<u32>(file.entries.size());
            if (table == "Tables") {
                for (const auto &[key, value] : file.entries) {
                    const TextColumn *column = nullptr;
                    u32 row = 0;
                    std::string bad;
                    if (!parse_field_key(key, &column, &row, &bad)) {
                        stats.unknown++;
                        logf("textpack: %s: \"%s\": %s: ignored", path.c_str(), key.c_str(),
                             bad.c_str());
                        continue;
                    }
                    pack.fields[column->table].push_back({column, row, value, key});
                }
            } else {
                pack.tables[table] = std::move(file.entries);
            }
        }
        if (found.count("textpack") == 0) {
            logf("textpack: %s has no textpack.strings (the manifest): no text pack", dir);
            return -1;
        }
        const std::string bad = manifest.empty() && stats.refused != 0
                                    ? std::string("the manifest was refused")
                                    : check_manifest(manifest);
        if (!bad.empty()) {
            logf("textpack: %s/textpack.strings: %s: no text pack", dir, bad.c_str());
            return -1;
        }
        pack.tables["textpack"] = std::move(manifest);
        pack.open = true;
        g_pack = std::move(pack);
        g_stats = stats;
        mt_cf::set_korean_utf8(true);
        install_hooks(true);
        const auto &m = g_pack.tables["textpack"];
        const auto get = [&m](const char *k) {
            const auto it = m.find(k);
            return it != m.end() ? it->second.c_str() : "";
        };
        logf("textpack: %s: \"%s\" (%s, version %s, for the game's %s): %u files, %u entries, "
             "%u refused",
             dir, get("name"), get("language"), get("version")[0] != 0 ? get("version") : "-",
             get("game_version")[0] != 0 ? get("game_version") : "-", g_stats.files,
             g_stats.entries, g_stats.refused);
        return static_cast<int>(g_stats.entries);
    } catch (...) {
        logf("textpack: %s cannot be read: no text pack", dir);
        mt_textpack_close();
        return -1;
    }
}

extern "C" void mt_textpack_close(void) noexcept {
    if (g_pack.open) {
        install_hooks(false);
        mt_cf::set_korean_utf8(false);
    }
    g_pack = Pack{};
    g_stats = mt_textpack_stats{};
}

extern "C" int mt_textpack_is_open(void) noexcept { return g_pack.open ? 1 : 0; }

extern "C" const char *mt_textpack_lookup(const char *table, const char *key) noexcept {
    if (!g_pack.open || table == nullptr || key == nullptr) {
        return nullptr;
    }
    g_stats.lookups++;
    const auto t = g_pack.tables.find(table);
    if (t == g_pack.tables.end() || std::strcmp(table, "textpack") == 0) {
        return nullptr;
    }
    const auto it = t->second.find(key);
    if (it == t->second.end()) {
        return nullptr;
    }
    g_stats.replaced++;
    return it->second.c_str();
}

extern "C" const char *mt_textpack_manifest(const char *key) noexcept {
    if (!g_pack.open || key == nullptr) {
        return nullptr;
    }
    const auto &m = g_pack.tables["textpack"];
    const auto it = m.find(key);
    return it != m.end() ? it->second.c_str() : nullptr;
}

extern "C" mt_textpack_stats mt_textpack_get_stats(void) noexcept { return g_stats; }
