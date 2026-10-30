// The frontend's settings file (#59, ADR 0043) and the settings registry and presets (#144,
// ADR 0070); see settings.h.
#include "settings.h"

#include <algorithm>
#include <cctype>
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

// `key` equals the lower-case `name`, ASCII case-insensitively.
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

std::string lower(std::string_view s) {
    std::string out(s);
    for (char &c : out) {
        c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return out;
}

// "a, b, c or d".
std::string name_list(std::span<const char *const> names) {
    std::string out;
    for (std::size_t i = 0; i < names.size(); i++) {
        if (i > 0) {
            out += i + 1 == names.size() ? " or " : ", ";
        }
        out += names[i];
    }
    return out;
}

bool parse_right_touch_value(const SettingValue &, std::string_view text, Settings *s) {
    return parse_right_touch(text, &s->right_touch);
}

std::string print_right_touch(const SettingValue &, const Settings &s) {
    const RightTouch &r = s.right_touch;
    return r.on ? std::to_string(r.x) + "," + std::to_string(r.y) : std::string("off");
}

// save_dir (#148): empty, or an absolute path of printable characters (UTF-8 allowed).
bool parse_save_dir(const SettingValue &, std::string_view text, Settings *s) {
    if (text.size() > kMaxSaveDirBytes) {
        return false;
    }
    for (const char c : text) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
            return false;
        }
    }
    const bool absolute =
        text.empty() || text.front() == '/' || text.front() == '\\' ||
        (text.size() >= 3 && std::isalpha(static_cast<unsigned char>(text[0])) != 0 &&
         text[1] == ':' && (text[2] == '\\' || text[2] == '/'));
    if (!absolute) {
        return false;
    }
    s->save_dir = std::string(text);
    return true;
}

std::string print_save_dir(const SettingValue &, const Settings &s) { return s.save_dir; }

