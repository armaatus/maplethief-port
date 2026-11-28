// The pack manifest (#370, ADR 0149, maplethief/pack_manifest.h): pack.strings (or a text
// pack's textpack.strings) through strings_file.cpp's strict parser, then ADR 0149's rules.
#include "maplethief/pack_manifest.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <system_error>
#include <vector>

#include "maplethief/textpack.h"
#include "textpack/strings_file.h"

namespace {

namespace fs = std::filesystem;
using Entries = std::map<std::string, std::string>;

fs::path host_path(const std::string &utf8) {
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string utf8_of(const fs::path &p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

template <std::size_t N> void put(char (&field)[N], const std::string &value) {
    const std::string cut = mt_textpack::utf8_cut(value, N - 1);
    std::memcpy(field, cut.data(), cut.size());
    field[cut.size()] = 0;
}

// Characters (code points) of valid UTF-8.
std::size_t characters(const std::string &s) {
    std::size_t n = 0;
    for (const char c : s) {
        n += (static_cast<unsigned char>(c) & 0xC0) != 0x80 ? 1 : 0;
    }
    return n;
}

// The folder's own name: the last part of `dir`, without a trailing separator.
std::string folder_name(const std::string &dir) {
    std::string d = dir;
    while (d.size() > 1 && (d.back() == '/' || d.back() == '\\')) {
        d.pop_back();
    }
    const std::size_t cut = d.find_last_of("/\\");
    return cut == std::string::npos ? d : d.substr(cut + 1);
}

// The manifest file in `root` named `want` (any case), not a symbolic link; empty if none.
fs::path find_file(const fs::path &root, const char *want) {
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(root, ec)) {
        std::string name = utf8_of(entry.path().filename());
        for (char &c : name) {
            c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        }
        std::error_code fe;
        const fs::file_status status = entry.symlink_status(fe);
        if (name == want && !fe && fs::is_regular_file(status)) {
            return entry.path();
        }
    }
    return {};
}

// Reads and parses the .strings file at `path`; false with `*why` ("line 3: ...").
bool read_strings(const fs::path &path, Entries *out, std::string *why) {
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
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream in(path, std::ios::binary);
    if (!in || (size > 0 && !in.read(reinterpret_cast<char *>(bytes.data()),
                                     static_cast<std::streamsize>(size)))) {
        *why = "cannot be read";
        return false;
    }
    mt_textpack::StringsFile file;
    if (!mt_textpack::parse_strings(bytes.data(), bytes.size(), &file, why)) {
        return false;
    }
    *out = std::move(file.entries);
    return true;
}

bool valid_id(const std::string &id) {
    if (id.empty() || id.size() > MT_PACK_MAX_ID) {
        return false;
    }
    for (std::size_t i = 0; i < id.size(); i++) {
        const char c = id[i];
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!alnum && (i == 0 || (c != '.' && c != '_' && c != '-'))) {
            return false;
        }
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

// X.Y.Z[-rc.N] as four numbers (N: UINT64_MAX for a release, so it sorts after its rcs).
bool parse_version(const char *text, std::uint64_t (&v)[4]) {
    if (text == nullptr) {
        return false;
    }
    const char *p = text;
    for (int i = 0; i < 3; i++) {
        if (*p < '0' || *p > '9' || (*p == '0' && p[1] >= '0' && p[1] <= '9')) {
            return false;
        }
        std::uint64_t n = 0;
        for (; *p >= '0' && *p <= '9'; p++) {
            if (n > 1'000'000'000) {
                return false;
            }
            n = n * 10 + static_cast<std::uint64_t>(*p - '0');
        }
        v[i] = n;
        if (i < 2 && *p++ != '.') {
            return false;
        }
    }
    v[3] = UINT64_MAX;
    if (*p == 0) {
        return true;
    }
    if (std::strncmp(p, "-rc.", 4) != 0 || p[4] < '0' || p[4] > '9') {
        return false;
    }
    std::uint64_t n = 0;
    for (p += 4; *p >= '0' && *p <= '9'; p++) {
        if (n > 1'000'000'000) {
            return false;
        }
        n = n * 10 + static_cast<std::uint64_t>(*p - '0');
    }
    v[3] = n;
    return *p == 0;
}

// Why ADR 0149 refuses `m` as a pack of `kind` for `port`, or "".
std::string check(const Entries &m, const std::string &kind, const char *port, bool own) {
    const auto get = [&m](const char *k) {
        const auto it = m.find(k);
        return it != m.end() ? it->second : std::string();
    };
    const std::string format = get("format");
    if (format.empty()) {
        return "it has no \"format\"";
    }
    if (format != "1") {
        const bool number =
            format.size() <= 9 && format.find_first_not_of("0123456789") == std::string::npos;
        return number && format[0] != '0' ? "pack format " + format + " needs a newer port"
                                          : "its \"format\" is not a number";
    }
    if (own) {
        const std::string k = get("kind");
        if (k.empty()) {
            return "it has no \"kind\"";
        }
        if (k != "texture" && k != "audio" && k != "text") {
            return "its \"kind\" is not texture, audio or text";
        }
        if (k != kind) {
            return "it is a" + std::string(k == "audio" ? "n " : " ") + k + " pack, not a" +
                   (kind == "audio" ? "n " : " ") + kind + " pack";
        }
        if (!valid_id(get("id"))) {
            return "its \"id\" is not 1 to 64 of a-z, 0-9, '.', '_' and '-'";
        }
    }
    const std::string name = get("name");
    if (name.empty()) {
        return "it has no \"name\"";
    }
    if (characters(name) > MT_PACK_MAX_NAME) {
        return "its \"name\" is longer than 64 characters";
    }
    if (characters(get("version")) > MT_PACK_MAX_VERSION) {
        return "its \"version\" is longer than 32 characters";
    }
    if (characters(get("author")) > MT_PACK_MAX_AUTHOR) {
        return "its \"author\" is longer than 128 characters";
    }
    if (characters(get("description")) > MT_PACK_MAX_DESCRIPTION) {
        return "its \"description\" is longer than 1,024 characters";
    }
    if (kind == "text" && !valid_language(get("language"))) {
        return "its \"language\" is not a language tag (such as \"de\" or \"pt-BR\")";
    }
    const std::string game = get("game_version");
    if (!game.empty() && game != MT_PACK_MANIFEST_GAME) {
        return "made for the game's version " + mt_textpack::utf8_cut(game, 16) +
               "; this port runs " MT_PACK_MANIFEST_GAME;
    }
    const std::string port_min = get("port_min");
    if (!port_min.empty()) {
        std::uint64_t v[4];
        if (!parse_version(port_min.c_str(), v)) {
            return "its \"port_min\" is not a version (X.Y.Z)";
        }
        int ok = 0;
        if (port != nullptr && port[0] != 0 &&
            mt_pack_compare_versions(port_min.c_str(), port, &ok) > 0 && ok != 0) {
            return "needs port " + port_min + " or later; this is " + port;
        }
    }
    return "";
}

} // namespace

extern "C" int mt_pack_manifest_kind(const std::uint8_t *bytes, std::size_t n,
                                     char kind[8]) MT_NOEXCEPT {
    if (kind == nullptr) {
        return 0;
    }
    kind[0] = 0;
    if (bytes == nullptr && n != 0) {
        return 0;
    }
    try {
        mt_textpack::StringsFile file;
        std::string why;
        if (n > MT_TEXTPACK_MAX_FILE || !mt_textpack::parse_strings(bytes, n, &file, &why)) {
            return 0;
        }
        const auto it = file.entries.find("kind");
        if (it == file.entries.end() ||
            (it->second != "texture" && it->second != "audio" && it->second != "text")) {
            return 0;
        }
        std::memcpy(kind, it->second.c_str(), it->second.size() + 1);
        return 1;
    } catch (...) {
        return 0;
    }
}

extern "C" int mt_pack_compare_versions(const char *a, const char *b, int *ok) MT_NOEXCEPT {
    std::uint64_t x[4], y[4];
    const bool good = parse_version(a, x) && parse_version(b, y);
    if (ok != nullptr) {
        *ok = good ? 1 : 0;
    }
    if (!good) {
        return 0;
    }
    for (int i = 0; i < 4; i++) {
        if (x[i] != y[i]) {
            return x[i] < y[i] ? -1 : 1;
        }
    }
    return 0;
}

extern "C" int mt_pack_manifest_read(const char *dir, const char *kind, const char *port_version,
                                     mt_pack_manifest *out) MT_NOEXCEPT {
    if (out == nullptr) {
        return 0;
    }
    *out = mt_pack_manifest{};
    const std::string k = kind != nullptr ? kind : "";
    const std::string d = dir != nullptr ? dir : "";
    try {
        const std::string folder = folder_name(d);
        put(out->kind, k);
        put(out->name, folder);
        put(out->id, folder);
        const auto refuse = [out](const std::string &why) {
            put(out->refused, why);
            return 0;
        };
        if (k != "texture" && k != "audio" && k != "text") {
            return refuse("no such pack kind");
        }
        std::error_code ec;
        const fs::path root = host_path(d);
        if (d.empty() || !fs::is_directory(root, ec)) {
            return refuse("the folder is missing");
        }
        fs::path file = find_file(root, "pack.strings");
        const bool own = !file.empty();
        if (!own && k == "text") {
            file = find_file(root, "textpack.strings");
            if (file.empty()) {
                return refuse("it has no pack.strings or textpack.strings");
            }
        }
        if (file.empty()) {
            return 1; // a bare texture or audio folder (ADR 0081, 0106)
        }
        put(out->source, own ? "pack.strings" : "textpack.strings");
        Entries m;
        std::string why;
        if (!read_strings(file, &m, &why)) {
            return refuse(std::string(out->source) + ": " + why);
        }
        const auto get = [&m](const char *key) {
            const auto it = m.find(key);
            return it != m.end() ? it->second : std::string();
        };
        if (!get("name").empty()) {
            put(out->name, get("name"));
        }
        if (own && valid_id(get("id"))) {
            put(out->id, get("id"));
        }
        put(out->version, get("version"));
        put(out->author, get("author"));
        put(out->description, get("description"));
        put(out->language, get("language"));
        put(out->game_version, get("game_version"));
        put(out->port_min, get("port_min"));
        put(out->content, get("content"));
        put(out->license, get("license"));
        why = check(m, k, port_version, own);
        if (!why.empty()) {
            return refuse(std::string(out->source) + ": " + why);
        }
        return 1;
    } catch (...) {
        put(out->refused, "the folder cannot be read");
        return 0;
    }
}
