// The frontend's settings file (#59, ADR 0043; the audio keys #64; hide_keypad #71; rumble and
// rumble_strength #72; gfx_backend #120; filter and hd_text #88, ADR 0066; gapless_music #125)
// and the settings registry and presets (#144, ADR 0070): settings.ini in the data directory
// (MAPLETHIEF_DATA_DIR, else SDL_GetPrefPath("", "MapleThief")). Pure parsing and writing, no
// SDL; game.cpp reads and writes the file, main.cpp applies the command line over it.
//
// Every setting is one row of the registry (settings_registry(), the table in settings.cpp):
// its key, group, comment, value type bound to its Settings field, command-line flag, menu
// label and help, and its value in each preset. Parsing, writing, the command line, the presets
// and #89's settings menu all go through the rows. To add a setting: add its field to Settings
// and its row to the table (ADR 0070 has the recipe and the planned keys' preset columns).
//
// A new file, the defaults and a preset chosen (the order of the table):
//
//   # MapleThief settings (settings.ini). ...
//   # the preset: original | original_controller | enhanced | custom. ...
//   preset = original
//   # integer | fit | smooth | sharp
//   scale_mode = integer
//   # the bars around the image, #RRGGBB
//   border_color = #000000
//   # the renderbuffers' internal scale, 1 to 8
//   render_scale = 1
//   # what draws the game's GL frames: software (the reference) | gpu (SDL3 GPU)
//   gfx_backend = software
//   # a shader filter over the scaled image: none | xbr | crt | lcd
//   filter = none
//   # draw the game's text textures at the render scale: on | off
//   hd_text = off
//   # volume of everything, 0 to 100 (percent)
//   master_volume = 100
//   # volume of the music, 0 to 100
//   music_volume = 100
//   # volume of the sound effects, 0 to 100
//   effects_volume = 100
//   # silence while the window is not focused: on | off
//   mute_unfocused = on
//   # loop the music without the silence at the start and end of its files: on | off (off: ...)
//   gapless_music = off
//   # the right mouse button: a touch at screen point X,Y, or off
//   right_touch = off
//   # hide the on-screen key pad: auto (while a gamepad or the keyboard is used) | always | never
//   hide_keypad = never
//   # the gamepad rumbles on a vibration, a hit and a level-up: on | off
//   rumble = on
//   # how strongly, 0 to 100 (percent)
//   rumble_strength = 100
//
// The format, in full:
//   - UTF-8 (a leading byte order mark is skipped); lines end in LF or CR LF.
//   - Blank lines and lines whose first non-blank character is '#' are comments. There are no
//     comments after a value ('#' starts a colour), no sections, no quoting, no escapes.
//   - Every other line is `key = value`: blanks (space, tab) around both are dropped. Keys are
//     ASCII letters, digits and '_', matched case-insensitively, as are the mode names.
//   - Unknown keys, malformed lines, bad values and a `[section]` line are reported (the caller
//     logs them) and ignored; the setting keeps its earlier value. A key given twice: the last
//     one counts. Never fatal.
//   - A file over kMaxSettingsBytes is ignored as a whole.
//   - On/off values: on, off, true, false, yes, no, 1, 0 (any case); written as on or off.
//
// The presets (#144, ADR 0070):
//   - `preset = original | original_controller | enhanced | custom`. A preset other than
//     custom sets every key it controls (the rows' preset columns) and wins over those keys'
//     lines in the file, wherever the preset line is: a line that disagrees is reported and
//     ignored. `custom` keeps every key's own line. Keys outside the presets (the volumes,
//     right_touch, ...) are the file's in every preset.
//   - Changing a key the presets control (apply_setting: the command line, the settings menu)
//     moves a preset to custom when the new value differs from the preset's.
//   - No `preset` line (no file, or a file from before #144): no preset (Settings::preset
//     empty), every key as the file or the defaults say, exactly as before. The frontend then
//     shows the first-launch picker in an interactive run (preset_picker.h).
#ifndef MAPLETHIEF_PLATFORM_SETTINGS_H
#define MAPLETHIEF_PLATFORM_SETTINGS_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "scaling.h"

