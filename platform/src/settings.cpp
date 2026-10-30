// The frontend's settings file (#59, ADR 0043); see settings.h.
#include "settings.h"

#include <cstdio>
#include <iterator>

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

// on, off, true, false, yes, no, 1, 0 in any case.
bool parse_on_off(std::string_view s, bool *out) {
    for (const char *on : {"on", "true", "yes", "1"}) {
        if (key_is(s, on)) {
            *out = true;
            return true;
        }
    }
    for (const char *off : {"off", "false", "no", "0"}) {
        if (key_is(s, off)) {
            *out = false;
            return true;
        }
    }
    return false;
}

std::string color_text(const Color &c) {
    char text[8];
    std::snprintf(text, sizeof text, "#%02X%02X%02X", c.r, c.g, c.b);
    return text;
}

std::string right_touch_text(const RightTouch &r) {
    return r.on ? std::to_string(r.x) + "," + std::to_string(r.y) : std::string("off");
}

} // namespace

bool parse_right_touch(std::string_view text, RightTouch *out) {
    text = trim(text);
    if (key_is(text, "off")) {
        *out = RightTouch{};
        return true;
    }
    const std::size_t comma = text.find(',');
    if (comma == std::string_view::npos) {
        return false;
    }
    std::uint32_t x = 0, y = 0;
    if (!parse_small_uint(trim(text.substr(0, comma)), &x) ||
        !parse_small_uint(trim(text.substr(comma + 1)), &y) || x >= kScreenWidth ||
        y >= kScreenHeight) {
        return false;
    }
    *out = RightTouch{true, x, y};
    return true;
}

const char *keypad_mode_name(KeypadMode mode) {
    switch (mode) {
    case KeypadMode::kAuto:
        return "auto";
    case KeypadMode::kAlways:
        return "always";
    case KeypadMode::kNever:
        return "never";
    }
    return "?";
}

bool parse_keypad_mode(std::string_view text, KeypadMode *out) {
    for (const KeypadMode m : {KeypadMode::kAuto, KeypadMode::kAlways, KeypadMode::kNever}) {
        const std::string_view name = keypad_mode_name(m);
        if (text.size() != name.size()) {
            continue;
        }
        bool same = true;
        for (std::size_t i = 0; i < text.size() && same; i++) {
            char c = text[i];
            c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
            same = c == name[i];
        }
        if (same) {
            *out = m;
            return true;
        }
    }
    return false;
}

const char *gfx_backend_name(GfxBackend backend) {
    return backend == GfxBackend::kGpu ? "gpu" : "software";
}

bool parse_gfx_backend(std::string_view text, GfxBackend *out) {
    for (const GfxBackend b : {GfxBackend::kSoftware, GfxBackend::kGpu}) {
        const std::string_view name = gfx_backend_name(b);
        bool same = text.size() == name.size();
        for (std::size_t i = 0; i < text.size() && same; i++) {
            char c = text[i];
            c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
            same = c == name[i];
        }
        if (same) {
            *out = b;
            return true;
        }
    }
    return false;
}

const char *shader_filter_name(ShaderFilter filter) {
    switch (filter) {
    case ShaderFilter::kXbr:
        return "xbr";
    case ShaderFilter::kCrt:
        return "crt";
    case ShaderFilter::kLcd:
        return "lcd";
    case ShaderFilter::kNone:
        break;
    }
    return "none";
}

bool parse_shader_filter(std::string_view text, ShaderFilter *out) {
    for (const ShaderFilter f :
         {ShaderFilter::kNone, ShaderFilter::kXbr, ShaderFilter::kCrt, ShaderFilter::kLcd}) {
        const std::string_view name = shader_filter_name(f);
        bool same = text.size() == name.size();
        for (std::size_t i = 0; i < text.size() && same; i++) {
            char c = text[i];
            c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
            same = c == name[i];
        }
        if (same) {
            *out = f;
            return true;
        }
    }
    return false;
}

