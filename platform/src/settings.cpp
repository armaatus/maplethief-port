// The frontend's settings file (#59, ADR 0043) and the settings registry and presets (#144,
// ADR 0070); see settings.h.
#include "settings.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <iterator>

#include "maplethief/gfx.h"
#include "port_text.h"

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

// texture_pack (#153): a directory, any text but control characters (the file has no quoting:
// blanks at its ends are dropped); empty is none.
bool parse_texture_pack(const SettingValue &, std::string_view text, Settings *s) {
    for (const char c : text) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
            return false;
        }
    }
    s->texture_pack = std::string(text);
    return true;
}

std::string print_texture_pack(const SettingValue &, const Settings &s) { return s.texture_pack; }

// audio_pack (#188): a directory, as texture_pack.
bool parse_audio_pack(const SettingValue &, std::string_view text, Settings *s) {
    for (const char c : text) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
            return false;
        }
    }
    s->audio_pack = std::string(text);
    return true;
}

std::string print_audio_pack(const SettingValue &, const Settings &s) { return s.audio_pack; }

// text_pack (#189): a directory, as texture_pack.
bool parse_text_pack(const SettingValue &, std::string_view text, Settings *s) {
    for (const char c : text) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
            return false;
        }
    }
    s->text_pack = std::string(text);
    return true;
}

std::string print_text_pack(const SettingValue &, const Settings &s) { return s.text_pack; }

// fullscreen (#179): on or off; desktop and borderless (the value before #179, and the flag's
// shorthand) are on in that mode.
constexpr const char *kFullscreenChoices[] = {"off", "on"};
bool parse_fullscreen(const SettingValue &, std::string_view text, Settings *s) {
    if (settings_detail::parse_on_off(text, &s->fullscreen)) {
        return true;
    }
    const int mode = settings_detail::find_name(kFullscreenModeNames, text);
    if (mode < 0) {
        return false;
    }
    s->fullscreen = true;
    s->fullscreen_mode = static_cast<FullscreenMode>(mode);
    return true;
}
std::string print_fullscreen(const SettingValue &, const Settings &s) {
    return s.fullscreen ? "on" : "off";
}

// gamma (#187): 0.80 to 1.40, written with two decimals, read with none, one or two ("1",
// "1.2", "1.25"); kept in hundredths.
constexpr const char *kGammaChoices[] = {"0.80", "0.85", "0.90", "0.95", "1.00", "1.05", "1.10",
                                         "1.15", "1.20", "1.25", "1.30", "1.35", "1.40"};
static_assert(std::size(kGammaChoices) == (kGammaMax - kGammaMin) / kGammaStep + 1);
bool parse_gamma(const SettingValue &, std::string_view text, Settings *s) {
    const std::size_t dot = text.find('.');
    const std::string_view whole = text.substr(0, dot);
    const std::string_view frac = dot == std::string_view::npos ? "" : text.substr(dot + 1);
    if (whole.size() != 1 || frac.size() > 2 || (dot != std::string_view::npos && frac.empty())) {
        return false;
    }
    std::uint32_t n = 0;
    for (const char c :
         std::string(whole) + std::string(frac) + std::string(2 - frac.size(), '0')) {
        if (c < '0' || c > '9') {
            return false;
        }
        n = n * 10 + static_cast<std::uint32_t>(c - '0');
    }
    if (n < kGammaMin || n > kGammaMax) {
        return false;
    }
    s->gamma = n;
    return true;
}
std::string print_gamma(const SettingValue &, const Settings &s) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%u.%02u", s.gamma / 100, s.gamma % 100);
    return buf;
}

bool parse_keypad_layout_value(const SettingValue &, std::string_view text, Settings *s) {
    return parse_keypad_layout(text, &s->keypad_layout);
}

std::string print_keypad_layout(const SettingValue &, const Settings &s) {
    return keypad_layout_text(s.keypad_layout);
}
// border_image (#157): a file, any text but control characters (the file has no quoting:
// blanks at its ends are dropped), at most kMaxBorderImagePath bytes; empty is none.
constexpr std::size_t kMaxBorderImagePath = 4096;

bool parse_border_image(const SettingValue &, std::string_view text, Settings *s) {
    if (text.size() > kMaxBorderImagePath) {
        return false;
    }
    for (const char c : text) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
            return false;
        }
    }
    s->border_image = std::string(text);
    return true;
}

std::string print_border_image(const SettingValue &, const Settings &s) { return s.border_image; }

// audio_device (#195): `default` (any case; empty too) or a device's name: any text but control
// characters, at most kMaxAudioDeviceBytes bytes.
bool parse_audio_device(const SettingValue &, std::string_view text, Settings *s) {
    if (text.size() > kMaxAudioDeviceBytes) {
        return false;
    }
    for (const char c : text) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
            return false;
        }
    }
    s->audio_device = key_is(text, kAudioDeviceDefault) ? std::string() : std::string(text);
    return true;
}

std::string print_audio_device(const SettingValue &, const Settings &s) {
    return s.audio_device.empty() ? kAudioDeviceDefault : s.audio_device;
}

std::vector<std::string> &audio_device_names() {
    static std::vector<std::string> names;
    return names;
}

// `default`, then the devices audio_out.cpp listed (a name `default` itself is left out); none
// while it listed none (no device open: the menu shows the value only).
std::vector<std::string> offer_audio_devices() {
    std::vector<std::string> out = {kAudioDeviceDefault};
    for (const std::string &name : audio_device_names()) {
        if (!name.empty() && !key_is(name, kAudioDeviceDefault) &&
            std::find(out.begin(), out.end(), name) == out.end()) {
            out.push_back(name);
        }
    }
    return out.size() > 1 ? out : std::vector<std::string>{};
}

// fullscreen_display (#196): `current` (any case; empty too) or a display's name, as
// audio_device's.
bool parse_fullscreen_display(const SettingValue &, std::string_view text, Settings *s) {
    if (text.size() > kMaxAudioDeviceBytes) {
        return false;
    }
    for (const char c : text) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
            return false;
        }
    }
    s->fullscreen_display =
        key_is(text, kFullscreenDisplayCurrent) ? std::string() : std::string(text);
    return true;
}

std::string print_fullscreen_display(const SettingValue &, const Settings &s) {
    return s.fullscreen_display.empty() ? kFullscreenDisplayCurrent : s.fullscreen_display;
}

std::vector<std::string> &display_names() {
    static std::vector<std::string> names;
    return names;
}