// --- The registry -------------------------------------------------------------------------------
//
// One row per setting, in the order a new file lists them. To add a setting: its field in
// Settings (settings.h), with today's behaviour as the default, and a row here. The preset
// columns hold the file's text of the value in Original, Original + controller and Enhanced
// (#89's table, ADR 0070); kNotInPresets for a setting that keeps its own value in every
// preset (volumes, bindings). An Assists row must have the same, neutral value in every column
// (the static_asserts below and check_registry).
constexpr SettingInfo kRegistry[] = {
    // Video.
    {.key = "scale_mode",
     .group = SettingGroup::kVideo,
     .comment = "# integer | fit | smooth | sharp",
     .value = choice<&Settings::scale_mode>(kScaleModeNames),
     .presets = {"integer", "integer", "integer"},
     .flag = "--scale-mode",
     .label = "Scale mode",
     .help = "How the image fills the window: integer is crisp, fit fills more of it"},
    {.key = "border_color",
     .group = SettingGroup::kVideo,
     .comment = "# the bars around the image, #RRGGBB",
     .value = color<&Settings::border_color>(),
     .flag = "--border-color",
     .label = "Border colour",
     .help = "The colour of the bars around the image"},
    {.key = "render_scale",
     .group = SettingGroup::kVideo,
     .comment = "# the renderbuffers' internal scale, 1 to 8, or auto (the largest that fits the "
                "frame budget and the window)",
     .value = uint_range<&Settings::render_scale>(1, MT_GFX_MAX_SCALE, 1, "auto"),
     .presets = {"1", "1", "auto"},
     .flag = "--render-scale",
     .game_only = true,
     .label = "Render scale",
     .help = "Draws the game's GL layer at N times 480 x 320: smoother, slower; auto fits it"},
    {.key = "gfx_backend",
     .group = SettingGroup::kVideo,
     .comment = "# what draws the game's GL frames: software (the reference) | gpu (SDL3 GPU)",
     .value = choice<&Settings::gfx_backend>(kGfxBackendNames),
     // Enhanced: `gpu` once #131 makes it the equal of the reference.
     .presets = {"software", "software", "software"},
     .flag = "--gfx-backend",
     .label = "Renderer",
     .help = "Software is the reference; GPU draws on the graphics card"},
    {.key = "filter",
     .group = SettingGroup::kVideo,
     .comment = "# a shader filter over the scaled image: none | xbr | crt | lcd",
     .value = choice<&Settings::filter>(kShaderFilterNames),
     .presets = {"none", "none", "none"},
     .flag = "--filter",
     .game_only = true,
     .label = "Filter",
     .help = "A shader over the image: xBR smoothing, CRT scanlines or an LCD grid"},
    {.key = "hd_text",
     .group = SettingGroup::kVideo,
     .comment = "# draw the game's text textures at the render scale: on | off",
     .value = on_off<&Settings::hd_text>(),
     .presets = {"off", "off", "on"},
     .flag = "--hd-text",
     .game_only = true,
     .label = "HD text",
     .help = "Draws the game's text at the render scale (needs a render scale of 2 or more)"},
    // Audio.
    {.key = "master_volume",
     .group = SettingGroup::kAudio,
     .comment = "# volume of everything, 0 to 100 (percent)",
     .value = uint_range<&Settings::master_volume>(0, kMaxVolume, 5),
     .flag = "--master-volume",
     .label = "Volume",
     .help = "Everything you hear, in percent"},
    {.key = "music_volume",
     .group = SettingGroup::kAudio,
     .comment = "# volume of the music, 0 to 100",
     .value = uint_range<&Settings::music_volume>(0, kMaxVolume, 5),
     .flag = "--music-volume",
     .label = "Music volume",
     .help = "The music, in percent of the volume"},
    {.key = "effects_volume",
     .group = SettingGroup::kAudio,
     .comment = "# volume of the sound effects, 0 to 100",
     .value = uint_range<&Settings::effects_volume>(0, kMaxVolume, 5),
     .flag = "--effects-volume",
     .label = "Effects volume",
     .help = "The sound effects, in percent of the volume"},
    {.key = "mute_unfocused",
     .group = SettingGroup::kAudio,
     .comment = "# silence while the window is not focused: on | off",
     .value = on_off<&Settings::mute_unfocused>(),
     .presets = {"on", "on", "on"},
     .flag = "--mute-unfocused",
     .label = "Mute in the background",
     .help = "Silence while the window is not focused, as the phone in the background"},
    {.key = "gapless_music",
     .group = SettingGroup::kAudio,
     .comment = "# loop the music without the silence at the start and end of its files: on | off "
                "(off: as the device)",
     .value = on_off<&Settings::gapless_music>(),
     .presets = {"off", "off", "on"},
     .flag = "--gapless-music",
     .label = "Gapless music",
     .help = "Loops the music without the silence at the ends of its files"},
    // Controls.
    {.key = "right_touch",
     .group = SettingGroup::kControls,
     .comment = "# the right mouse button: a touch at screen point X,Y, or off",
     .value = custom_value(&parse_right_touch_value, &print_right_touch,
                           "off or X,Y (screen points, 0-479, 0-319)"),
     .flag = "--right-touch",
     .label = "Right mouse button",
     .help = "A second touch at a fixed screen point X,Y, or off"},
    {.key = "hide_keypad",
     .group = SettingGroup::kControls,
     .comment = "# hide the on-screen key pad: auto (while a gamepad or the keyboard is used) | "
                "always | never",
     .value = choice<&Settings::hide_keypad>(kKeypadModeNames),
     .presets = {"never", "auto", "auto"},
     .flag = "--hide-keypad",
     .label = "Hide the key pad",
     .help = "Auto hides the touch key pad while a gamepad or the keyboard is used"},
    {.key = "rumble",
     .group = SettingGroup::kControls,
     .comment = "# the gamepad rumbles on a vibration, a hit and a level-up: on | off",
     .value = on_off<&Settings::rumble>(),
     // Original and Original + controller: the game's vibration only, once #72's follow-up
     // splits it from the hit and the level-up.
     .presets = {"on", "on", "on"},
     .flag = "--rumble",
     .label = "Rumble",
     .help = "The gamepad rumbles on the game's vibration, a hit and a level-up"},
    {.key = "rumble_strength",
     .group = SettingGroup::kControls,
     .comment = "# how strongly, 0 to 100 (percent)",
     .value = uint_range<&Settings::rumble_strength>(0, kMaxRumbleStrength, 10),
     .flag = "--rumble-strength",
     .label = "Rumble strength",
     .help = "How strongly the gamepad rumbles, in percent"},
    // Video, after the input keys (the order of a new file since #137).
    {.key = "perf_overlay",
     .group = SettingGroup::kVideo,
     .comment = "# the frame-time overlay (F3 shows or hides it while the game runs): on | off",
     .value = on_off<&Settings::perf_overlay>(),
     .presets = {"off", "off", "off"},
     .flag = "--perf-overlay",
     .game_only = true,
     .label = "Frame-time overlay",
     .help = "Shows the frame time and the frame rate over the game (F3 toggles it)"},
    // Assists (#156, ADR 0072).
    {.key = "cash_shop",
     .group = SettingGroup::kAssists,
     .comment =
         "# Assist: the cash shop and the paid revives: off (as the original offline) | meso "
         "(sold\n# for the hero's meso, dearer with the level, the falls and the purchases)",
     .value = choice<&Settings::cash_shop>(kCashShopNames),
     .presets = {"off", "off", "off"},
     .flag = "--cash-shop",
     .game_only = true,
     .label = "Cash shop",
     .help = "Sells the cash items and the revives for meso, at rising prices"},
    // Saves (#148, ADR 0078).
    {.key = "save_dir",
     .group = SettingGroup::kSaves,
     .comment = "# the folder for the game's save files, e.g. a cloud-synced one (an absolute "
                "path; empty: Documents in the data folder)",
     .value = custom_value(&parse_save_dir, &print_save_dir,
                           "an absolute folder path, or empty for the default"),
     .flag = "--save-dir",
     .label = "Save folder",
     .help = "Where the saves are: empty for the default, or a folder such as a cloud-synced one"},
    {.key = "save_backups",
     .group = SettingGroup::kSaves,
     .comment = "# backups kept of each character, taken after each save, 0 to 100 (0: none)",
     .value = uint_range<&Settings::save_backups>(0, kMaxSaveBackups, 5),
     .flag = "--save-backups",
     .label = "Backups per character",
     .help = "How many earlier saves of each character are kept in the backups folder"},
};

