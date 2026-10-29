// The frontend's settings file (#59, ADR 0043); see settings.h.
#include "settings.h"

#include <cstdio>

#include "maplethief/gfx.h"

namespace maplethief::frontend {

namespace {

bool is_blank(char c) { return c == ' ' || c == '\t'; }

std::string_view trim(std::string_view s) {
    while (!s.empty() && is_blank(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && is_blank(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

bool is_key_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

bool key_is(std::string_view key, std::string_view name) {
    if (key.size() != name.size()) {
        return false;
    }
    for (std::size_t i = 0; i < key.size(); i++) {
        char c = key[i];
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if (c != name[i]) {
            return false;
        }
    }
    return true;
}

// A small decimal number: digits only (no sign, no blanks), at most 3 of them.
bool parse_small_uint(std::string_view s, std::uint32_t *out) {
    if (s.empty() || s.size() > 3) {
        return false;
    }
    std::uint32_t v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
        v = v * 10 + static_cast<std::uint32_t>(c - '0');
    }
    *out = v;
    return true;
}

} // namespace

std::string printable(std::string_view s, std::size_t max) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); i++) {
        if (i == max) {
            out += "...";
            break;
        }
        const auto c = static_cast<unsigned char>(s[i]);
        if (c >= 0x20 && c < 0x7F && c != '\\') {
            out += static_cast<char>(c);
        } else {
            char hex[8];
            std::snprintf(hex, sizeof hex, "\\x%02X", c);
            out += hex;
        }
    }
    return out;
}

bool apply_setting(Settings *settings, std::string_view key, std::string_view value,
                   std::string *why) {
    auto fail = [&](const std::string &what) {
        if (why != nullptr) {
            *why = what;
        }
        return false;
    };
    if (key_is(key, "scale_mode")) {
        ScaleMode mode{};
        if (!parse_scale_mode(value, &mode)) {
            return fail("scale_mode: '" + printable(value) +
                        "' is not integer, fit, smooth or sharp");
        }
        settings->scale_mode = mode;
        return true;
    }
    if (key_is(key, "border_color")) {
        Color color;
        if (!parse_color(value, &color)) {
            return fail("border_color: '" + printable(value) + "' is not #RRGGBB");
        }
        settings->border_color = color;
        return true;
    }
    if (key_is(key, "render_scale")) {
        std::uint32_t scale = 0;
        if (!parse_small_uint(value, &scale) || scale < 1 || scale > MT_GFX_MAX_SCALE) {
            return fail("render_scale: '" + printable(value) + "' is not an integer from 1 to " +
                        std::to_string(MT_GFX_MAX_SCALE));
        }
        settings->render_scale = scale;
        return true;
    }
    return fail("unknown key '" + printable(key) + "'");
}

std::vector<std::string> parse_settings(std::string_view text, Settings *settings) {
    std::vector<std::string> problems;
    if (text.size() > kMaxSettingsBytes) {
        problems.push_back("the file has " + std::to_string(text.size()) + " bytes, more than " +
                           std::to_string(kMaxSettingsBytes) + ": ignored");
        return problems;
    }
    if (text.substr(0, 3) == "\xEF\xBB\xBF") {
        text.remove_prefix(3);
    }
    // The keys seen so far and the line that set them, to report a repeated key.
    std::vector<std::pair<std::string, std::size_t>> seen;
    std::size_t number = 0;
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
        number++;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        const std::string at = "line " + std::to_string(number) + ": ";
        line = trim(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        if (line.front() == '[') {
            problems.push_back(at + "sections are not supported: '" + printable(line) +
                               "' ignored");
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            problems.push_back(at + "not key = value: '" + printable(line) + "'");
            continue;
        }
        const std::string_view key = trim(line.substr(0, eq));
        const std::string_view value = trim(line.substr(eq + 1));
        bool key_ok = !key.empty();
        for (const char c : key) {
            key_ok = key_ok && is_key_char(c);
        }
        if (!key_ok) {
            problems.push_back(at + "not a key: '" + printable(key) + "'");
            continue;
        }
        std::string why;
        if (!apply_setting(settings, key, value, &why)) {
            problems.push_back(at + why + ", ignored");
            continue;
        }
        std::string lower_key(key);
        for (char &c : lower_key) {
            c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        }
        bool repeated = false;
        for (auto &[k, line_no] : seen) {
            if (k == lower_key) {
                problems.push_back(at + k + " again (line " + std::to_string(line_no) +
                                   " set it too): this one counts");
                line_no = number;
                repeated = true;
            }
        }
        if (!repeated) {
            seen.emplace_back(lower_key, number);
        }
    }
    return problems;
}

} // namespace maplethief::frontend