namespace maplethief::frontend {

inline constexpr const char *kSettingsFileName = "settings.ini";
inline constexpr std::size_t kMaxSettingsBytes = 64 * 1024;

// right_touch (#65, ADR 0047): the right mouse button as a second touch at a fixed screen point.
struct RightTouch {
    bool on = false;
    std::uint32_t x = 0, y = 0; // screen points, x < 480, y < 320

    bool operator==(const RightTouch &) const = default;
};

// "off", or "X,Y" (whole screen points, x 0-479, y 0-319, blanks allowed around each) to the
// setting; false otherwise.
bool parse_right_touch(std::string_view text, RightTouch *out);

// hide_keypad (#71, ADR 0052): when the on-screen key pad is hidden (keypad_hide.h).
enum class KeypadMode { kAuto, kAlways, kNever };
// "auto", "always", "never".
const char *keypad_mode_name(KeypadMode mode);
// One of those names, in any case (no blanks).
bool parse_keypad_mode(std::string_view text, KeypadMode *out);

// gfx_backend (#120, ADR 0060): what executes mt_gfx's draws. kGpu installs the GPU backend
// (maplethief/gfx_gpu.h) and presents with SDL's GPU renderer on the same device; without a
// usable GPU the frontend logs it and stays on the software rasterizer.
enum class GfxBackend { kSoftware, kGpu };
// "software", "gpu".
const char *gfx_backend_name(GfxBackend backend);
// One of those names, in any case (no blanks).
bool parse_gfx_backend(std::string_view text, GfxBackend *out);

// filter (#88, ADR 0066): a fragment shader of the present stage (gfx_backend.h's PresentPass)
// that draws the screen image into the scale mode's rectangle. It needs SDL's GPU renderer for
// the window, which the frontend asks for whatever gfx_backend is; without one it logs that and
// draws with the scale mode's own filter.
//   none  the scale mode's filter (the default)
//   xbr   xBR-style edge smoothing (present_xbr.frag)
//   crt   scanlines and an aperture mask (present_crt.frag)
//   lcd   an LCD cell grid (present_lcd.frag)
enum class ShaderFilter { kNone, kXbr, kCrt, kLcd };
// "none", "xbr", "crt", "lcd".
const char *shader_filter_name(ShaderFilter filter);
// One of those names, in any case (no blanks).
bool parse_shader_filter(std::string_view text, ShaderFilter *out);

// The names of each enum's values, in the enum's order (value 0 first): the registry's choice
// rows use them, and the *_name functions above return them.
inline constexpr const char *kScaleModeNames[] = {"integer", "fit", "smooth", "sharp"};
inline constexpr const char *kGfxBackendNames[] = {"software", "gpu"};
inline constexpr const char *kShaderFilterNames[] = {"none", "xbr", "crt", "lcd"};
inline constexpr const char *kKeypadModeNames[] = {"auto", "always", "never"};

// preset (#144, ADR 0070): the whole experience in one setting. The first three are the columns
// of the registry; custom is whatever the player changed.
enum class Preset { kOriginal, kOriginalController, kEnhanced, kCustom };
inline constexpr Preset kPresets[] = {Preset::kOriginal, Preset::kOriginalController,
                                      Preset::kEnhanced, Preset::kCustom};
inline constexpr const char *kPresetNames[] = {"original", "original_controller", "enhanced",
                                               "custom"};
// "original", "original_controller", "enhanced", "custom" (the file's and --preset's values).
const char *preset_name(Preset preset);
// One of those names, in any case (no blanks).
bool parse_preset(std::string_view text, Preset *out);
// "Original", "Original + controller", "Enhanced", "Custom" (the picker's and the menu's).
const char *preset_title(Preset preset);
// What the preset is, in one line of at most 64 characters (the picker).
const char *preset_summary(Preset preset);

struct Settings {
    // The preset (#144): empty when nothing chose one yet (no file, or one without the key).
    std::optional<Preset> preset;
    ScaleMode scale_mode = ScaleMode::kInteger;     // scale_mode, --scale-mode
    Color border_color;                             // border_color, --border-color (black)
    std::uint32_t render_scale = 1;                 // render_scale, --render-scale (1 to 8)
    GfxBackend gfx_backend = GfxBackend::kSoftware; // gfx_backend, --gfx-backend
    ShaderFilter filter = ShaderFilter::kNone;      // filter, --filter
    // hd_text, --hd-text (on | off; #88): the text the game draws into its text textures is
    // drawn a second time at render_scale times the size, and the textures sample that copy
    // (maplethief/cg.h). No effect at render scale 1.
    bool hd_text = false;
    // Audio (#64, ADR 0045's buses): percent, 0 to 100; the bus gain is volume / 100.
    std::uint32_t master_volume = 100;  // master_volume, --master-volume
    std::uint32_t music_volume = 100;   // music_volume, --music-volume
    std::uint32_t effects_volume = 100; // effects_volume, --effects-volume
    bool mute_unfocused = true;         // mute_unfocused, --mute-unfocused (on | off)
    // ADR 0062: the music's loops without its files' leading and trailing digital silence.
    bool gapless_music = false;                 // gapless_music, --gapless-music (on | off)
    RightTouch right_touch;                     // right_touch, --right-touch (off | X,Y)
    KeypadMode hide_keypad = KeypadMode::kAuto; // hide_keypad, --hide-keypad
    // Rumble (#72, rumble.h): the active gamepad on a vibration, a hit and a level-up.
    bool rumble = true;                  // rumble, --rumble (on | off)
    std::uint32_t rumble_strength = 100; // rumble_strength, --rumble-strength (0 to 100)

