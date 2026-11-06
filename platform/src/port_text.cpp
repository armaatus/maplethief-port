// The port's own text in one table (#189, ADR 0108); see port_text.h.
#include "port_text.h"

#include <SDL3/SDL.h>

#include <array>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include "maplethief/textpack.h"

namespace maplethief::frontend {

namespace {

struct Entry {
    const char *key;
    const char *english;
};

constexpr Entry kTable[] = {
#define MT_TEXT(id, english) {#id, english},
#include "port_text.def"
#undef MT_TEXT
};
static_assert(sizeof kTable / sizeof kTable[0] == static_cast<std::size_t>(Txt::kCount));

// U+00A0..U+017F (Latin-1 Supplement, Latin Extended-A) in ASCII.
constexpr const char *kFold[] = {
    " ", "!",  "c",  "L",  "?", "Y", "|",  "S", "?", "(c)", "?", "\"", "?",   "-",   "(R)", "?",
    "o", "+-", "2",  "3",  "?", "u", "?",  ".", "?", "1",   "?", "\"", "1/4", "1/2", "3/4", "?",
    "A", "A",  "A",  "A",  "A", "A", "AE", "C", "E", "E",   "E", "E",  "I",   "I",   "I",   "I",
    "D", "N",  "O",  "O",  "O", "O", "O",  "x", "O", "U",   "U", "U",  "U",   "Y",   "Th",  "ss",
    "a", "a",  "a",  "a",  "a", "a", "ae", "c", "e", "e",   "e", "e",  "i",   "i",   "i",   "i",
    "d", "n",  "o",  "o",  "o", "o", "o",  "/", "o", "u",   "u", "u",  "u",   "y",   "th",  "y",
    "A", "a",  "A",  "a",  "A", "a", "C",  "c", "C", "c",   "C", "c",  "C",   "c",   "D",   "d",
    "D", "d",  "E",  "e",  "E", "e", "E",  "e", "E", "e",   "E", "e",  "G",   "g",   "G",   "g",
    "G", "g",  "G",  "g",  "H", "h", "H",  "h", "I", "i",   "I", "i",  "I",   "i",   "I",   "i",
    "I", "i",  "IJ", "ij", "J", "j", "K",  "k", "k", "L",   "l", "L",  "l",   "L",   "l",   "L",
    "l", "L",  "l",  "N",  "n", "N", "n",  "N", "n", "'n",  "N", "n",  "O",   "o",   "O",   "o",
    "O", "o",  "OE", "oe", "R", "r", "R",  "r", "R", "r",   "S", "s",  "S",   "s",   "S",   "s",
    "S", "s",  "T",  "t",  "T", "t", "T",  "t", "U", "u",   "U", "u",  "U",   "u",   "U",   "u",
    "U", "u",  "U",  "u",  "W", "w", "Y",  "y", "Y", "Z",   "z", "Z",  "z",   "Z",   "z",   "s",
};
static_assert(sizeof kFold / sizeof kFold[0] == 0x180 - 0xA0);

// The places "{N}" of a text, in order of first use; nullopt if a '{' starts no place.
std::optional<std::set<int>> places(std::string_view s) {
    std::set<int> out;
    for (std::size_t i = 0; i < s.size(); i++) {
        if (s[i] != '{') {
            continue;
        }
        const std::size_t close = s.find('}', i);
        if (close == std::string_view::npos || close == i + 1 || close > i + 3) {
            return std::nullopt;
        }
        int n = 0;
        for (std::size_t k = i + 1; k < close; k++) {
            if (s[k] < '0' || s[k] > '9') {
                return std::nullopt;
            }
            n = n * 10 + (s[k] - '0');
        }
        out.insert(n);
        i = close;
    }
    return out;
}

struct Cache {
    std::array<std::optional<std::string>, static_cast<std::size_t>(Txt::kCount)> ids;
    std::map<std::string, std::string> keys;
    std::set<std::string> refused; // logged once
};
Cache g_cache;

// The pack's text for `key` folded to ASCII, if it has one with the English's places.
std::optional<std::string> packed(const char *key, const char *english) {
    if (mt_textpack_is_open() == 0) {
        return std::nullopt;
    }
    const char *v = mt_textpack_lookup("Port", key);
    if (v == nullptr) {
        return std::nullopt;
    }
    const std::string folded = ascii_fold(v);
    if (places(folded) != places(english)) {
        if (g_cache.refused.insert(key).second) {
            SDL_Log("textpack: Port.strings \"%s\": its places ({0}, ...) are not the English "
                    "text's: the English kept",
                    key);
        }
        return std::nullopt;
    }
    return folded;
}

} // namespace

const char *port_text_key(Txt id) { return kTable[static_cast<std::size_t>(id)].key; }
const char *port_text_english(Txt id) { return kTable[static_cast<std::size_t>(id)].english; }

const char *tr(Txt id) {
    const Entry &e = kTable[static_cast<std::size_t>(id)];
    if (mt_textpack_is_open() == 0) {
        return e.english;
    }
    std::optional<std::string> &slot = g_cache.ids[static_cast<std::size_t>(id)];
    if (!slot) {
        slot = packed(e.key, e.english).value_or(e.english);
    }
    return slot->c_str();
}

std::string tr(Txt id, std::initializer_list<std::string_view> args) {
    const std::string_view text = tr(id);
    std::string out;
    out.reserve(text.size() + 16);
    for (std::size_t i = 0; i < text.size(); i++) {
        const std::size_t close = text[i] == '{' ? text.find('}', i) : std::string_view::npos;
        if (close != std::string_view::npos && close > i + 1 && close <= i + 3) {
            std::size_t n = 0;
            bool digits = true;
            for (std::size_t k = i + 1; k < close; k++) {
                digits = digits && text[k] >= '0' && text[k] <= '9';
                n = n * 10 + static_cast<std::size_t>(text[k] - '0');
            }
            if (digits) {
                if (n < args.size()) {
                    out += *(args.begin() + n);
                }
                i = close;
                continue;
            }
        }
        out.push_back(text[i]);
    }
    return out;
}

const char *tr_key(const std::string &key, const char *english) {
    if (mt_textpack_is_open() == 0 || english == nullptr) {
        return english;
    }
    auto it = g_cache.keys.find(key);
    if (it == g_cache.keys.end()) {
        it = g_cache.keys.emplace(key, packed(key.c_str(), english).value_or(english)).first;
    }
    return it->second.c_str();
}

void reload_port_text() { g_cache = Cache{}; }

std::string ascii_fold(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
            i++;
            continue;
        }
        // One UTF-8 character (strict), else one '?' for the byte.
        std::size_t len = 0;
        std::uint32_t cp = 0, min = 0;
        if (c >= 0xC2 && c <= 0xDF) {
            len = 2, cp = c & 0x1Fu, min = 0x80;
        } else if (c >= 0xE0 && c <= 0xEF) {
            len = 3, cp = c & 0x0Fu, min = 0x800;
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4, cp = c & 0x07u, min = 0x10000;
        }
        bool ok = len != 0 && i + len <= s.size();
        for (std::size_t k = 1; ok && k < len; k++) {
            const auto b = static_cast<unsigned char>(s[i + k]);
            ok = (b & 0xC0u) == 0x80u;
            cp = (cp << 6) | (b & 0x3Fu);
        }
        ok = ok && cp >= min && cp <= 0x10FFFF && !(cp >= 0xD800 && cp < 0xE000);
        if (!ok) {
            out.push_back('?');
            i++;
            continue;
        }
        i += len;
        if (cp >= 0xA0 && cp < 0x180) {
            out += kFold[cp - 0xA0];
        } else if (cp == 0x2018 || cp == 0x2019 || cp == 0x201A || cp == 0x2032) {
            out.push_back('\'');
        } else if (cp == 0x201C || cp == 0x201D || cp == 0x201E || cp == 0x2033) {
            out.push_back('"');
        } else if (cp >= 0x2010 && cp <= 0x2015) {
            out.push_back('-');
        } else if (cp == 0x2026) {
            out += "...";
        } else if (cp == 0x2022) {
            out.push_back('*');
        } else if (cp == 0x2009 || cp == 0x202F || cp == 0x2007) {
            out.push_back(' ');
        } else {
            out.push_back('?');
        }
    }
    return out;
}

} // namespace maplethief::frontend
