// The pack manifest, pack.strings (ADR 0149), as the checks read it, and the check that tells a
// pack's kind (#371, ADR 0205, maplethief/packcheck.h). The manifest is untrusted data in ADR
// 0108's .strings syntax: strings_file.cpp parses it with its limits.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <system_error>
#include <vector>

#include "maplethief/packcheck.h"
#include "maplethief/textpack.h"
#include "textpack/packcheck_internal.h"
#include "textpack/strings_file.h"

namespace mt_packcheck {

namespace fs = std::filesystem;

fs::path host_path(const std::string &utf8) {
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string utf8_of(const fs::path &p) {
    const std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

bool parse_version(const std::string &v, unsigned out[3]) {
    std::size_t at = 0;
    for (int part = 0; part < 3; ++part) {
        const std::size_t start = at;
        unsigned n = 0;
        while (at < v.size() && v[at] >= '0' && v[at] <= '9' && at - start < 6) {
            n = n * 10u + static_cast<unsigned>(v[at] - '0');
            ++at;
        }
        if (at == start) {
            return false;
        }
        out[part] = n;
        if (part < 2) {
            if (at >= v.size() || v[at] != '.') {
                return false;
            }
            ++at;
        }
    }
    // A release candidate counts as its version: "-rc.N" and nothing else may follow.
    return at == v.size() || v.compare(at, 4, "-rc.") == 0;
}

namespace {

// The characters (not bytes) of UTF-8 text the parser has checked.
std::size_t chars(const std::string &s) {
    std::size_t n = 0;
    for (const char c : s) {
        n += (static_cast<unsigned char>(c) & 0xC0u) != 0x80u ? 1u : 0u;
    }
    return n;
}

bool valid_id(const std::string &id) {
    if (id.empty() || id.size() > 64) {
        return false;
    }
    for (std::size_t i = 0; i < id.size(); ++i) {
        const char c = id[i];
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!alnum && (i == 0 || (c != '.' && c != '_' && c != '-'))) {
            return false;
        }
    }
    return true;
}

// ADR 0108's rule: letters, digits, '-' and '_', at most 35.
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

constexpr const char *k_game_version = "1.5";

// The keys ADR 0149 names: unknown ones are ignored, so a later format 1 may add some.
constexpr const char *k_keys[] = {"format",       "kind",     "id",          "name",
                                  "version",      "author",   "description", "language",
                                  "game_version", "port_min", "content",     "license"};

} // namespace

bool check_manifest(const fs::path &dir, const char *kind, Reporter &r,
                    std::map<std::string, std::string> *out) {
    out->clear();
    const fs::path path = dir / k_manifest;
    std::error_code ec;
    const fs::file_status st = fs::symlink_status(path, ec);
    if (ec || !fs::exists(st)) {
        r.note(std::string(k_manifest) + ": none: the game shows the folder's name");
        return false;
    }
    if (fs::is_symlink(st) || !fs::is_regular_file(st)) {
        r.ignored(std::string(k_manifest) + ": not a file of its own (a link is never followed)");
        return false;
    }
    const std::uintmax_t size = fs::file_size(path, ec);
    std::vector<std::uint8_t> bytes;
    std::string why;
    if (ec) {
        why = "cannot be read";
    } else if (size > MT_TEXTPACK_MAX_FILE) {
        why = "larger than " + std::to_string(MT_TEXTPACK_MAX_FILE >> 20) + " MiB";
    } else {
        std::ifstream in(path, std::ios::binary);
        bytes.resize(static_cast<std::size_t>(size));
        if (!in || (size > 0 && !in.read(reinterpret_cast<char *>(bytes.data()),
                                         static_cast<std::streamsize>(size)))) {
            why = "cannot be read";
        }
    }
    mt_textpack::StringsFile file;
    // A parse error's reason names its line ("line 3: ...").
    if (!why.empty() || !mt_textpack::parse_strings(bytes.data(), bytes.size(), &file, &why)) {
        r.refused(std::string(k_manifest) + ": " + why + ": the pack is refused");
        return true;
    }
    if (file.duplicates != 0) {
        r.note(std::string(k_manifest) + ": " + std::to_string(file.duplicates) +
               " keys given more than once: the last value of each kept");
    }
    const auto &m = file.entries;
    const auto get = [&m](const char *k) {
        const auto it = m.find(k);
        return it != m.end() ? it->second : std::string();
    };
    const auto refuse = [&r](const std::string &reason) {
        r.refused(std::string(k_manifest) + ": " + reason + ": the pack is refused");
    };
    const std::string format = get("format");
    if (format.empty()) {
        refuse("it has no \"format\"");
    } else if (format != "1") {
        unsigned v[3] = {0, 0, 0};
        const bool number = format.find_first_not_of("0123456789") == std::string::npos &&
                            format.size() < 7 && parse_version(format + ".0.0", v);
        refuse(number && v[0] > 1 ? "pack format " + format + " needs a newer port"
                                  : "its \"format\" is not \"1\"");
    }
    const std::string k = get("kind");
    if (k != "texture" && k != "audio" && k != "text") {
        refuse("its \"kind\" is not texture, audio or text");
    } else if (kind != nullptr && *kind != '\0' && k != kind) {
        refuse("a " + k + " pack, opened as a " + kind + " pack");
    }
    if (!valid_id(get("id"))) {
        refuse("its \"id\" is not 1 to 64 of a-z, 0-9, '.', '_' and '-', starting with a letter "
               "or digit");
    }
    const std::string name = get("name");
    if (name.empty() || chars(name) > 64) {
        refuse("its \"name\" is missing or longer than 64 characters");
    }
    const struct {
        const char *key;
        std::size_t max;
    } lengths[] = {{"version", 32}, {"author", 128}, {"description", 1024}};
    for (const auto &l : lengths) {
        if (chars(get(l.key)) > l.max) {
            refuse("its \"" + std::string(l.key) + "\" is longer than " + std::to_string(l.max) +
                   " characters");
        }
    }
    const std::string language = get("language");
    if ((k == "text" || !language.empty()) && !valid_language(language)) {
        refuse("its \"language\" is not a language tag (letters, digits and '-', such as \"de\" "
               "or \"pt-BR\")");
    }
    const std::string game = get("game_version");
    if (!game.empty() && game != k_game_version) {
        refuse("made for the game's version " + game + "; this port runs " + k_game_version);
    }
    const std::string port_min = get("port_min");
    unsigned want[3] = {0, 0, 0}, have[3] = {0, 0, 0};
    if (!port_min.empty() && !parse_version(port_min, want)) {
        refuse("its \"port_min\" is not a version X.Y.Z");
    } else if (!port_min.empty() && parse_version(MT_VERSION, have) &&
               (want[0] != have[0]   ? want[0] > have[0]
                : want[1] != have[1] ? want[1] > have[1]
                                     : want[2] > have[2])) {
        refuse("needs port " + port_min + " or later; this is " + MT_VERSION);
    }
    const std::string content = get("content");
    if (!content.empty() && content != "own" && content != "derived") {
        r.ignored(std::string(k_manifest) + ": its \"content\" is neither own nor derived: not "
                                            "shown");
    }
    for (const auto &[key, value] : m) {
        bool known = false;
        for (const char *kk : k_keys) {
            known = known || key == kk;
        }
        if (!known) {
            r.note(std::string(k_manifest) + ": \"" + key + "\" is not a key of format 1: ignored");
        }
    }
    *out = m;
    return true;
}

} // namespace mt_packcheck

namespace {

namespace fs = std::filesystem;
using mt_packcheck::host_path;
using mt_packcheck::utf8_of;

std::string lower(std::string s) {
    for (char &c : s) {
        c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return s;
}

// The kind's name as a static string, for the report.
const char *kind_name(const std::string &k) {
    return k == "texture" ? "texture" : k == "audio" ? "audio" : k == "text" ? "text" : "";
}

} // namespace

extern "C" const char *mt_pack_kind(const char *dir) noexcept {
    try {
        if (dir == nullptr || *dir == '\0') {
            return "";
        }
        const fs::path root = host_path(dir);
        std::error_code ec;
        if (!fs::is_directory(root, ec)) {
            return "";
        }
        mt_packcheck::Reporter quiet(nullptr, nullptr, "");
        std::map<std::string, std::string> m;
        if (mt_packcheck::check_manifest(root, nullptr, quiet, &m)) {
            const auto it = m.find("kind");
            if (it != m.end() && *kind_name(it->second) != '\0') {
                return kind_name(it->second);
            }
        }
        bool texture = false, audio = false, text = false;
        unsigned seen = 0;
        for (fs::directory_iterator it(root, ec), end; !ec && it != end && seen < 4096;
             it.increment(ec), ++seen) {
            const std::string name = lower(utf8_of(it->path().filename()));
            const std::size_t dot = name.rfind('.');
            const std::string ext = dot == std::string::npos ? "" : name.substr(dot + 1);
            if (name == mt_packcheck::k_text_manifest) {
                text = true;
            } else if (name == "ui" || (ext == "png" && name.size() > 17 && name[16] == '_')) {
                texture = true;
            } else if (ext == "wav" || ext == "mp3" || ext == "flac" || ext == "loop") {
                audio = true;
            }
        }
        return text ? "text" : texture ? "texture" : audio ? "audio" : "";
    } catch (...) {
        return "";
    }
}

extern "C" mt_packcheck_report mt_pack_check(const char *dir, const char *kind, const char *bundle,
                                             mt_packcheck_fn fn, void *user) noexcept {
    const char *k = kind != nullptr && *kind != '\0' ? kind : mt_pack_kind(dir);
    if (std::strcmp(k, "texture") == 0) {
        return mt_texpack_check(dir, bundle, fn, user);
    }
    if (std::strcmp(k, "audio") == 0) {
        return mt_audio_pack_check(dir, bundle, fn, user);
    }
    if (std::strcmp(k, "text") == 0) {
        return mt_textpack_check(dir, fn, user);
    }
    mt_packcheck::Reporter r(fn, user, "");
    r.refused(std::string(dir != nullptr ? dir : "") +
              ": no pack of any kind (no pack.strings, textpack.strings, texture or sound file)");
    return r.report;
}

extern "C" uint32_t mt_textpack_quote(const char *s, char *out, uint32_t size) noexcept {
    std::string q = "\"";
    for (const char *p = s != nullptr ? s : ""; *p != '\0'; ++p) {
        const auto c = static_cast<unsigned char>(*p);
        switch (c) {
        case '"':
            q += "\\\"";
            break;
        case '\\':
            q += "\\\\";
            break;
        case '\n':
            q += "\\n";
            break;
        case '\t':
            q += "\\t";
            break;
        case '\r':
            q += "\\r";
            break;
        default:
            if (c < 0x20u || c == 0x7Fu) {
                char esc[8];
                std::snprintf(esc, sizeof esc, "\\U%04X", static_cast<unsigned>(c));
                q += esc;
            } else {
                q += static_cast<char>(c);
            }
        }
    }
    q += '"';
    if (out != nullptr && size != 0) {
        const std::size_t n = q.size() < size ? q.size() : size - 1u;
        std::memcpy(out, q.data(), n);
        out[n] = '\0';
    }
    return static_cast<uint32_t>(q.size());
}