    bool operator==(const Settings &) const = default;
};

inline constexpr std::uint32_t kMaxVolume = 100;
inline constexpr std::uint32_t kMaxRumbleStrength = 100;

// --- The registry (#144, ADR 0070) ------------------------------------------------------------

// The settings menu's sections (#89). Enhancements (presentation and comfort) sit in Video,
// Audio, Controls, Gameplay and Accessibility; Assists hold whatever changes gameplay (game
// speed, turbo, auto-potion, cheats) and are never turned on by a preset.
enum class SettingGroup {
    kVideo,
    kAudio,
    kControls,
    kGameplay,
    kAccessibility,
    kAssists,
    kSaves,
    kOther
};
// "Video", "Audio", "Controls", "Gameplay", "Accessibility", "Assists", "Saves", "Other".
const char *setting_group_name(SettingGroup group);

// A value's type, which says how the menu edits it.
enum class ValueKind {
    kChoice, // one of a list of names (an enum whose values are 0, 1, ... in the names' order)
    kOnOff,  // on | off (also true/false, yes/no, 1/0 when read)
    kUint,   // a whole number from min to max (at most three digits)
    kColor,  // #RRGGBB
    kCustom, // its own parser and printer
};

struct SettingValue;
// Reads `text` (trimmed) into the row's field; false (the field unchanged) if it is not a value.
using SettingParse = bool (*)(const SettingValue &value, std::string_view text, Settings *s);
// The field's value as the file writes it.
using SettingPrint = std::string (*)(const SettingValue &value, const Settings &s);

// A row's value: its type and the field it is bound to (through parse and print). Made by
// choice<&Settings::f>(names), on_off<&Settings::f>(), uint_range<&Settings::f>(min, max, step),
// color<&Settings::f>() or custom_value(parse, print, expect).
struct SettingValue {
    ValueKind kind = ValueKind::kCustom;
    SettingParse parse = nullptr;
    SettingPrint print = nullptr;
    std::span<const char *const> names = {}; // kChoice: the names, value 0 first
    std::uint32_t min = 0, max = 0;          // kUint: the range
    std::uint32_t step = 1;                  // kUint: the menu's step
    // kCustom: what a good value is, after "is not" in the message about a bad one.
    const char *expect = nullptr;
};

// A row's value in each preset: the text the file would have (exactly as the row's printer
// writes it). All three null: kNotInPresets, the setting keeps its own value in every preset.
struct PresetColumns {
    const char *original = nullptr;
    const char *original_controller = nullptr;
    const char *enhanced = nullptr;

