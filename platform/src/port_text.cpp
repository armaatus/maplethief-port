// The port's own text in one table (#189, ADR 0108); see port_text.h.
#include "port_text.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

#include "canvas.h"
#include "maplethief/textpack.h"
#include "settings.h"
#include "settings_menu.h"

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

// The open pack's texts, looked up once each: the text, or nullopt where the pack has none.
struct Cache {
    std::array<std::optional<std::optional<std::string>>, static_cast<std::size_t>(Txt::kCount)>
        ids;
    std::map<std::string, std::optional<std::string>> keys;
    std::set<std::string> refused; // logged once
};
Cache g_cache;

// The menu language's texts (nullptr: English): never freed, so a text stays valid when the
// language changes.
struct MenuTexts {
    std::string tag;
    std::array<const char *, static_cast<std::size_t>(Txt::kCount)> ids{};
    std::map<std::string, std::string, std::less<>> keys;
};
const MenuTexts *g_language = nullptr;
std::string g_language_tag = "en";

// The menu language's text for `key`, else `english`.
const char *shipped(const std::string_view key, const char *english) {
    if (g_language == nullptr) {
        return english;
    }
    const auto it = g_language->keys.find(key);
    return it != g_language->keys.end() ? it->second.c_str() : english;
}

// The pack's text for `key`, if it has one with the English's places.
std::optional<std::string> packed(const char *key, const char *english) {
    if (mt_textpack_is_open() == 0) {
        return std::nullopt;
    }
    const char *v = mt_textpack_lookup("Port", key);
    if (v == nullptr) {
        return std::nullopt;
    }
    const std::string text = v;
    if (places(text) != places(english)) {
        if (g_cache.refused.insert(key).second) {
            SDL_Log("textpack: Port.strings \"%s\": its places ({0}, ...) are not the English "
                    "text's: the English kept",
                    key);
        }
        return std::nullopt;
    }
    return text;
}

} // namespace

const char *port_text_key(Txt id) { return kTable[static_cast<std::size_t>(id)].key; }
const char *port_text_english(Txt id) { return kTable[static_cast<std::size_t>(id)].english; }

const char *tr(Txt id) {
    const auto i = static_cast<std::size_t>(id);
    const Entry &e = kTable[i];
    if (mt_textpack_is_open() != 0) {
        std::optional<std::optional<std::string>> &slot = g_cache.ids[i];
        if (!slot) {
            slot = packed(e.key, e.english);
        }
        if (*slot) {
            return (*slot)->c_str();
        }
    }
    return g_language != nullptr && g_language->ids[i] != nullptr ? g_language->ids[i] : e.english;
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
    if (english == nullptr) {
        return english;
    }
    if (mt_textpack_is_open() != 0) {
        auto it = g_cache.keys.find(key);
        if (it == g_cache.keys.end()) {
            it = g_cache.keys.emplace(key, packed(key.c_str(), english)).first;
        }
        if (it->second) {
            return it->second->c_str();
        }
    }
    return shipped(key, english);
}

void reload_port_text() { g_cache = Cache{}; }

const std::vector<std::pair<std::string, std::string>> &port_text_rows() {
    static const std::vector<std::pair<std::string, std::string>> rows = [] {
        std::vector<std::pair<std::string, std::string>> out;
        std::set<std::string> keys;
        const auto add = [&](std::string key, const char *english) {
            if (english != nullptr && keys.insert(key).second) {
                out.emplace_back(std::move(key), english);
            }
        };
        for (std::size_t i = 0; i < static_cast<std::size_t>(Txt::kCount); ++i) {
            add(kTable[i].key, kTable[i].english);
        }
        for (const SettingInfo &r : settings_registry()) {
            add(std::string("setting.") + r.key + ".label", r.label);
            add(std::string("setting.") + r.key + ".help", r.help);
            add(std::string("setting.") + r.key + ".help_pad", r.help_pad);
        }
        for (const MenuActionInfo &a : menu_actions()) {
            const std::string base = std::string("action.") + a.key + ".";
            add(base + "label", a.label);
            add(base + "help", a.help);
            add(base + "help_pad", a.help_pad);
            add(base + "button", a.button);
            add(base + "confirm", a.confirm);
        }
        return out;
    }();
    return rows;
}

