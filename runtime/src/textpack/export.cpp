// The text export (#371, ADR 0205, maplethief/packcheck.h): the game's text written out as a
// text pack to translate, from the game's own bundle, by the game's binary. What
// tools/mtport/textpack.py's export writes, without Python or the repository.
//
// The bundle is untrusted: its <lang>.lproj tables are parsed by the binary plist reader
// (foundation/plist.h), its data tables by their header (the layout textpack.cpp's text columns
// name), their text decoded as CP949 as the game does (docs/formats/data-tables.md).
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <system_error>
#include <tuple>
#include <vector>

#include "cf/cf_objects.h"
#include "foundation/plist.h"
#include "maplethief/packcheck.h"
#include "maplethief/saves.h"
#include "textpack/packcheck_internal.h"
#include "textpack/textpack_internal.h"

namespace {

namespace fs = std::filesystem;
using mt_packcheck::host_path;
using mt_packcheck::utf8_of;

// A bundle file this large is not the game's (its biggest table is under 1 MiB).
constexpr std::uintmax_t k_max_bundle_file = 16u << 20;

bool read_bundle_file(const fs::path &p, std::vector<std::uint8_t> *out) {
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(p, ec);
    if (ec || size > k_max_bundle_file) {
        return false;
    }
    std::ifstream in(p, std::ios::binary);
    out->resize(static_cast<std::size_t>(size));
    return in && (size == 0 || in.read(reinterpret_cast<char *>(out->data()),
                                       static_cast<std::streamsize>(size)));
}

std::string quoted(const std::string &s) {
    std::string q(mt_textpack_quote(s.c_str(), nullptr, 0) + 1u, '\0');
    mt_textpack_quote(s.c_str(), q.data(), static_cast<uint32_t>(q.size()));
    q.pop_back();
    return q;
}

// One entry of a .strings file: its key, value and a comment above it ("" for none).
using Entry = std::tuple<std::string, std::string, std::string>;

// The file's text, as textpack.py's _write lays it out.
std::string strings_text(const std::string &header, const std::vector<Entry> &entries) {
    std::string out = "/* " + header + " */\n\n";
    for (const auto &[key, value, comment] : entries) {
        if (!comment.empty()) {
            out += "/* " + comment + " */\n";
        }
        out += quoted(key) + " = " + quoted(value) + ";\n";
    }
    return out;
}

bool write_text(const fs::path &path, const std::string &text) {
    return mt_saves_replace_file(utf8_of(path).c_str(), text.data(), text.size()) == 0;
}

// A <lang>.lproj/<table>.strings of the bundle: its string entries, sorted by key.
bool lproj_table(const fs::path &file, std::vector<Entry> *out, std::string *why) {
    std::vector<std::uint8_t> bytes;
    mt_ns::PlistValue root;
    std::string error;
    if (!read_bundle_file(file, &bytes)) {
        *why = utf8_of(file) + ": cannot be read";
        return false;
    }
    if (!mt_ns::plist_parse(bytes.data(), bytes.size(), &root, &error) ||
        root.kind != mt_ns::PlistValue::dict) {
        *why = utf8_of(file) + ": not a table of strings (" + error + ")";
        return false;
    }
    std::map<std::string, std::string> sorted;
    for (const auto &[k, v] : root.entries) {
        if (v.kind == mt_ns::PlistValue::string) {
            sorted[mt_cf::to_utf8(k)] = mt_cf::to_utf8(v.s);
        }
    }
    for (auto &[k, v] : sorted) {
        out->emplace_back(k, v, "");
    }
    return true;
}

// The text columns of the language's data tables, "<table>.<row>.<column>" with their room.
bool table_texts(const fs::path &data, std::vector<Entry> *out, std::string *why) {
    std::size_t count = 0;
    const mt_textpack::TextColumn *columns = mt_textpack::text_columns(&count);
    std::map<std::string, std::vector<std::uint8_t>> files;
    for (std::size_t c = 0; c < count; ++c) {
        const mt_textpack::TextColumn &col = columns[c];
        auto file = files.find(col.table);
        if (file == files.end()) {
            std::vector<std::uint8_t> bytes;
            const fs::path p = data / (std::string(col.table) + ".dat");
            if (!read_bundle_file(p, &bytes)) {
                *why = utf8_of(p) + ": cannot be read";
                return false;
            }
            file = files.emplace(col.table, std::move(bytes)).first;
        }
        const std::vector<std::uint8_t> &b = file->second;
        std::uint16_t header[3] = {0, 0, 0};
        if (b.size() >= 6) {
            std::memcpy(header, b.data(), 6);
        }
        if (b.size() < 6 || header[0] != col.columns || header[2] != col.stride ||
            b.size() != 6u + static_cast<std::size_t>(header[1]) * col.stride) {
            *why = std::string(col.table) + ".dat is not the table the game reads";
            return false;
        }
        for (std::uint32_t row = 0; row < header[1]; ++row) {
            const std::uint8_t *field = b.data() + 6u + row * col.stride + col.offset;
            std::size_t n = 0;
            while (n < col.width && field[n] != 0) {
                ++n;
            }
            std::u16string text;
            if (n == 0 || !mt_cf::decode(field, n, mt_cf::k_enc_dos_korean, text)) {
                continue;
            }
            out->emplace_back(std::string(col.table) + "." + std::to_string(row) + "." + col.column,
                              mt_cf::to_utf8(text),
                              "at most " + std::to_string(col.width) + " bytes");
        }
    }
    return true;
}

} // namespace