namespace {

// The keys in the order a new file lists them, with their comment lines and their values.
struct KeyInfo {
    const char *key;
    const char *comment;
    std::string (*value)(const Settings &);
};
const KeyInfo kKeys[] = {
    {"scale_mode", "# integer | fit | smooth | sharp",
     [](const Settings &s) { return std::string(scale_mode_name(s.scale_mode)); }},
    {"border_color", "# the bars around the image, #RRGGBB",
     [](const Settings &s) { return color_text(s.border_color); }},
    {"render_scale", "# the renderbuffers' internal scale, 1 to 8",
     [](const Settings &s) { return std::to_string(s.render_scale); }},
    {"gfx_backend", "# what draws the game's GL frames: software (the reference) | gpu (SDL3 GPU)",
     [](const Settings &s) { return std::string(gfx_backend_name(s.gfx_backend)); }},
    {"filter", "# a shader filter over the scaled image: none | xbr | crt | lcd",
     [](const Settings &s) { return std::string(shader_filter_name(s.filter)); }},
    {"hd_text", "# draw the game's text textures at the render scale: on | off",
     [](const Settings &s) { return std::string(s.hd_text ? "on" : "off"); }},
    {"master_volume", "# volume of everything, 0 to 100 (percent)",
     [](const Settings &s) { return std::to_string(s.master_volume); }},
    {"music_volume", "# volume of the music, 0 to 100",
     [](const Settings &s) { return std::to_string(s.music_volume); }},
    {"effects_volume", "# volume of the sound effects, 0 to 100",
     [](const Settings &s) { return std::to_string(s.effects_volume); }},
    {"mute_unfocused", "# silence while the window is not focused: on | off",
     [](const Settings &s) { return std::string(s.mute_unfocused ? "on" : "off"); }},
    {"gapless_music",
     "# loop the music without the silence at the start and end of its files: on | off (off: "
     "as the device)",
     [](const Settings &s) { return std::string(s.gapless_music ? "on" : "off"); }},
    {"right_touch", "# the right mouse button: a touch at screen point X,Y, or off",
     [](const Settings &s) { return right_touch_text(s.right_touch); }},
    {"hide_keypad",
     "# hide the on-screen key pad: auto (while a gamepad or the keyboard is used) | always | "
     "never",
     [](const Settings &s) { return std::string(keypad_mode_name(s.hide_keypad)); }},
    {"rumble", "# the gamepad rumbles on a vibration, a hit and a level-up: on | off",
     [](const Settings &s) { return std::string(s.rumble ? "on" : "off"); }},
    {"rumble_strength", "# how strongly, 0 to 100 (percent)",
     [](const Settings &s) { return std::to_string(s.rumble_strength); }},
};

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
    if (key_is(key, "gfx_backend")) {
        GfxBackend backend{};
        if (!parse_gfx_backend(value, &backend)) {
            return fail("gfx_backend: '" + printable(value) + "' is not software or gpu");
        }
        settings->gfx_backend = backend;
        return true;
    }
    if (key_is(key, "filter")) {
        ShaderFilter filter{};
        if (!parse_shader_filter(value, &filter)) {
            return fail("filter: '" + printable(value) + "' is not none, xbr, crt or lcd");
        }
        settings->filter = filter;
        return true;
    }
    if (key_is(key, "hd_text")) {
        bool on = false;
        if (!parse_on_off(value, &on)) {
            return fail("hd_text: '" + printable(value) + "' is not on or off");
        }
        settings->hd_text = on;
        return true;
    }
    struct Volume {
        const char *key;
        std::uint32_t Settings::*field;
    };
    for (const Volume &v : {Volume{"master_volume", &Settings::master_volume},
                            Volume{"music_volume", &Settings::music_volume},
                            Volume{"effects_volume", &Settings::effects_volume}}) {
        if (key_is(key, v.key)) {
            std::uint32_t volume = 0;
            if (!parse_small_uint(value, &volume) || volume > kMaxVolume) {
                return fail(std::string(v.key) + ": '" + printable(value) +
                            "' is not an integer from 0 to " + std::to_string(kMaxVolume));
            }
            settings->*v.field = volume;
            return true;
        }
    }
    struct Switch {
        const char *key;
        bool Settings::*field;
    };
    for (const Switch &w : {Switch{"mute_unfocused", &Settings::mute_unfocused},
                            Switch{"gapless_music", &Settings::gapless_music}}) {
        if (key_is(key, w.key)) {
            bool on = false;
            if (!parse_on_off(value, &on)) {
                return fail(std::string(w.key) + ": '" + printable(value) + "' is not on or off");
            }
            settings->*w.field = on;
            return true;
        }
    }
    if (key_is(key, "right_touch")) {
        RightTouch r;
        if (!parse_right_touch(value, &r)) {
            return fail("right_touch: '" + printable(value) +
                        "' is not off or X,Y (screen points, 0-479, 0-319)");
        }
        settings->right_touch = r;
        return true;
    }
    if (key_is(key, "hide_keypad")) {
        KeypadMode mode{};
        if (!parse_keypad_mode(value, &mode)) {
            return fail("hide_keypad: '" + printable(value) + "' is not auto, always or never");
        }
        settings->hide_keypad = mode;
        return true;
    }
    if (key_is(key, "rumble")) {
        bool on = false;
        if (!parse_on_off(value, &on)) {
            return fail("rumble: '" + printable(value) + "' is not on or off");
        }
        settings->rumble = on;
        return true;
    }
    if (key_is(key, "rumble_strength")) {
        std::uint32_t strength = 0;
        if (!parse_small_uint(value, &strength) || strength > kMaxRumbleStrength) {
            return fail("rumble_strength: '" + printable(value) + "' is not an integer from 0 to " +
                        std::to_string(kMaxRumbleStrength));
        }
        settings->rumble_strength = strength;
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

std::string serialise_settings(const Settings &settings, std::string_view old_text) {
    const std::size_t key_count = std::size(kKeys);
    if (old_text.empty() || old_text.size() > kMaxSettingsBytes) {
        std::string out = "# MapleThief settings (settings.ini). One key = value per line; lines "
                          "starting with # are\n# comments. The command line overrides these.\n";
        for (const KeyInfo &k : kKeys) {
            out += k.comment;
            out += '\n';
            out += k.key;
            out += " = ";
            out += k.value(settings);
            out += '\n';
        }
        return out;
    }
    std::string out;
    out.reserve(old_text.size() + 256);
    std::vector<bool> present(key_count, false);
    std::string_view text = old_text;
    std::string_view newline = "\n";
    bool first_line = true;
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        const bool has_nl = nl != std::string_view::npos;
        text.remove_prefix(has_nl ? nl + 1 : text.size());
        if (first_line && line.substr(0, 3) == "\xEF\xBB\xBF") {
            out += line.substr(0, 3);
            line.remove_prefix(3);
        }
        std::string_view cr;
        if (!line.empty() && line.back() == '\r') {
            cr = line.substr(line.size() - 1);
            line.remove_suffix(1);
        }
        if (first_line && has_nl) {
            newline = cr.empty() ? "\n" : "\r\n";
        }
        first_line = false;
        // A known key's line: everything up to its old value is kept, and the new value
        // replaces the rest (the value and any blanks after it).
        const std::string_view trimmed = trim(line);
        const std::size_t eq = line.find('=');
        bool rewritten = false;
        if (eq != std::string_view::npos && !trimmed.empty() && trimmed.front() != '#' &&
            trimmed.front() != '[') {
            const std::string_view key = trim(line.substr(0, eq));
            for (std::size_t i = 0; i < key_count && !rewritten; i++) {
                if (key_is(key, kKeys[i].key)) {
                    std::size_t value_at = eq + 1;
                    while (value_at < line.size() && is_blank(line[value_at])) {
                        value_at++;
                    }
                    out += line.substr(0, value_at);
                    out += kKeys[i].value(settings);
                    present[i] = true;
                    rewritten = true;
                }
            }
        }
        if (!rewritten) {
            out += line;
        }
        out += cr;
        if (has_nl) {
            out += '\n';
        }
    }
    bool ends_open = !out.empty() && out.back() != '\n';
    for (std::size_t i = 0; i < key_count; i++) {
        if (present[i]) {
            continue;
        }
        if (ends_open) {
            out += newline;
            ends_open = false;
        }
        out += kKeys[i].comment;
        out += newline;
        out += kKeys[i].key;
        out += " = ";
        out += kKeys[i].value(settings);
        out += newline;
    }
    // A file grown past what parse_settings reads (from just under the limit): a new one.
    return out.size() > kMaxSettingsBytes ? serialise_settings(settings) : out;
}

} // namespace maplethief::frontend