// `current`, then the displays desktop.cpp listed; none while it listed none.
std::vector<std::string> offer_displays() {
    std::vector<std::string> out = {kFullscreenDisplayCurrent};
    for (const std::string &name : display_names()) {
        if (!name.empty() && !key_is(name, kFullscreenDisplayCurrent) &&
            std::find(out.begin(), out.end(), name) == out.end()) {
            out.push_back(name);
        }
    }
    return out.size() > 1 ? out : std::vector<std::string>{};
}

constexpr SettingValue fullscreen_display_value() {
    SettingValue v = custom_value(&parse_fullscreen_display, &print_fullscreen_display,
                                  "current or a display's name");
    v.offer = &offer_displays;
    return v;
}

constexpr SettingValue audio_device_value() {
    SettingValue v = custom_value(&parse_audio_device, &print_audio_device,
                                  "default or an output device's name");
    v.offer = &offer_audio_devices;
    return v;
}

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
     .help = "How the image fills the window: integer is crisp, fit fills more of it",
     .applies = Applies::kAtOnce},
    {.key = "border_color",
     .group = SettingGroup::kVideo,
     .comment = "# the bars around the image, #RRGGBB",
     .value = color<&Settings::border_color>(kBorderColorChoices),
     .flag = "--border-color",
     .label = "Border colour",
     .help = "The colour of the bars around the image",
     .applies = Applies::kAtOnce},
    {.key = "render_scale",
     .group = SettingGroup::kVideo,
     .comment = "# the renderbuffers' internal scale, 1 to 8, or auto (the largest that fits the "
                "frame budget and the window)",
     .value = uint_range<&Settings::render_scale>(1, MT_GFX_MAX_SCALE, 1, "auto"),
     .presets = {"1", "1", "auto"},
     .flag = "--render-scale",
     .game_only = true,
     .label = "Render scale",
     .help = "Draws the game's GL layer at N times 480 x 320: smoother, slower; auto fits it",
     .applies = Applies::kAtOnce,
     .frame_cost = true},
    {.key = "gfx_backend",
     .group = SettingGroup::kVideo,
     .comment = "# what draws the game's GL frames: software (the reference) | gpu (SDL3 GPU)",
     .value = choice<&Settings::gfx_backend>(kGfxBackendNames),
     // Enhanced: `gpu` once #131 makes it the equal of the reference.
     .presets = {"software", "software", "software"},
     .flag = "--gfx-backend",
     .label = "Renderer",
     .help = "Software is the reference; GPU draws on the graphics card",
     .frame_cost = true},
    {.key = "filter",
     .group = SettingGroup::kVideo,
     .comment = "# a shader filter over the scaled image: none | xbr | crt | lcd",
     .value = choice<&Settings::filter>(kShaderFilterNames),
     .presets = {"none", "none", "none"},
     .flag = "--filter",
     .game_only = true,
     .label = "Filter",
     .help = "A shader over the image: xBR smoothing, CRT scanlines or an LCD grid",
     .applies = Applies::kAtOnce},
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
     .help = "Everything you hear, in percent",
     .applies = Applies::kAtOnce},
    {.key = "music_volume",
     .group = SettingGroup::kAudio,
     .comment = "# volume of the music, 0 to 100",
     .value = uint_range<&Settings::music_volume>(0, kMaxVolume, 5),
     .flag = "--music-volume",
     .label = "Music volume",
     .help = "The music, in percent of the volume",
     .applies = Applies::kAtOnce},
    {.key = "effects_volume",
     .group = SettingGroup::kAudio,
     .comment = "# volume of the sound effects, 0 to 100",
     .value = uint_range<&Settings::effects_volume>(0, kMaxVolume, 5),
     .flag = "--effects-volume",
     .label = "Effects volume",
     .help = "The sound effects, in percent of the volume",
     .applies = Applies::kAtOnce},
    {.key = "mute_unfocused",
     .group = SettingGroup::kAudio,
     .comment = "# silence while the window is not focused: on | off",
     .value = on_off<&Settings::mute_unfocused>(),
     .presets = {"on", "on", "on"},
     .flag = "--mute-unfocused",
     .label = "Mute in the background",
     .help = "Silence while the window is not focused, as the phone in the background",
     .applies = Applies::kAtOnce},
    {.key = "gapless_music",
     .group = SettingGroup::kAudio,
     .comment = "# loop the music without the silence at the start and end of its files: on | off "
                "(off: as the device)",
     .value = on_off<&Settings::gapless_music>(),
     .presets = {"off", "off", "on"},
     .flag = "--gapless-music",
     .label = "Gapless music",
     .help = "Loops the music without the silence at the ends of its files"},
    {.key = "audio_mono",
     .group = SettingGroup::kAudio,
     .comment = "# both channels the same mix, for one ear or one speaker: on | off",
     .value = on_off<&Settings::audio_mono>(),
     .flag = "--audio-mono",
     .label = "Mono",
     .help = "Left and right mixed into both channels, for one ear or one speaker",
     .applies = Applies::kAtOnce},
    {.key = "audio_device",
     .group = SettingGroup::kAudio,
     .comment = "# the sound's output device, by its name, or default (the system's; from the "
                "menu's list)",
     .value = audio_device_value(),
     .flag = "--audio-device",
     .label = "Output device",
     .help = "Where the sound plays; default follows the system's choice",
     .applies = Applies::kAtOnce},
    {.key = "audio_pack",
     .group = SettingGroup::kAudio,
     .comment = "# a directory of your own music and sound files that replace the game's, by "
                "name (python3 -m mtport.audiopack init lists them); empty: none",
     .value = custom_value(&parse_audio_pack, &print_audio_pack,
                           "a directory (empty: none), without control characters"),
     .flag = "--audio-pack",
     .game_only = true,
     .label = "Audio pack",
     .help = "A folder of music and sound files that replace the game's, by file name"},
    // #197 (audio_session.h): read when the device opens; iOS only, outside the presets.
    {.key = "ios_audio",
     .group = SettingGroup::kAudio,
     .comment = "# iOS: the audio session: original (mixes with other apps' music, quiet with "
                "the Silent switch) | own (plays with the switch on silent, other apps' music "
                "turned down)",
     .value = choice<&Settings::ios_audio>(kIosAudioNames),
     .flag = "--ios-audio",
     .label = "Audio session",
     .help = "original: mixes with other apps' music, quiet with the Silent switch. own: plays "
             "with the switch on silent, other apps' music turned down",
     .ios_only = true},
    // Controls.
    {.key = "right_touch",
     .group = SettingGroup::kControls,
     .comment = "# the right mouse button: a touch at screen point X,Y, or off",
     .value = custom_value(&parse_right_touch_value, &print_right_touch,
                           "off or X,Y (screen points, 0-479, 0-319)", kRightTouchChoices),
     .flag = "--right-touch",
     .label = "Right mouse button",
     .help = "A second touch at a fixed screen point X,Y, or off",
     .applies = Applies::kAtOnce},
    {.key = "hide_keypad",
     .group = SettingGroup::kControls,
     .comment = "# hide the on-screen key pad: auto (while a gamepad or the keyboard is used) | "
                "always | never",
     .value = choice<&Settings::hide_keypad>(kKeypadModeNames),
     .presets = {"never", "auto", "auto"},
     .flag = "--hide-keypad",
     .label = "Hide the key pad",
     .help = "Auto hides the touch key pad while a gamepad or the keyboard is used",
     .applies = Applies::kAtOnce},
    {.key = "rumble",
     .group = SettingGroup::kControls,
     .comment = "# the gamepad rumbles on a vibration, a hit and a level-up: on | off",
     .value = on_off<&Settings::rumble>(),
     // Original and Original + controller: the game's vibration only, once #72's follow-up
     // splits it from the hit and the level-up.
     .presets = {"on", "on", "on"},
     .flag = "--rumble",
     .label = "Rumble",
     .help = "The gamepad rumbles on the game's vibration, a hit and a level-up",
     .applies = Applies::kAtOnce},
    {.key = "rumble_strength",
     .group = SettingGroup::kControls,
     .comment = "# how strongly, 0 to 100 (percent)",
     .value = uint_range<&Settings::rumble_strength>(0, kMaxRumbleStrength, 10),
     .flag = "--rumble-strength",
     .label = "Rumble strength",
     .help = "How strongly the gamepad rumbles, in percent",
     .applies = Applies::kAtOnce},
    // #194 (keypad_haptics.h): read live by game.cpp; iOS only, outside the presets.
    {.key = "keypad_haptics",
     .group = SettingGroup::kControls,
     .comment = "# iOS: a light tap of the phone at each press of a touch key pad key: off | "
                "light | medium",
     .value = choice<&Settings::keypad_haptics>(kKeypadHapticsNames),
     .flag = "--keypad-haptics",
     .label = "Key pad haptics",
     .help = "A haptic tap at each press of a touch key pad key: light or medium",
     .applies = Applies::kAtOnce,
     .ios_only = true},
    // Video, after the input keys (the order of a new file since #137).
    {.key = "perf_overlay",
     .group = SettingGroup::kVideo,
     .comment = "# the frame-time overlay (F3 shows or hides it while the game runs): on | off",
     .value = on_off<&Settings::perf_overlay>(),
     .presets = {"off", "off", "off"},
     .flag = "--perf-overlay",
     .game_only = true,
     .label = "Frame-time overlay",
     .help = "Shows the frame time and the frame rate over the game (F3 toggles it)",
     .applies = Applies::kAtOnce},
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
    {.key = "pacing",
     .group = SettingGroup::kVideo,
     .comment = "# frame pacing: display (each game tick lasts whole display refreshes when the "
                "refresh rate allows) | wall (the wall clock, as before #80)",
     .value = choice<&Settings::pacing>(kPacingNames),
     .presets = {"display", "display", "display"},
     .flag = "--pacing",
     .game_only = true,
     .label = "Frame pacing",
     .help = "Display: every game frame shown for the same number of refreshes; wall: as before"},
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
    // The desktop conveniences (#145, ADR 0077), applied at once by the frontend (desktop.h).
    {.key = "fullscreen",
     .group = SettingGroup::kVideo,
     .comment = "# the window: off | on (the whole display; F11 or Alt+Enter toggles it)",
     .value =
         custom_value(&parse_fullscreen, &print_fullscreen,
                      "on or off (desktop or borderless: on, in that mode)", kFullscreenChoices),
     .presets = {"off", "off", "on"},
     .flag = "--fullscreen",
     .game_only = true,
     .label = "Fullscreen",
     .help = "Fills the whole display; F11 or Alt+Enter toggles it",
     .applies = Applies::kAtOnce},
    {.key = "fullscreen_mode",
     .group = SettingGroup::kVideo,
     .comment = "# how macOS shows fullscreen: desktop (its own Space, as the green button) | "
                "borderless (over the desktop, no Space or animation); elsewhere the same",
     .value = choice<&Settings::fullscreen_mode>(kFullscreenModeNames),
     .presets = {"desktop", "desktop", "borderless"},
     .flag = "--fullscreen-mode",
     .game_only = true,
     .label = "Fullscreen mode",
     .help = "macOS: desktop is its own Space; borderless covers the desktop at once"},
    {.key = "fullscreen_display",
     .group = SettingGroup::kVideo,
     .comment = "# the display fullscreen goes to, by its name, or current (the window's; from "
                "the menu's list)",
     .value = fullscreen_display_value(),
     .flag = "--fullscreen-display",
     .game_only = true,
     .label = "Fullscreen display",
     .help = "Where fullscreen goes, from the next time it turns on; current: the window's",
     .applies = Applies::kAtOnce},
    {.key = "vsync",
     .group = SettingGroup::kVideo,
     .comment = "# wait for the display's refresh to present a frame (no tearing): on | off",
     .value = on_off<&Settings::vsync>(),
     .presets = {"on", "on", "on"},
     .flag = "--vsync",
     .game_only = true,
     .label = "Vsync",
     .help = "Presents in step with the display's refresh: no tearing",
     .applies = Applies::kAtOnce},
    {.key = "pause_unfocused",
     .group = SettingGroup::kGameplay,
     .comment = "# pause the game while the window is not focused, as when it is minimised: on | "
                "off",
     .value = on_off<&Settings::pause_unfocused>(),
     .presets = {"off", "on", "on"},
     .flag = "--pause-unfocused",
     .game_only = true,
     .label = "Pause in the background",
     .help = "The game pauses while its window is not focused, as when it is minimised",
     .applies = Applies::kAtOnce},
    {.key = "language",
     .group = SettingGroup::kOther,
     .comment = "# the game's language: en | ko (MAPLETHIEF_LANGUAGE overrides it; from the next "
                "start)",
     .value = choice<&Settings::language>(kLanguageNames),
     .flag = "--language",
     .game_only = true,
     .label = "Language",
     .help = "English or Korean (the Korean build's screens and help), from the next start"},
    {.key = "text_pack",
     .group = SettingGroup::kOther,
     .comment = "# a directory of translated text in UTF-8 (python3 -m mtport.textpack export "
                "makes one to edit); empty: none",
     .value = custom_value(&parse_text_pack, &print_text_pack,
                           "a directory (empty: none), without control characters"),
     .flag = "--text-pack",
     .game_only = true,
     .label = "Text pack",
     .help = "A folder of translated text: dialogue, item names and this menu, from the next "
             "start"},
    // Gameplay (#155, docs/re/original-bugs.md): crashes and dead ends only, never balance.
    {.key = "fix_original_bugs",
     .group = SettingGroup::kGameplay,
     .comment = "# fix the original game's crashes and dead ends (docs/re/original-bugs.md): "
                "on | off",
     .value = on_off<&Settings::fix_original_bugs>(),
     .presets = {"off", "off", "on"},
     .flag = "--fix-original-bugs",
     .game_only = true,
     .label = "Fix original bugs",
     .help = "Fixes the original game's crashes and dead ends; never changes its balance"},
    // Video: texture packs (#153, ADR 0081).
    {.key = "texture_pack",
     .group = SettingGroup::kVideo,
     .comment = "# a directory of HD textures made from your own --dump-textures (python3 -m "
                "mtport.texpack); empty: none",
     .value = custom_value(&parse_texture_pack, &print_texture_pack,
                           "a directory (empty: none), without control characters"),
     .flag = "--texture-pack",
     .game_only = true,
     .label = "Texture pack",
     .help = "A folder of HD textures and sprites made from your own copy of the game"},
    {.key = "texture_filter",
     .group = SettingGroup::kVideo,
     .comment = "# how textures are sampled at render scale 2 and up: game (smooth, the device's "
                "look) | nearest (sharp)",
     .value = choice<&Settings::texture_filter>(kTextureFilterNames),
     .presets = {"game", "game", "nearest"},
     .flag = "--texture-filter",
     .game_only = true,
     .label = "Texture filter",
     .help = "Nearest keeps sprites sharp at a high render scale; game smooths them as the "
             "device"},
    // Offline polish (#154, ADR 0084).
    {.key = "hide_online_ui",
     .group = SettingGroup::kGameplay,
     .comment = "# hide the title menu's dead online buttons (the banner, More Games, About's "
                "links;\n# Game Center too while local_game_center is off): on | off",
     .value = on_off<&Settings::hide_online_ui>(),
     .presets = {"off", "on", "on"},
     .flag = "--hide-online-ui",
     .game_only = true,
     .label = "Hide dead online buttons",
     .help = "Hides the banner, More Games and the About links, whose pages are gone"},
    {.key = "local_game_center",
     .group = SettingGroup::kGameplay,
     .comment = "# Game Center on this computer: the leaderboard screen and local achievements: "
                "on | off",
     .value = on_off<&Settings::local_game_center>(),
     .presets = {"off", "off", "on"},
     .flag = "--local-game-center",
     .game_only = true,
     .label = "Local Game Center",
     .help = "Shows the leaderboard of your slots and unlocks achievements on this computer"},
    // Controls: the menus (#147, menu_input.h).
    {.key = "menu_repeat",
     .group = SettingGroup::kControls,
     .comment = "# a held direction repeats in the popup menu, NPC chat, the shop (its amount "
                "too) and the\n# map (after 0.4 s, then every 0.15 s): on | off",
     .value = on_off<&Settings::menu_repeat>(),
     .presets = {"off", "on", "on"},
     .flag = "--menu-repeat",
     .game_only = true,
     .label = "Menu key repeat",
     .help = "A held direction keeps moving the cursor in menus, lists and the map",
     .applies = Applies::kAtOnce},
    {.key = "confirm_guard",
     .group = SettingGroup::kControls,
     .comment = "# a popup that just opened ignores confirm and back for 0.3 s, and while a "
                "button\n# pressed in the field is still held: on | off",
     .value = on_off<&Settings::confirm_guard>(),
     .presets = {"off", "on", "on"},
     .flag = "--confirm-guard",
     .game_only = true,
     .label = "Popup confirm guard",
     .help = "A button mashed in the field can't answer a popup that has just opened",
     .applies = Applies::kAtOnce},
    // Controller hints (#146, prompts.h): drawn in the window only, never in a shot.
    {.key = "focus_highlight",
     .group = SettingGroup::kControls,
     .comment = "# with a gamepad or the keyboard, a frame around the focused button or the "
                "chosen\n# item where the game shows none: on | off",
     .value = on_off<&Settings::focus_highlight>(),
     .presets = {"on", "on", "on"},
     .flag = "--focus-highlight",
     .game_only = true,
     .label = "Focus highlight",
     .help = "A frame around the focused button or item when a gamepad or the keyboard is used",
     .applies = Applies::kAtOnce},
    {.key = "button_prompts",
     .group = SettingGroup::kControls,
     .comment = "# with a gamepad or the keyboard, its button next to each of the game's "
                "buttons\n# (Xbox, PlayStation, Nintendo or key glyphs): on | off",
     .value = on_off<&Settings::button_prompts>(),
     .presets = {"off", "on", "on"},
     .flag = "--button-prompts",
     .game_only = true,
     .label = "Button prompts",
     .help = "Shows the gamepad's or the keyboard's button next to the game's buttons",
     .applies = Applies::kAtOnce},
    {.key = "button_labels",
     .group = SettingGroup::kControls,
     .comment = "# with a gamepad or the keyboard, its button beside each key of the field's "
                "key pad\n# (jump, attack, the quick slots, Map, Bag, Cash): on | off",
     .value = on_off<&Settings::button_labels>(),
     .presets = {"on", "on", "on"},
     .flag = "--button-labels",
     .game_only = true,
     .label = "Key pad labels",
     .help = "Shows the gamepad's or the keyboard's button on the quick slots and attack keys",
     .applies = Applies::kAtOnce},
    // The gamepad pointer (#185, ADR 0107, pad_pointer.h): read live by the turn.
    {.key = "pad_pointer",
     .group = SettingGroup::kControls,
     .comment = "# a gamepad pointer for what needs a finger: the right stick moves it (its "
                "click held,\n# or the touchpad's click, taps), a finger on a touchpad places "
                "it: off | stick | touchpad | both",
     .value = choice<&Settings::pad_pointer>(kPadPointerNames),
     .presets = {"off", "both", "both"},
     .flag = "--pad-pointer",
     .game_only = true,
     .label = "Gamepad pointer",
     .help = "The right stick or a finger on a touchpad moves a pointer; R3 held or the "
             "touchpad's click taps",
     .applies = Applies::kAtOnce},
    // Controls: the touch key pad's layout (#159, ADR 0086). A personal layout, in no preset.
    {.key = "keypad_layout",
     .group = SettingGroup::kControls,
     .comment = "# the touch key pad: original, or for each group moved (dpad, actions, slots)\n"
                "# GROUP=SIZE,DX,DY,OPACITY (percent, screen points, percent), ';' between groups",
     .value = custom_value(&parse_keypad_layout_value, &print_keypad_layout,
                           "original or GROUP=SIZE,DX,DY,OPACITY[;...] (dpad, actions, slots; "
                           "size 50-200, offsets -480 to 480, opacity 0-100)"),
     .flag = "--keypad-layout",
     .game_only = true,
     .label = "Touch key pad layout",
     .help = "The touch key pad's size, place and opacity per group (the key pad editor)",
     .applies = Applies::kAtOnce},
    // Video: the border (#157, ADR 0085).
    {.key = "border",
     .group = SettingGroup::kVideo,
     .comment = "# what fills the window around the picture: color (border_color) | blur (the "
                "game, blurred) |\n# image (border_image)",
     .value = choice<&Settings::border>(kBorderModeNames),
     .presets = {"color", "color", "blur"},
     .flag = "--border",
     .game_only = true,
     .label = "Border",
     .help = "What fills the bars: the border colour, the game blurred, or your own picture",
     .applies = Applies::kAtOnce},
    {.key = "border_image",
     .group = SettingGroup::kVideo,
     .comment = "# the PNG of border = image, scaled to cover the window; empty: none",
     .value = custom_value(&parse_border_image, &print_border_image,
                           "a PNG file (empty: none), without control characters"),
     .flag = "--border-image",
     .game_only = true,
     .label = "Border picture",
     .help = "A PNG of your own for border = image, scaled to fill the window",
     .applies = Applies::kAtOnce},
    // Assists (#151, ADR 0087): the game speed and the fast-forward key, applied at once.
    {.key = "game_speed",
     .group = SettingGroup::kAssists,
     .comment = "# Assist: the game's tick length: game (the game's own Speed option) | 65 | 50 | "
                "40 | 33\n# | 25 (ms; 33 and 25 are faster than any the game offers)",
     .value = choice<&Settings::game_speed>(kGameSpeedNames),
     .presets = {"game", "game", "game"},
     .flag = "--game-speed",
     .game_only = true,
     .label = "Game speed",
     .help = "Game: the game's own Speed option; or a tick length in ms (25 is twice as fast)",
     .applies = Applies::kAtOnce},
    {.key = "fast_forward",
     .group = SettingGroup::kAssists,
     .comment = "# Assist: holding the fast-forward key (Tab, the right stick's click) runs the "
                "game: off\n# | x2 | x3 (times as fast)",
     .value = choice<&Settings::fast_forward>(kFastForwardNames),
     .presets = {"off", "off", "off"},
     .flag = "--fast-forward",
     .game_only = true,
     .label = "Fast-forward key",
     .help = "Holding the fast-forward key (Tab, the right stick's click) runs the game faster",
     .applies = Applies::kAtOnce},
    // Gameplay: autosave (#150, autosave.h, docs/re/autosave.md). It writes the save, so the
    // Original columns keep the game's own saves only.
    {.key = "autosave",
     .group = SettingGroup::kGameplay,
     .comment = "# save the game when it quits, goes to the background or exits to the main "
                "menu, where the game's own Save is allowed (not on boss maps, in a scene or "
                "with a popup open): off | on_quit",
     .value = choice<&Settings::autosave>(kAutosaveNames),
     .presets = {"off", "off", "on_quit"},
     .flag = "--autosave",
     .game_only = true,
     .label = "Autosave",
     .help = "Saves when you quit, leave or exit to the main menu, wherever the game's own Save "
             "would",
     .applies = Applies::kAtOnce},
    // Video: the wider field (#86, ADR 0090). The GL layer's size is set at the start.
    {.key = "widescreen",
     .group = SettingGroup::kVideo,
     .comment = "# the field up to 16:9 (568 x 320) on maps wide enough;\n# menus and narrow "
                "maps stay 3:2: off | auto",
     .value = choice<&Settings::widescreen>(kWidescreenNames),
     .presets = {"off", "off", "auto"},
     .flag = "--widescreen",
     .game_only = true,
     .label = "Widescreen",
     .help = "Shows more of the map beside the 3:2 picture where the map is wide enough"},
    // Video: where the HUD and the touch key pad go in the wide view (#168). Read every turn.
    {.key = "widescreen_hud",
     .group = SettingGroup::kVideo,
     .comment = "# with widescreen, the touch key pad and its icons at the wide view's edges, or "
                "the HUD\n# and the key pad kept at their 3:2 places in the middle: edges | narrow",
     .value = choice<&Settings::widescreen_hud>(kWidescreenHudNames),
     .presets = {"edges", "edges", "edges"},
     .flag = "--widescreen-hud",
     .game_only = true,
     .label = "Widescreen HUD",
     .help = "Edges: the key pad at the wide view's sides. Narrow: HUD and key pad stay at 3:2",
     .applies = Applies::kAtOnce},
    // Accessibility (#158, ADR 0093): the player's own needs, which no preset changes.
    {.key = "colorblind_filter",
     .group = SettingGroup::kAccessibility,
     .comment = "# the picture's colours corrected for colour blindness: off | protanopia | "
                "deuteranopia |\n# tritanopia",
     .value = choice<&Settings::colorblind_filter>(kColorblindFilterNames),
     .flag = "--colorblind-filter",
     .game_only = true,
     .label = "Colour-blind filter",
     .help = "Moves colours you confuse to ones you can tell apart (red-, green- or blue-blind)",
     .applies = Applies::kAtOnce},
    {.key = "screen_shake",
     .group = SettingGroup::kAccessibility,
     .comment = "# how far the scenes shake the camera, 0 to 100 (percent of the game's)",
     .value = uint_range<&Settings::screen_shake>(0, kMaxScreenShake, 10),
     .flag = "--screen-shake",
     .game_only = true,
     .label = "Screen shake",
     .help = "How much the camera shakes in the game's scenes, in percent (0: none)",
     .applies = Applies::kAtOnce},
    {.key = "flashes",
     .group = SettingGroup::kAccessibility,
     .comment = "# how strong the game's full-screen flashes are (a level-up's white and black "
                "frames,\n# the scenes' white-outs), 0 to 100 (percent of the game's)",
     .value = uint_range<&Settings::flashes>(0, kMaxFlashes, 25),
     .flag = "--flashes",
     .game_only = true,
     .label = "Flashes",
     .help = "How strong the full-screen white and black flashes are, in percent (0: none)",
     .applies = Applies::kAtOnce},
    // Video: the picture adjustments (#187, ADR 0093's addendum, color_filter.h), the colour
    // step with the colour-blind filter. The player's screen: no preset changes them.
    {.key = "brightness",
     .group = SettingGroup::kVideo,
     .comment = "# the picture's brightness, 50 to 150 (percent; 100: as the game drew it)",
     .value = uint_range<&Settings::brightness>(kPictureMin, kPictureMax, kPictureStep),
     .flag = "--brightness",
     .game_only = true,
     .label = "Brightness",
     .help = "Makes the whole picture lighter or darker, in percent (100: as the game drew it)",
     .applies = Applies::kAtOnce,
     .test_pattern = true},
    {.key = "contrast",
     .group = SettingGroup::kVideo,
     .comment = "# the picture's contrast about mid-grey, 50 to 150 (percent; 100: the game's)",
     .value = uint_range<&Settings::contrast>(kPictureMin, kPictureMax, kPictureStep),
     .flag = "--contrast",
     .game_only = true,
     .label = "Contrast",
     .help = "Spreads the darks and lights apart or draws them together, in percent",
     .applies = Applies::kAtOnce,
     .test_pattern = true},
    {.key = "saturation",
     .group = SettingGroup::kVideo,
     .comment = "# how colourful the picture is, 50 to 150 (percent; 100: the game's colours)",
     .value = uint_range<&Settings::saturation>(kPictureMin, kPictureMax, kPictureStep),
     .flag = "--saturation",
     .game_only = true,
     .label = "Saturation",
     .help = "More or less colour, in percent (100: the game's colours; greys stay grey)",
     .applies = Applies::kAtOnce,
     .test_pattern = true},
    {.key = "gamma",
     .group = SettingGroup::kVideo,
     .comment = "# the mid-tones' gamma, 0.80 to 1.40 (above 1.00 lighter, below darker; black "
                "and white\n# stay)",
     .value = custom_value(&parse_gamma, &print_gamma, "a gamma from 0.80 to 1.40", kGammaChoices),
     .flag = "--gamma",
     .game_only = true,
     .label = "Gamma",
     .help = "Lightens (above 1.00) or darkens the mid-tones; black and white stay as they are",
     .applies = Applies::kAtOnce,
     .test_pattern = true},
    // Video: frame interpolation (#87, ADR 0091, frame_interp.h).
    {.key = "frame_rate",
     .group = SettingGroup::kVideo,
     .comment = "# how often the window shows the game: original (each game tick's frame, 20 a "
                "second) | smooth\n# (every refresh, interpolated between the last two ticks: "
                "smoother, but about a tick later)",
     .value = choice<&Settings::frame_rate>(kFrameRateNames),
     .presets = {"original", "original", "original"},
     .flag = "--frame-rate",
     .game_only = true,
     .label = "Frame rate",
     .help = "Smooth: every refresh, between the game's frames (about 33 ms later at 60 Hz)",
     .applies = Applies::kAtOnce},
    // Quality of life (#90, ADR 0092, docs/re/qol.md).
    {.key = "ui_animations",
     .group = SettingGroup::kVideo,
     .comment = "# how fast the menus' UIKit animations run: normal | fast | instant",
     .value = choice<&Settings::ui_animations>(kUiAnimationsNames),
     .presets = {"normal", "normal", "normal"},
     .flag = "--ui-animations",
     .game_only = true,
     .label = "Menu animations",
     .help = "The title menu's fades and slides: as the game made them, faster, or none",
     .applies = Applies::kAtOnce},
    {.key = "turbo_attack",
     .group = SettingGroup::kAssists,
     .comment = "# Assist: holding attack in the field attacks again and again: on | off",
     .value = on_off<&Settings::turbo_attack>(),
     .presets = {"off", "off", "off"},
     .flag = "--turbo-attack",
     .game_only = true,
     .label = "Turbo attack",
     .help = "Holding attack keeps attacking (the game attacks once per press)",
     .applies = Applies::kAtOnce},
    {.key = "auto_potion",
     .group = SettingGroup::kAssists,
     .comment = "# Assist: drink the HP potion of a quick slot under this share of max HP: off | "
                "25 | 50\n# | 75 (percent)",
     .value = choice<&Settings::auto_potion>(kAutoPotionNames),
     .presets = {"off", "off", "off"},
     .flag = "--auto-potion",
     .game_only = true,
     .label = "Auto-potion",
     .help = "Under this share of max HP the hero drinks the HP potion on its quick slots",
     .applies = Applies::kAtOnce},
    // The scene skip (#174, ADR 0096, docs/re/qol.md#cutscene-skip).
    {.key = "skip_intros",
     .group = SettingGroup::kGameplay,
     .comment = "# scenes (the intro, chapter cards, boss entrances) run fast, each speech box "
                "tapped: on\n# | off",
     .value = on_off<&Settings::skip_intros>(),
     .presets = {"off", "off", "off"},
     .flag = "--skip-intros",
     .game_only = true,
     .label = "Skip scenes",
     .help = "Scenes run 8 times as fast and their speech boxes close by themselves",
     .applies = Applies::kAtOnce},
    // Video: the port's minimap (#176, ADR 0098, minimap.h). Read at every present.
    {.key = "minimap",
     .group = SettingGroup::kVideo,
     .comment = "# the port's minimap of the map in the field (platforms, ladders, the hero, "
                "portals and NPCs):\n# off | border (beside the picture where there is room, "
                "else over it) | overlay",
     .value = choice<&Settings::minimap>(kMinimapNames),
     .presets = {"off", "off", "off"},
     .flag = "--minimap",
     .game_only = true,
     .label = "Minimap",
     .help = "The map's platforms and where you are: beside the picture, or over its top",
     .applies = Applies::kAtOnce},
    // Video: the EXP percentage beside the HUD's bar (#191, ADR 0109, exp_percent.h).
    {.key = "exp_percent",
     .group = SettingGroup::kVideo,
     .comment = "# the hero's EXP to the next level as a percentage beside the HUD's bar: on | off",
     .value = on_off<&Settings::exp_percent>(),
     .presets = {"off", "off", "on"},
     .flag = "--exp-percent",
     .game_only = true,
     .label = "EXP percentage",
     .help = "Your EXP to the next level, as \"EXP 42.17%\" beside the HUD's bar",
     .applies = Applies::kAtOnce},
    // Controller extras (#172, ADR 0094, assists.h).
    {.key = "auto_run",
     .group = SettingGroup::kAssists,
     .comment =
         "# Assist: the auto-run key (R, the left stick's click) keeps the hero walking: on | "
         "off",
     .value = on_off<&Settings::auto_run>(),
     .presets = {"off", "off", "off"},
     .flag = "--auto-run",
     .game_only = true,
     .label = "Auto-run",
     .help = "Its key (R, L3) keeps the hero walking until pressed again, a menu, death or a new "
             "map. Attacks and potions wait meanwhile",
     .applies = Applies::kAtOnce},
    {.key = "hold_toggle",
     .group = SettingGroup::kAssists,
     .comment = "# Assist: a press starts these held actions and the next one stops them: off | "
                "walk\n# | fast_forward | all",
     .value = choice<&Settings::hold_toggle>(kHoldToggleNames),
     .presets = {"off", "off", "off"},
     .flag = "--hold-toggle",
     .game_only = true,
     .label = "Hold or toggle",
     .help = "Walk: a direction walks until pressed again (attacks and potions wait meanwhile). "
             "Fast-forward: its key turns it on and off",
     .applies = Applies::kAtOnce},
    // Auto-loot (#175, docs/re/qol.md#auto-loot).
    {.key = "auto_loot",
     .group = SettingGroup::kAssists,
     .comment = "# Assist: the hero picks up drops near it without walking over them: on | off",
     .value = on_off<&Settings::auto_loot>(),
     .presets = {"off", "off", "off"},
     .flag = "--auto-loot",
     .game_only = true,
     .label = "Auto-loot",
     .help = "Drops near the hero fly to it, as if it had walked over them",
     .applies = Applies::kAtOnce},
    // Save states (#204, ADR 0105's addendum, quick_state.h): read live by the turn hook.
    {.key = "save_states",
     .group = SettingGroup::kAssists,
     .comment = "# Assist: quick-save (F5) and quick-load (F9) a state of the game in the field,\n"
                "# one per character, beside the game's own saves: on | off",
     .value = on_off<&Settings::save_states>(),
     .presets = {"off", "off", "off"},
     .flag = "--save-states",
     .game_only = true,
     .label = "Save states",
     .help = "F5 saves the game as it is now, F9 goes back to it. Not the game's save: its next "
             "save writes what is loaded",
     .applies = Applies::kAtOnce},
    // #184 (disconnect.h): read live by game.cpp; never in a scripted run.
    {.key = "pause_on_disconnect",
     .group = SettingGroup::kControls,
     .comment =
         "# pause when the gamepad in use disconnects, until a pad connects or a key: on | off",
     .value = on_off<&Settings::pause_on_disconnect>(),
     .presets = {"off", "on", "on"},
     .flag = "--pause-on-disconnect",
     .game_only = true,
     .label = "Pause on disconnect",
     .help = "The game pauses when the gamepad in use disconnects, until it is back or a key",
     .applies = Applies::kAtOnce},
    // #138 (ADR 0128): read live by game.cpp's turn (Gamepads, Actions).
    {.key = "stick_mode",
     .group = SettingGroup::kControls,
     .comment =
         "# the left stick: 4way (the dominant axis; a ladder climbs even with the stick off\n"
         "# straight up) | 8way (a diagonal holds two directions)",
     .value = choice<&Settings::stick_mode>(kStickModeNames),
     .presets = {"4way", "4way", "4way"},
     .flag = "--stick-mode",
     .label = "Stick directions",
     .help = "4-way: the stick picks one direction, as a finger on the d-pad. 8-way: diagonals",
     .applies = Applies::kAtOnce},
    {.key = "direction_priority",
     .group = SettingGroup::kControls,
     .comment = "# two directions held in the field: game (its own order: left and right before\n"
                "# up and down) | last (the newest held direction replaces the older one)",
     .value = choice<&Settings::direction_priority>(kDirectionPriorityNames),
     .presets = {"game", "game", "last"},
     .flag = "--direction-priority",
     .label = "Direction priority",
     .help = "Two directions held: the game's own order, or the newest one wins (rolling keys)",
     .applies = Applies::kAtOnce},
    // The opt-in update notice (#199, ADR 0137, update_check.h): the port's only network use.
    {.key = "update_check",
     .group = SettingGroup::kOther,
     .comment = "# once a day, ask github.com whether a newer version of the port is out (never "
                "in\n# Original; it downloads nothing): on | off",
     .value = on_off<&Settings::update_check>(),
     .flag = "--update-check",
     .game_only = true,
     .label = "Check for new versions",
     .help = "Asks github.com once a day whether a newer version of the port is out; downloads "
             "nothing. Never in the Original preset"},
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
const char *pacing_mode_name(PacingMode mode) { return enum_name(mode, kPacingNames); }
const char *border_mode_name(BorderMode mode) { return enum_name(mode, kBorderModeNames); }
const char *colorblind_filter_name(ColorblindFilter filter) {
    return enum_name(filter, kColorblindFilterNames);
}