const std::vector<ShippedLanguage> &shipped_port_languages() {
    static const std::vector<ShippedLanguage> languages = [] {
        std::map<std::string, std::string, std::less<>> english;
        for (const auto &[key, text] : port_text_rows()) {
            english.emplace(key, text);
        }
        std::vector<ShippedLanguage> out;
        for (const ShippedLanguageFiles &f : shipped_port_language_files()) {
            ShippedLanguage l;
            l.tag = f.tag;
            using Map = std::map<std::string, std::string>;
            const auto parse = [&l](std::string_view bytes, const char *file, Map *entries) {
                char why[256] = {};
                const int n = mt_textpack_parse(
                    bytes.data(), static_cast<std::uint32_t>(bytes.size()),
                    [](void *user, const char *k, const char *v) {
                        static_cast<Map *>(user)->emplace(k, v);
                    },
                    entries, why, sizeof why);
                if (n < 0 && l.error.empty()) {
                    l.error = std::string(file) + ": " + why;
                }
                return n >= 0;
            };
            Map manifest;
            Map port;
            if (parse(f.manifest, "textpack.strings", &manifest) &&
                parse(f.port, "Port.strings", &port)) {
                l.name = manifest["name"];
                l.description = manifest["description"];
                if (manifest["language"] != l.tag) {
                    l.error = "textpack.strings: its language is not " + l.tag;
                }
                l.entries = port.size();
                for (auto &[key, text] : port) {
                    const auto en = english.find(key);
                    if (en == english.end()) {
                        l.unknown.push_back(key);
                    } else if (places(text) != places(en->second)) {
                        l.wrong_places.push_back(key);
                    } else {
                        l.texts.emplace_back(key, std::move(text));
                    }
                }
            }
            if (!l.error.empty()) {
                l.texts.clear();
                SDL_Log("menu language %s: %s: refused", l.tag.c_str(), l.error.c_str());
            }
            out.push_back(std::move(l));
        }
        return out;
    }();
    return languages;
}

std::vector<std::string> menu_languages() {
    std::vector<std::string> out{"en"};
    for (const ShippedLanguage &l : shipped_port_languages()) {
        out.push_back(l.tag);
    }
    return out;
}

bool set_menu_language(std::string_view tag) {
    static std::map<std::string, std::unique_ptr<MenuTexts>, std::less<>> loaded;
    if (tag == g_language_tag) {
        return true;
    }
    if (tag == "en") {
        g_language = nullptr;
        g_language_tag = "en";
        SDL_Log("menu language: en");
        return true;
    }
    const auto &shipped_list = shipped_port_languages();
    const auto l = std::find_if(shipped_list.begin(), shipped_list.end(),
                                [tag](const ShippedLanguage &s) { return s.tag == tag; });
    if (l == shipped_list.end()) {
        return false;
    }
    auto it = loaded.find(tag);
    if (it == loaded.end()) {
        auto made = std::make_unique<MenuTexts>();
        made->tag = l->tag;
        for (const auto &[key, text] : l->texts) {
            made->keys.emplace(key, text);
        }
        for (std::size_t i = 0; i < static_cast<std::size_t>(Txt::kCount); ++i) {
            const auto k = made->keys.find(std::string_view(kTable[i].key));
            made->ids[i] = k != made->keys.end() ? k->second.c_str() : nullptr;
        }
        it = loaded.emplace(l->tag, std::move(made)).first;
    }
    g_language = it->second.get();
    g_language_tag = l->tag;
    SDL_Log("menu language: %s (%zu of %zu texts)", l->tag.c_str(), l->texts.size(),
            port_text_rows().size());
    return true;
}

const std::string &menu_language() { return g_language_tag; }

std::string apply_menu_language(MenuLanguage setting) {
    const auto i = static_cast<std::size_t>(setting);
    const std::string tag = setting == MenuLanguage::kAuto || i >= std::size(kMenuLanguageNames)
                                ? system_menu_language()
                                : kMenuLanguageNames[i];
    if (!set_menu_language(tag)) {
        set_menu_language("en");
    }
    return menu_language();
}

std::string menu_language_name(std::string_view tag) {
    if (tag == "auto") {
        return tr(Txt::menu_language_auto, {menu_language_name(system_menu_language())});
    }
    if (tag == "en") {
        return "English";
    }
    for (const ShippedLanguage &l : shipped_port_languages()) {
        if (l.tag == tag && !l.name.empty()) {
            return l.name;
        }
    }
    return std::string(tag);
}

std::string system_menu_language() {
    const std::vector<std::string> ours = menu_languages();
    int count = 0;
    SDL_Locale **locales = SDL_GetPreferredLocales(&count);
    std::string out = "en";
    for (int i = 0; locales != nullptr && i < count; ++i) {
        if (locales[i] == nullptr || locales[i]->language == nullptr) {
            continue;
        }
        std::string language = locales[i]->language;
        std::transform(language.begin(), language.end(), language.begin(), [](char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        });
        if (std::find(ours.begin(), ours.end(), language) != ours.end()) {
            out = language;
            break;
        }
    }
    SDL_free(locales);
    return out;
}

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
        const std::int32_t cp = next_code_point(s, &i);
        if (cp < 0) {
            out.push_back('?');
            continue;
        }
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