// The table's rules that need no Settings value, at compile time (check_registry has the rest).
constexpr bool same_text(const char *a, const char *b) {
    if (a == nullptr || b == nullptr) {
        return a == b;
    }
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

constexpr bool registry_ok(std::span<const SettingInfo> rows) {
    for (std::size_t i = 0; i < rows.size(); i++) {
        const SettingInfo &r = rows[i];
        if (r.key == nullptr || r.comment == nullptr || r.value.parse == nullptr ||
            r.value.print == nullptr || same_text(r.key, kPresetKey)) {
            return false;
        }
        // A column for each preset, or none.
        const PresetColumns &p = r.presets;
        if ((p.original == nullptr) != (p.original_controller == nullptr) ||
            (p.original == nullptr) != (p.enhanced == nullptr)) {
            return false;
        }
        // Assists: in the presets, the same value in every column (never on through a preset).
        if (r.group == SettingGroup::kAssists &&
            (!p.in_presets() || !same_text(p.original, p.original_controller) ||
             !same_text(p.original, p.enhanced))) {
            return false;
        }
        for (std::size_t j = 0; j < i; j++) {
            if (same_text(rows[j].key, r.key) ||
                (r.flag != nullptr && same_text(rows[j].flag, r.flag))) {
                return false;
            }
        }
    }
    return true;
}
static_assert(registry_ok(kRegistry), "settings registry: a missing field, a duplicate key or "
                                      "flag, a partial preset column, or an Assists row whose "
                                      "columns differ (ADR 0070)");

std::string fail_message(const SettingInfo &info, std::string_view value) {
    return std::string(info.key) + ": '" + printable(value) + "' is not " + setting_expects(info);
}

std::string preset_fail_message(std::string_view value) {
    return std::string(kPresetKey) + ": '" + printable(value) + "' is not " +
           name_list(kPresetNames);
}

// Sets a key without moving the preset to custom (parse_settings' lines under a preset line).
bool set_raw(Settings *settings, std::string_view key, std::string_view value, std::string *why) {
    auto fail = [&](const std::string &what) {
        if (why != nullptr) {
            *why = what;
        }
        return false;
    };
    if (key_is(key, kPresetKey)) {
        Preset p{};
        if (!parse_preset(value, &p)) {
            return fail(preset_fail_message(value));
        }
        apply_preset(settings, p);
        return true;
    }
    const SettingInfo *info = find_setting(key);
    if (info == nullptr) {
        return fail("unknown key '" + printable(key) + "'");
    }
    if (!info->value.parse(info->value, value, settings)) {
        return fail(fail_message(*info, value));
    }
    return true;
}

} // namespace