const char *game_speed_name(GameSpeed speed) { return enum_name(speed, kGameSpeedNames); }

const char *fast_forward_name(FastForward ff) { return enum_name(ff, kFastForwardNames); }

const char *frame_rate_name(FrameRate rate) { return enum_name(rate, kFrameRateNames); }
const char *ui_animations_name(UiAnimations a) { return enum_name(a, kUiAnimationsNames); }
double ui_animation_scale(UiAnimations a) {
    switch (a) {
    case UiAnimations::kNormal:
        break;
    case UiAnimations::kFast:
        return 0.25;
    case UiAnimations::kInstant:
        return 0.0;
    }
    return 1.0;
}
const char *auto_potion_name(AutoPotion a) { return enum_name(a, kAutoPotionNames); }
std::uint32_t auto_potion_percent(AutoPotion a) {
    switch (a) {
    case AutoPotion::kOff:
        break;
    case AutoPotion::k25:
        return 25;
    case AutoPotion::k50:
        return 50;
    case AutoPotion::k75:
        return 75;
    }
    return 0;
}
const char *pad_pointer_name(PadPointerMode m) { return enum_name(m, kPadPointerNames); }
const char *hold_toggle_name(HoldToggle h) { return enum_name(h, kHoldToggleNames); }
bool hold_toggle_walk(HoldToggle h) { return h == HoldToggle::kWalk || h == HoldToggle::kAll; }
bool hold_toggle_fast_forward(HoldToggle h) {
    return h == HoldToggle::kFastForward || h == HoldToggle::kAll;
}
bool parse_gfx_backend(std::string_view text, GfxBackend *out) {
    return parse_enum(text, kGfxBackendNames, out);
}
const char *texture_filter_name(TextureFilter filter) {
    return enum_name(filter, kTextureFilterNames);
}