extern "C" int mt_textpack_export(const char *bundle, const char *language, const char *dir,
                                  mt_packcheck_fn fn, void *user) noexcept {
    mt_packcheck::Reporter r(fn, user, "text");
    try {
        const std::string lang = language != nullptr && *language != '\0' ? language : "en";
        if (lang != "en" && lang != "ko") {
            r.rejected("the game's text is in en or ko, not " + lang);
            return -1;
        }
        const fs::path app = host_path(bundle != nullptr ? bundle : "");
        std::error_code ec;
        if (bundle == nullptr || *bundle == '\0' || !fs::is_directory(app, ec)) {
            r.rejected(std::string("the game's folder ") + (bundle != nullptr ? bundle : "") +
                       " is not there");
            return -1;
        }
        const fs::path out = host_path(dir != nullptr ? dir : "");
        fs::create_directories(out, ec);
        if (dir == nullptr || *dir == '\0' || !fs::is_directory(out, ec)) {
            r.rejected(std::string("the folder ") + (dir != nullptr ? dir : "") +
                       " cannot be made");
            return -1;
        }
        const std::string lproj = lang + ".lproj";
        const fs::path data = app / (lang == "en" ? "English" : "Korean") / "data";
        std::string why;
        std::map<std::string, std::vector<Entry>> files;
        for (const char *table : {"Scripts", "ScriptExtra"}) {
            if (!lproj_table(app / lproj / (std::string(table) + ".strings"), &files[table],
                             &why)) {
                r.rejected(why);
                return -1;
            }
        }
        if (!table_texts(data, &files["Tables"], &why)) {
            r.rejected(why);
            return -1;
        }
        const std::vector<Entry> manifest = {
            {"format", "1", ""},
            {"kind", "text", ""},
            {"id", "maplethief-text-" + lang, ""},
            {"name", "MapleThief text (" + lang + ")", ""},
            {"language", lang, ""},
            {"version", "1", ""},
            {"game_version", "1.5", ""},
            {"author", "", ""},
            {"content", "derived", ""},
        };
        const struct {
            const char *file, *header;
            const std::vector<Entry> *entries;
        } writes[] = {
            {"pack.strings",
             "The pack: give it your own id and name, set its language (en, de, pt-BR, ...) and "
             "your name. Made from the game's text: keep it to yourself (docs/play/packs.md).",
             &manifest},
            {"Scripts.strings", "Scripts: the game's dialogue, by its keys.", &files["Scripts"]},
            {"ScriptExtra.strings", "ScriptExtra: the game's dialogue, by its keys.",
             &files["ScriptExtra"]},
            {"Tables.strings",
             "Tables: names and descriptions, <table>.<row>.<column>; '$' is a line break. A "
             "value longer than its room is cut at a whole character.",
             &files["Tables"]},
        };
        int total = 0;
        for (const auto &w : writes) {
            if (!write_text(out / w.file, strings_text(w.header, *w.entries))) {
                r.rejected(utf8_of(out / w.file) + " cannot be written");
                return -1;
            }
            if (w.entries != &manifest) {
                total += static_cast<int>(w.entries->size());
                r.note(std::string(w.file) + ": " + std::to_string(w.entries->size()) + " entries");
            }
        }
        return total;
    } catch (const std::exception &) {
        r.rejected("the export failed");
        return -1;
    }
}
