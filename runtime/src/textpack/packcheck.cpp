// What the pack checks share (#371, ADR 0205, maplethief/packcheck.h): pack.strings checked by
// the manifest reader (pack_manifest.cpp, ADR 0149) plus the notes the reader has no words for,
// the check that tells a pack's kind, and the .strings quoting of the exports.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <system_error>
#include <vector>

#include "maplethief/pack_manifest.h"
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

namespace {

// The keys ADR 0149 names: unknown ones are ignored, so a later format 1 may add some.
constexpr const char *k_keys[] = {"format",       "kind",     "id",          "name",
                                  "version",      "author",   "description", "language",
                                  "game_version", "port_min", "content",     "license"};

// pack.strings' entries, through the strict parser; false with `*why` ("line 3: ...").
bool read_manifest(const fs::path &path, mt_textpack::StringsFile *file, std::string *why) {
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
    return mt_textpack::parse_strings(bytes.data(), bytes.size(), file, why);
}

} // namespace

std::string manifest_kind(const fs::path &dir) {
    const fs::path path = dir / k_manifest;
    std::error_code ec;
    mt_textpack::StringsFile file;
    std::string why;
    if (!fs::is_regular_file(fs::symlink_status(path, ec)) || !read_manifest(path, &file, &why)) {
        return "";
    }
    const auto it = file.entries.find("kind");
    return it != file.entries.end() ? it->second : std::string();
}

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
    // The rules are the manifest reader's (maplethief/pack_manifest.h), as the packs screen and
    // the start apply them; a pack checked without a kind is read as the kind it names.
    const std::string named = manifest_kind(dir);
    const char *as = kind != nullptr && *kind != '\0'      ? kind
                     : named == "audio" || named == "text" ? named.c_str()
                                                           : "texture";
    mt_pack_manifest m;
    if (mt_pack_manifest_read(utf8_of(dir).c_str(), as, MT_VERSION, &m) == 0) {
        r.refused(std::string(m.refused) + ": the pack is refused");
        return true;
    }
    // What the reader passes over without a word: said here, so the author knows.
    mt_textpack::StringsFile file;
    std::string why;
    if (!read_manifest(path, &file, &why)) {
        return true;
    }
    if (file.duplicates != 0) {
        r.note(std::string(k_manifest) + ": " + std::to_string(file.duplicates) +
               " keys given more than once: the last value of each kept");
    }
    const auto content = file.entries.find("content");
    if (content != file.entries.end() && content->second != "own" && content->second != "derived") {
        r.ignored(std::string(k_manifest) + ": its \"content\" is neither own nor derived: not "
                                            "shown");
    }
    for (const auto &[key, value] : file.entries) {
        bool known = false;
        for (const char *kk : k_keys) {
            known = known || key == kk;
        }
        if (!known) {
            r.note(std::string(k_manifest) + ": \"" + key + "\" is not a key of format 1: ignored");
        }
    }
    *out = std::move(file.entries);
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
        const char *named = kind_name(mt_packcheck::manifest_kind(root));
        if (*named != '\0') {
            return named;
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