// --- Names
// ----------------------------------------------------------------------------------------

namespace settings_detail {

int find_name(std::span<const char *const> names, std::string_view text) {
    for (std::size_t i = 0; i < names.size(); i++) {
        if (key_is(text, names[i])) {
            return static_cast<int>(i);
        }
    }
    return -1;
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

std::string color_text(const Color &c) {
    char text[8];
    std::snprintf(text, sizeof text, "#%02X%02X%02X", c.r, c.g, c.b);
    return text;
}

} // namespace settings_detail

namespace {

template <typename T, std::size_t N> const char *enum_name(T value, const char *const (&names)[N]) {
    const auto i = static_cast<std::size_t>(value);
    return i < N ? names[i] : "?";
}

template <typename T, std::size_t N>
bool parse_enum(std::string_view text, const char *const (&names)[N], T *out) {
    const int i = settings_detail::find_name(names, text);
    if (i < 0) {
        return false;
    }
    *out = static_cast<T>(i);
    return true;
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
    if (!settings_detail::parse_small_uint(trim(text.substr(0, comma)), &x) ||
        !settings_detail::parse_small_uint(trim(text.substr(comma + 1)), &y) || x >= kScreenWidth ||
        y >= kScreenHeight) {
        return false;
    }
    *out = RightTouch{true, x, y};
    return true;
}

const char *keypad_mode_name(KeypadMode mode) { return enum_name(mode, kKeypadModeNames); }
bool parse_keypad_mode(std::string_view text, KeypadMode *out) {
    return parse_enum(text, kKeypadModeNames, out);
}
const char *gfx_backend_name(GfxBackend backend) { return enum_name(backend, kGfxBackendNames); }
bool parse_gfx_backend(std::string_view text, GfxBackend *out) {
    return parse_enum(text, kGfxBackendNames, out);
}
const char *shader_filter_name(ShaderFilter filter) {
    return enum_name(filter, kShaderFilterNames);
}
bool parse_shader_filter(std::string_view text, ShaderFilter *out) {
    return parse_enum(text, kShaderFilterNames, out);
}
const char *preset_name(Preset preset) { return enum_name(preset, kPresetNames); }
bool parse_preset(std::string_view text, Preset *out) {
    return parse_enum(text, kPresetNames, out);
}

const char *preset_title(Preset preset) {
    switch (preset) {
    case Preset::kOriginal:
        return "Original";
    case Preset::kOriginalController:
        return "Original + controller";
    case Preset::kEnhanced:
        return "Enhanced";
    case Preset::kCustom:
        break;
    }
    return "Custom";
}

const char *preset_summary(Preset preset) {
    switch (preset) {
    case Preset::kOriginal:
        return "The iPhone game as it was: touch key pad, its look and pacing";
    case Preset::kOriginalController:
        return "The original game, with comfort for a gamepad or the keyboard";
    case Preset::kEnhanced:
        return "The best version: every enhancement on, the same gameplay";
    case Preset::kCustom:
        break;
    }
    return "Keep your own settings from settings.ini as they are";
}

const char *setting_group_name(SettingGroup group) {
    switch (group) {
    case SettingGroup::kVideo:
        return "Video";
    case SettingGroup::kAudio:
        return "Audio";
    case SettingGroup::kControls:
        return "Controls";
    case SettingGroup::kGameplay:
        return "Gameplay";
    case SettingGroup::kAccessibility:
        return "Accessibility";
    case SettingGroup::kAssists:
        return "Assists";
    case SettingGroup::kSaves:
        return "Saves";
    case SettingGroup::kOther:
        break;
    }
    return "Other";
}

// --- The registry
// ---------------------------------------------------------------------------------

std::span<const SettingInfo> settings_registry() { return kRegistry; }

const SettingInfo *find_setting(std::string_view key) {
    for (const SettingInfo &info : kRegistry) {
        if (key_is(key, info.key)) {
            return &info;
        }
    }
    return nullptr;
}

const SettingInfo *find_setting_flag(std::string_view flag) {
    for (const SettingInfo &info : kRegistry) {
        if (info.flag != nullptr && flag == info.flag) {
            return &info;
        }
    }
    return nullptr;
}

std::string setting_text(const Settings &settings, const SettingInfo &info) {
    return info.value.print(info.value, settings);
}

std::string setting_expects(const SettingInfo &info) {
    const SettingValue &v = info.value;
    switch (v.kind) {
    case ValueKind::kChoice:
        return name_list(v.names);
    case ValueKind::kOnOff:
        return "on or off";
    case ValueKind::kUint:
        return std::string(v.zero != nullptr ? v.zero : "") + (v.zero != nullptr ? " or " : "") +
               "an integer from " + std::to_string(v.min) + " to " + std::to_string(v.max);
    case ValueKind::kColor:
        return "#RRGGBB";
    case ValueKind::kCustom:
        break;
    }
    return v.expect != nullptr ? v.expect : "a value";
}

std::vector<std::string> setting_choices(const SettingInfo &info) {
    const SettingValue &v = info.value;
    std::vector<std::string> out;
    switch (v.kind) {
    case ValueKind::kChoice:
        out.assign(v.names.begin(), v.names.end());
        break;
    case ValueKind::kOnOff:
        out = {"off", "on"};
        break;
    case ValueKind::kUint:
        if (v.zero != nullptr) {
            out.emplace_back(v.zero);
        }
        for (std::uint32_t n = v.min; n <= v.max; n += v.step == 0 ? 1 : v.step) {
            out.push_back(std::to_string(n));
        }
        break;
    case ValueKind::kColor:
    case ValueKind::kCustom:
        break;
    }
    return out;
}

bool step_setting(Settings *settings, const SettingInfo &info, int delta) {
    const SettingValue &v = info.value;
    const std::string now = setting_text(*settings, info);
    if (v.kind == ValueKind::kUint && v.zero != nullptr) {
        // The name for 0 comes before the range: step through the choices, clamped.
        const std::vector<std::string> choices = setting_choices(info);
        long i = 0;
        for (std::size_t k = 0; k < choices.size(); k++) {
            i = choices[k] == now ? static_cast<long>(k) : i;
        }
        i = std::clamp<long>(i + delta, 0, static_cast<long>(choices.size()) - 1);
        return apply_setting(settings, info.key, choices[static_cast<std::size_t>(i)], nullptr);
    }
    if (v.kind == ValueKind::kUint) {
        std::uint32_t n = 0;
        settings_detail::parse_small_uint(now, &n);
        const long long step = static_cast<long long>(v.step == 0 ? 1 : v.step) * delta;
        long long next = static_cast<long long>(n) + step;
        next = next < v.min ? v.min : next > v.max ? v.max : next;
        return apply_setting(settings, info.key, std::to_string(next), nullptr);
    }
    const std::vector<std::string> choices = setting_choices(info);
    if (choices.empty()) {
        return false;
    }
    const long n = static_cast<long>(choices.size());
    long i = 0;
    for (long k = 0; k < n; k++) {
        if (choices[static_cast<std::size_t>(k)] == now) {
            i = k;
        }
    }
    i = ((i + delta) % n + n) % n;
    return apply_setting(settings, info.key, choices[static_cast<std::size_t>(i)], nullptr);
}

std::vector<std::string> check_registry(std::span<const SettingInfo> rows) {
    std::vector<std::string> problems;
    const Settings defaults;
    for (std::size_t i = 0; i < rows.size(); i++) {
        const SettingInfo &r = rows[i];
        const std::string key = r.key != nullptr ? r.key : "";
        const std::string at = "row " + std::to_string(i) + " (" + key + "): ";
        bool key_ok = !key.empty() && key != kPresetKey;
        for (const char c : key) {
            key_ok = key_ok && is_key_char(c) && !(c >= 'A' && c <= 'Z');
        }
        if (!key_ok) {
            problems.push_back(at + "not a lower-case key");
        }
        if (r.value.parse == nullptr || r.value.print == nullptr) {
            problems.push_back(at + "no parser or printer");
            continue;
        }
        if (r.value.kind == ValueKind::kChoice && r.value.names.empty()) {
            problems.push_back(at + "a choice without names");
        }
        if (r.value.kind == ValueKind::kUint && (r.value.min > r.value.max || r.value.max > 999)) {
            problems.push_back(at + "a range that is empty or over three digits");
        }
        // Comment lines: each starts with '#'.
        const std::string_view comment = r.comment != nullptr ? r.comment : "";
        std::size_t start = 0;
        while (start <= comment.size()) {
            const std::size_t nl = comment.find('\n', start);
            const std::string_view line =
                comment.substr(start, nl == std::string_view::npos ? nl : nl - start);
            if (line.empty() || line.front() != '#') {
                problems.push_back(at + "a comment line that does not start with '#'");
            }
            if (nl == std::string_view::npos) {
                break;
            }
            start = nl + 1;
        }
        if (r.flag != nullptr) {
            std::string want = "--" + key;
            for (char &c : want) {
                c = c == '_' ? '-' : c;
            }
            if (want != r.flag) {
                problems.push_back(at + "the flag is not " + want);
            }
        }
        if ((r.label == nullptr || r.label[0] == '\0') ||
            (r.help == nullptr || r.help[0] == '\0')) {
            problems.push_back(at + "no menu label or help");
        }
        for (std::size_t j = 0; j < i; j++) {
            if (rows[j].key != nullptr && key == rows[j].key) {
                problems.push_back(at + "the key again (row " + std::to_string(j) + ")");
            }
            if (r.flag != nullptr && rows[j].flag != nullptr &&
                std::string_view(r.flag) == rows[j].flag) {
                problems.push_back(at + "the flag again (row " + std::to_string(j) + ")");
            }
        }
        // The preset columns: all or none, each a value the row reads back as the same text.
        const PresetColumns &p = r.presets;
        const int given =
            (p.original != nullptr) + (p.original_controller != nullptr) + (p.enhanced != nullptr);
        if (given != 0 && given != 3) {
            problems.push_back(at + "a column for some presets only");
            continue;
        }
        for (const Preset preset : kPresets) {
            const char *column = p[preset];
            if (column == nullptr) {
                continue;
            }
            Settings s;
            if (!r.value.parse(r.value, column, &s) || setting_text(s, r) != column) {
                problems.push_back(at + "the " + preset_name(preset) + " column '" + column +
                                   "' is not a value as the file writes it");
            }
        }
        // Assists: never on through a preset. Every column is the default (neutral) value.
        if (r.group == SettingGroup::kAssists) {
            const std::string neutral = setting_text(defaults, r);
            if (!p.in_presets()) {
                problems.push_back(at + "an assist outside the presets (a preset must turn it "
                                        "off)");
            }
            for (const Preset preset : kPresets) {
                const char *column = p[preset];
                if (column != nullptr && neutral != column) {
                    problems.push_back(at + "an assist that is '" + column + "' in " +
                                       preset_name(preset) + ", not its neutral '" + neutral + "'");
                }
            }
        }
    }
    return problems;
}

// --- Presets
// --------------------------------------------------------------------------------------

void apply_preset(Settings *settings, Preset preset) {
    for (const SettingInfo &info : kRegistry) {
        const char *column = info.presets[preset];
        if (column != nullptr) {
            info.value.parse(info.value, column, settings);
        }
    }
    settings->preset = preset;
}

std::vector<const SettingInfo *> preset_differences(const Settings &settings, Preset preset) {
    std::vector<const SettingInfo *> out;
    for (const SettingInfo &info : kRegistry) {
        const char *column = info.presets[preset];
        if (column != nullptr && setting_text(settings, info) != column) {
            out.push_back(&info);
        }
    }
    return out;
}

std::optional<Preset> matching_preset(const Settings &settings) {
    for (const Preset p : {Preset::kOriginal, Preset::kOriginalController, Preset::kEnhanced}) {
        if (preset_differences(settings, p).empty()) {
            return p;
        }
    }
    return std::nullopt;
}

// --- The file
// -------------------------------------------------------------------------------------

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
    Settings changed = *settings;
    if (!set_raw(&changed, key, value, why)) {
        return false;
    }
    // A key in the presets that no longer has the preset's value: the preset becomes custom.
    const SettingInfo *info = find_setting(key);
    if (info != nullptr && changed.preset && *changed.preset != Preset::kCustom) {
        const char *column = info->presets[*changed.preset];
        if (column != nullptr && setting_text(changed, *info) != column) {
            changed.preset = Preset::kCustom;
        }
    }
    *settings = changed;
    return true;
}

namespace {

// Calls `line(number, text)` for each line of `text` (a byte order mark and a CR before the LF
// dropped).
template <typename F> void for_each_line(std::string_view text, F line) {
    if (text.substr(0, 3) == "\xEF\xBB\xBF") {
        text.remove_prefix(3);
    }
    std::size_t number = 0;
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        std::string_view l = text.substr(0, nl);
        text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
        number++;
        if (!l.empty() && l.back() == '\r') {
            l.remove_suffix(1);
        }
        line(number, l);
    }
}

// A `key = value` line: its key and value, trimmed; false for a comment, a blank line or
// anything else.
bool key_value(std::string_view line, std::string_view *key, std::string_view *value) {
    line = trim(line);
    if (line.empty() || line.front() == '#' || line.front() == '[') {
        return false;
    }
    const std::size_t eq = line.find('=');
    if (eq == std::string_view::npos) {
        return false;
    }
    *key = trim(line.substr(0, eq));
    *value = trim(line.substr(eq + 1));
    return true;
}

} // namespace