    constexpr bool in_presets() const { return original != nullptr; }
    // The column of a preset; nullptr for custom or outside the presets.
    constexpr const char *operator[](Preset p) const {
        return p == Preset::kOriginal             ? original
               : p == Preset::kOriginalController ? original_controller
               : p == Preset::kEnhanced           ? enhanced
                                                  : nullptr;
    }
};
inline constexpr PresetColumns kNotInPresets{};

// When the frontend applies a change of the setting while the game runs (#89).
enum class Applies {
    kNextStart, // read at start-up only: a change counts from the next start
    kAtOnce,    // the frontend applies a change at once (the menu says nothing)
};

// One setting. The table is in settings.cpp, written with designated initialisers.
struct SettingInfo {
    const char *key;     // lower-case letters, digits and '_'
    SettingGroup group;  // the menu's section
    const char *comment; // the line(s) above the key in a new file, each "# ...", '\n' between
    SettingValue value;  // the type, bound to the field
    PresetColumns presets = kNotInPresets;
    const char *flag = nullptr; // "--scale-mode": a command-line flag that sets it, or none
    bool game_only = false;     // the flag is a game option (not for the placeholder or tests)
    const char *label = "";     // the settings menu's label (#89), e.g. "Scale mode"
    const char *help = "";      // one line under it in the menu
    Applies applies = Applies::kNextStart;
};

namespace settings_detail {
// The index of `text` in `names`, ASCII case-insensitive; -1 if none.
int find_name(std::span<const char *const> names, std::string_view text);
bool parse_on_off(std::string_view text, bool *out);
// Digits only, at most 3 of them.
bool parse_small_uint(std::string_view text, std::uint32_t *out);
std::string color_text(const Color &c);

template <auto Field> using FieldType = std::remove_cvref_t<decltype(Settings{}.*Field)>;

template <auto Field> bool parse_choice(const SettingValue &v, std::string_view t, Settings *s) {
    const int i = find_name(v.names, t);
    if (i < 0) {
        return false;
    }
    s->*Field = static_cast<FieldType<Field>>(i);
    return true;
}
template <auto Field> std::string print_choice(const SettingValue &v, const Settings &s) {
    const auto i = static_cast<std::size_t>(s.*Field);
    return i < v.names.size() ? v.names[i] : "?";
}
template <auto Field> bool parse_switch(const SettingValue &, std::string_view t, Settings *s) {
    return parse_on_off(t, &(s->*Field));
}
template <auto Field> std::string print_switch(const SettingValue &, const Settings &s) {
    return s.*Field ? "on" : "off";
}
template <auto Field> bool parse_uint(const SettingValue &v, std::string_view t, Settings *s) {
    std::uint32_t n = 0;
    if (!parse_small_uint(t, &n) || n < v.min || n > v.max) {
        return false;
    }
    s->*Field = n;
    return true;
}
template <auto Field> std::string print_uint(const SettingValue &, const Settings &s) {
    return std::to_string(s.*Field);
}
template <auto Field> bool parse_rgb(const SettingValue &, std::string_view t, Settings *s) {
    return parse_color(t, &(s->*Field));
}
template <auto Field> std::string print_rgb(const SettingValue &, const Settings &s) {
    return color_text(s.*Field);
}
} // namespace settings_detail

// The value types, bound to a field of Settings.
template <auto Field> constexpr SettingValue choice(std::span<const char *const> names) {
    static_assert(std::is_enum_v<settings_detail::FieldType<Field>>);
    return {.kind = ValueKind::kChoice,
            .parse = &settings_detail::parse_choice<Field>,
            .print = &settings_detail::print_choice<Field>,
            .names = names};
}
template <auto Field> constexpr SettingValue on_off() {
    static_assert(std::is_same_v<settings_detail::FieldType<Field>, bool>);
    return {.kind = ValueKind::kOnOff,
            .parse = &settings_detail::parse_switch<Field>,
            .print = &settings_detail::print_switch<Field>};
}
template <auto Field>
constexpr SettingValue uint_range(std::uint32_t min, std::uint32_t max, std::uint32_t step = 1) {
    static_assert(std::is_same_v<settings_detail::FieldType<Field>, std::uint32_t>);
    return {.kind = ValueKind::kUint,
            .parse = &settings_detail::parse_uint<Field>,
            .print = &settings_detail::print_uint<Field>,
            .min = min,
            .max = max,
            .step = step};
}
template <auto Field> constexpr SettingValue color() {
    static_assert(std::is_same_v<settings_detail::FieldType<Field>, Color>);
    return {.kind = ValueKind::kColor,
            .parse = &settings_detail::parse_rgb<Field>,
            .print = &settings_detail::print_rgb<Field>};
}
constexpr SettingValue custom_value(SettingParse parse, SettingPrint print, const char *expect) {
    return {.kind = ValueKind::kCustom, .parse = parse, .print = print, .expect = expect};
}

// Every setting, in the order a new file lists them (after `preset`).
std::span<const SettingInfo> settings_registry();
// The row of a key (ASCII case-insensitive) or of a flag ("--hd-text"); nullptr if none. The
// preset is not a row: kPresetKey and kPresetFlag.
const SettingInfo *find_setting(std::string_view key);
const SettingInfo *find_setting_flag(std::string_view flag);
inline constexpr const char *kPresetKey = "preset";
inline constexpr const char *kPresetFlag = "--preset";

// A row's value in `settings`, as the file writes it.
std::string setting_text(const Settings &settings, const SettingInfo &info);
// What a good value of the row is: "integer, fit, smooth or sharp", "on or off", "an integer
// from 0 to 100", "#RRGGBB", or the custom row's own words.
std::string setting_expects(const SettingInfo &info);
// The values the menu cycles through: a choice's names, "off" and "on", or every step of a
// range; empty for a colour or a custom value.
std::vector<std::string> setting_choices(const SettingInfo &info);
// The menu's Left / Right (#89): the previous or next of setting_choices (wrapping for a choice
// or a switch, clamped for a range), through apply_setting (so a preset moves to custom).
// False if the row has no choices.
bool step_setting(Settings *settings, const SettingInfo &info, int delta);

// Checks a table of rows (the registry's own is checked by a static_assert and the tests):
// keys and flags unique and well formed (a flag is "--" and the key with '-' for '_'), comment
// lines starting with '#', every preset column a value the row reads back as the same text, and
// every Assists row in the presets with the same value in each column, the value a
// default-constructed Settings has (off / neutral). Returns one message per problem.
std::vector<std::string> check_registry(std::span<const SettingInfo> rows);

// --- Presets ----------------------------------------------------------------------------------

// Sets every row in the presets to the preset's column and settings->preset to the preset;
// custom changes only settings->preset. Keys outside the presets keep their values.
void apply_preset(Settings *settings, Preset preset);
// The rows in the presets whose value in `settings` is not the preset's column (custom: none).
std::vector<const SettingInfo *> preset_differences(const Settings &settings, Preset preset);
// The first of original, original_controller and enhanced whose column `settings` has in full;
// empty if none.
std::optional<Preset> matching_preset(const Settings &settings);

// --- The file ---------------------------------------------------------------------------------

// Sets one setting from its key and value (both already trimmed), as the command line and the
// settings menu change a setting. `preset` applies the preset (apply_preset). Another key in the
// presets moves a preset other than custom to custom when its new value is not the preset's.
// Returns false, with `why`, for an unknown key or a bad value, leaving `settings` unchanged.
bool apply_setting(Settings *settings, std::string_view key, std::string_view value,
                   std::string *why);

// Parses a settings file's text over `settings`. Returns the problems found, one message per
// ignored line ("line 3: unknown key 'scael_mode'"), in order; empty if there were none. With a
// `preset` line (the last good one counts) other than custom, that preset's columns win over
// the file's lines for the keys it controls ("line 7: hd_text = off: preset = enhanced sets
// on, ignored"); without one, each line is apply_setting.
std::vector<std::string> parse_settings(std::string_view text, Settings *settings);

// The file's text for `settings`. With `old_text` (the file as it is) empty: `preset` (when
// set) and every key with its comment line(s), in the registry's order. Otherwise `old_text` is
// kept byte for byte (comments, unknown keys, malformed lines, a byte order mark, its line
// endings), except that the value of each line that sets a known key is replaced by the
// setting's value, and the keys it lacks are appended, each after its comment. `preset` counts
// as known only when set (a preset line in the old text with no preset in `settings` becomes a
// comment). parse_settings(serialise_settings(s, t)) gives s for any t; an old text over
// kMaxSettingsBytes (which parse_settings ignores) is replaced whole.
std::string serialise_settings(const Settings &settings, std::string_view old_text = {});

// Text for a log message: at most `max` bytes of `s`, bytes outside printable ASCII as \xNN,
// "..." if cut.
std::string printable(std::string_view s, std::size_t max = 48);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_SETTINGS_H