std::uint32_t texture_filter_override(TextureFilter filter, std::uint32_t render_scale) {
    return filter == TextureFilter::kNearest && render_scale > 1 ? MT_GFX_NEAREST : 0u;
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

// The titles, summaries and group names are display text: the port's text table (#189).
const char *preset_title(Preset preset) {
    switch (preset) {
    case Preset::kOriginal:
        return tr(Txt::preset_original);
    case Preset::kOriginalController:
        return tr(Txt::preset_original_controller);
    case Preset::kEnhanced:
        return tr(Txt::preset_enhanced);
    case Preset::kCustom:
        break;
    }
    return tr(Txt::preset_custom);
}

const char *preset_summary(Preset preset) {
    switch (preset) {
    case Preset::kOriginal:
        return tr(Txt::preset_original_summary);
    case Preset::kOriginalController:
        return tr(Txt::preset_original_controller_summary);
    case Preset::kEnhanced:
        return tr(Txt::preset_enhanced_summary);
    case Preset::kCustom:
        break;
    }
    return tr(Txt::preset_custom_summary);
}

const char *setting_group_name(SettingGroup group) {
    switch (group) {
    case SettingGroup::kVideo:
        return tr(Txt::group_video);
    case SettingGroup::kAudio:
        return tr(Txt::group_audio);
    case SettingGroup::kControls:
        return tr(Txt::group_controls);
    case SettingGroup::kGameplay:
        return tr(Txt::group_gameplay);
    case SettingGroup::kAccessibility:
        return tr(Txt::group_accessibility);
    case SettingGroup::kAssists:
        return tr(Txt::group_assists);
    case SettingGroup::kSaves:
        return tr(Txt::group_saves);
    case SettingGroup::kOther:
        break;
    }
    return tr(Txt::group_other);
}

// --- The registry
// ---------------------------------------------------------------------------------

std::span<const SettingInfo> settings_registry() { return kRegistry; }

void set_audio_device_choices(std::vector<std::string> names) {
    audio_device_names() = std::move(names);
}

void set_display_choices(std::vector<std::string> names) { display_names() = std::move(names); }

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
        if (v.offer != nullptr) { // known only while the game runs (audio_device)
            out = v.offer();
            break;
        }
        out.assign(v.names.begin(), v.names.end()); // what the row offers, if anything
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
    long i = -1;
    for (long k = 0; k < n; k++) {
        if (choices[static_cast<std::size_t>(k)] == now) {
            i = k;
        }
    }
    if (i < 0) { // a value of its own (a colour, a point): to the first or the last offered
        i = delta >= 0 ? 0 : n - 1;
    } else {
        i = ((i + delta) % n + n) % n;
    }
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
        // The values a colour or a custom row offers the menu: each one a value as written.
        if (r.value.kind == ValueKind::kColor || r.value.kind == ValueKind::kCustom) {
            for (const char *offered : r.value.names) {
                Settings s;
                if (offered == nullptr || !r.value.parse(r.value, offered, &s) ||
                    setting_text(s, r) != offered) {
                    problems.push_back(at + "an offered value '" +
                                       (offered != nullptr ? offered : "") +
                                       "' that is not a value as the file writes it");
                }
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

const char *preset_column(const SettingInfo &info, Preset preset, SettingsPlatform platform) {
    const char *column = info.presets[preset];
    if (column == nullptr) {
        return nullptr;
    }
    const char *own = platform_value(info.key, platform);
    return own != nullptr ? own : column;
}

std::vector<std::string> check_platform_defaults(std::span<const PlatformDefault> rows) {
    std::vector<std::string> problems;
    for (std::size_t i = 0; i < rows.size(); i++) {
        const PlatformDefault &r = rows[i];
        const std::string key = r.key != nullptr ? r.key : "";
        const std::string at = "platform row " + std::to_string(i) + " (" + key + "): ";
        const SettingInfo *info = find_setting(key);
        if (info == nullptr) {
            problems.push_back(at + "no such setting");
            continue;
        }
        if (info->group == SettingGroup::kAssists) {
            problems.push_back(at + "an assist (a platform never turns one on)");
        }
        Settings s;
        if (r.value == nullptr || !info->value.parse(info->value, r.value, &s) ||
            setting_text(s, *info) != r.value) {
            problems.push_back(at + "'" + (r.value != nullptr ? r.value : "") +
                               "' is not a value as the file writes it");
        }
        if (r.why == nullptr || *r.why == '\0') {
            problems.push_back(at + "no reason");
        }
        for (std::size_t j = 0; j < i; j++) {
            if (rows[j].platform == r.platform && rows[j].key != nullptr && key == rows[j].key) {
                problems.push_back(at + "the key again for the same platform (row " +
                                   std::to_string(j) + ")");
            }
        }
    }
    return problems;
}

void apply_preset(Settings *settings, Preset preset) {
    for (const SettingInfo &info : kRegistry) {
        const char *column = preset_column(info, preset);
        if (column != nullptr) {
            info.value.parse(info.value, column, settings);
        }
    }
    settings->preset = preset;
}

std::vector<const SettingInfo *> preset_differences(const Settings &settings, Preset preset) {
    std::vector<const SettingInfo *> out;
    for (const SettingInfo &info : kRegistry) {
        const char *column = preset_column(info, preset);
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
    // So does any other row the value changed (fullscreen = desktop sets fullscreen_mode too,
    // #179).
    const SettingInfo *own = find_setting(key);
    if (changed.preset && *changed.preset != Preset::kCustom && !key_is(key, kPresetKey)) {
        for (const SettingInfo &info : kRegistry) {
            const char *column = preset_column(info, *changed.preset);
            if (column != nullptr && setting_text(changed, info) != column &&
                (&info == own || setting_text(changed, info) != setting_text(*settings, info))) {
                changed.preset = Preset::kCustom;
                break;
            }
        }
    }
    *settings = changed;
    return true;
}

bool apply_setting_list(Settings *settings, std::string_view list, std::string *why) {
    while (!list.empty()) {
        const std::size_t semi = list.find(';');
        const std::string_view part = trim(list.substr(0, semi));
        list.remove_prefix(semi == std::string_view::npos ? list.size() : semi + 1);
        if (part.empty()) {
            continue;
        }
        const std::size_t eq = part.find('=');
        std::string problem;
        if (eq == std::string_view::npos) {
            problem = "no '='";
        } else if (apply_setting(settings, trim(part.substr(0, eq)), trim(part.substr(eq + 1)),
                                 &problem)) {
            continue;
        }
        if (why != nullptr) {
            *why = "'" + std::string(part) + "': " + problem;
        }
        return false;
    }
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
        for (const char *frontend_key : kFrontendOnlyKeys) {
            if (key_is(key, frontend_key)) {
                return; // the frontend's own line (kFrontendOnlyKeys)
            }
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
                       setting_text(line_value, *info) != preset_column(*info, *file_preset)) {
                problems.push_back(at + info->key + " = " + printable(value) +
                                   ": preset = " + preset_name(*file_preset) + " sets " +
                                   preset_column(*info, *file_preset) +
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