std::vector<std::string> parse_settings(std::string_view text, Settings *settings) {
    std::vector<std::string> problems;
    if (text.size() > kMaxSettingsBytes) {
        problems.push_back("the file has " + std::to_string(text.size()) + " bytes, more than " +
                           std::to_string(kMaxSettingsBytes) + ": ignored");
        return problems;
    }
    // The file's preset: its last good `preset` line.
    std::optional<Preset> file_preset;
    for_each_line(text, [&](std::size_t, std::string_view line) {
        std::string_view key, value;
        Preset p{};
        if (key_value(line, &key, &value) && key_is(key, kPresetKey) && parse_preset(value, &p)) {
            file_preset = p;
        }
    });
    // The keys seen so far and the line that set them, to report a repeated key.
    std::vector<std::pair<std::string, std::size_t>> seen;
    for_each_line(text, [&](std::size_t number, std::string_view line) {
        const std::string at = "line " + std::to_string(number) + ": ";
        line = trim(line);
        if (line.empty() || line.front() == '#') {
            return;
        }
        if (line.front() == '[') {
            problems.push_back(at + "sections are not supported: '" + printable(line) +
                               "' ignored");
            return;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            problems.push_back(at + "not key = value: '" + printable(line) + "'");
            return;
        }
        const std::string_view key = trim(line.substr(0, eq));
        const std::string_view value = trim(line.substr(eq + 1));
        bool key_ok = !key.empty();
        for (const char c : key) {
            key_ok = key_ok && is_key_char(c);
        }
        if (!key_ok) {
            problems.push_back(at + "not a key: '" + printable(key) + "'");
            return;
        }
        std::string why;
        if (!file_preset) {
            if (!apply_setting(settings, key, value, &why)) {
                problems.push_back(at + why + ", ignored");
                return;
            }
        } else {
            // Under a preset line: the preset is applied after the lines, and a key it
            // controls keeps the preset's value.
            Settings line_value = *settings;
            if (key_is(key, kPresetKey)) {
                Preset p{};
                if (!parse_preset(value, &p)) {
                    problems.push_back(at + preset_fail_message(value) + ", ignored");
                    return;
                }
            } else if (!set_raw(&line_value, key, value, &why)) {
                problems.push_back(at + why + ", ignored");
                return;
            } else if (const SettingInfo *info = find_setting(key);
                       *file_preset != Preset::kCustom && info->presets.in_presets() &&
                       setting_text(line_value, *info) != info->presets[*file_preset]) {
                problems.push_back(at + info->key + " = " + printable(value) +
                                   ": preset = " + preset_name(*file_preset) + " sets " +
                                   info->presets[*file_preset] +
                                   ", ignored (preset = custom keeps the file's values)");
                return;
            }
            *settings = line_value;
        }
        const std::string lower_key = lower(key);
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
    });
    if (file_preset) {
        apply_preset(settings, *file_preset);
    }
    return problems;
}

namespace {

// A key a file lists: the preset or a row.
struct FileKey {
    const char *key;
    const char *comment;
    std::string value;
};

constexpr const char *kPresetComment =
    "# the preset: original | original_controller | enhanced | custom. A preset sets the keys "
    "it\n# controls (a line below that disagrees is ignored); custom keeps every value below";

std::vector<FileKey> file_keys(const Settings &settings) {
    std::vector<FileKey> keys;
    if (settings.preset) {
        keys.push_back({kPresetKey, kPresetComment, preset_name(*settings.preset)});
    }
    for (const SettingInfo &info : kRegistry) {
        keys.push_back({info.key, info.comment, setting_text(settings, info)});
    }
    return keys;
}

// The comment's lines, each ended by `newline`.
void put_comment(std::string *out, std::string_view comment, std::string_view newline) {
    std::size_t start = 0;
    while (true) {
        const std::size_t nl = comment.find('\n', start);
        *out += comment.substr(start, nl == std::string_view::npos ? nl : nl - start);
        *out += newline;
        if (nl == std::string_view::npos) {
            return;
        }
        start = nl + 1;
    }
}

} // namespace

std::string serialise_settings(const Settings &settings, std::string_view old_text) {
    const std::vector<FileKey> keys = file_keys(settings);
    const std::size_t key_count = keys.size();
    if (old_text.empty() || old_text.size() > kMaxSettingsBytes) {
        std::string out = "# MapleThief settings (settings.ini). One key = value per line; lines "
                          "starting with # are\n# comments. The command line overrides these.\n";
        for (const FileKey &k : keys) {
            put_comment(&out, k.comment, "\n");
            out += k.key;
            out += k.value.empty() ? " =" : " = "; // save_dir's default: no trailing blank
            out += k.value;
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
                if (key_is(key, keys[i].key)) {
                    std::size_t value_at = eq + 1;
                    while (value_at < line.size() && is_blank(line[value_at])) {
                        value_at++;
                    }
                    out += line.substr(0, value_at);
                    out += keys[i].value;
                    present[i] = true;
                    rewritten = true;
                }
            }
            // A preset line while no preset is set: it would set one, so it becomes a comment.
            if (!rewritten && !settings.preset && key_is(key, kPresetKey)) {
                out += "# ";
                out += line;
                rewritten = true;
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
        put_comment(&out, keys[i].comment, newline);
        out += keys[i].key;
        out += keys[i].value.empty() ? " =" : " = ";
        out += keys[i].value;
        out += newline;
    }
    // A file grown past what parse_settings reads (from just under the limit): a new one.
    return out.size() > kMaxSettingsBytes ? serialise_settings(settings) : out;
}

} // namespace maplethief::frontend
