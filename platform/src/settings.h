// The frontend's settings file (#59, ADR 0043; the audio keys #64; hide_keypad #71; rumble and
// rumble_strength #72; gfx_backend #120; filter and hd_text #88, ADR 0066; gapless_music #125;
// perf_overlay #137, ADR 0071):
// settings.ini in the data directory
// (MAPLETHIEF_DATA_DIR, else SDL_GetPrefPath("", "MapleThief")). Pure parsing and writing, no SDL;
// game.cpp reads and writes the file, main.cpp applies the command line over it.
//
//   # MapleThief settings
//   # integer | fit | smooth | sharp
//   scale_mode = integer
//   # the bars around the image
//   border_color = #000000
//   # the renderbuffers' internal scale, 1 to 8
//   render_scale = 1
//   # what draws the game's GL frames: software (the reference) | gpu (SDL3 GPU)
//   gfx_backend = software
//   # a shader filter over the scaled image: none | xbr | crt | lcd
//   filter = none
//   # draw the game's text textures at the render scale: on | off
//   hd_text = off
//   # volume in percent, 0 to 100: everything, the music, the sound effects
//   master_volume = 100
//   music_volume = 100
//   effects_volume = 100
//   # silence while the window is not focused: on | off
//   mute_unfocused = on
//   # loop the music without the silence at the start and end of its files: on | off (off:
//   # as the device)
//   gapless_music = off
//   # the right mouse button: a touch at screen point X,Y, or off
//   right_touch = off
//   # hide the on-screen key pad: auto (while a gamepad or the keyboard is used) | always |
//   # never
//   hide_keypad = auto
//   # the gamepad rumbles on a vibration, a hit and a level-up: on | off
//   rumble = on
//   # how strongly, 0 to 100 (percent)
//   rumble_strength = 100
//   # the frame-time overlay (F3 shows or hides it while the game runs): on | off
//   perf_overlay = off
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
#ifndef MAPLETHIEF_PLATFORM_SETTINGS_H
#define MAPLETHIEF_PLATFORM_SETTINGS_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
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

struct Settings {
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
    // The frame-time overlay (#137, perf_overlay.h): shown from the start; F3 toggles it.
    bool perf_overlay = false; // perf_overlay, --perf-overlay (on | off)

    bool operator==(const Settings &) const = default;
};

inline constexpr std::uint32_t kMaxVolume = 100;
inline constexpr std::uint32_t kMaxRumbleStrength = 100;

// Sets one setting from its key and value (both already trimmed). Returns false, with `why`,
// for an unknown key or a bad value, leaving `settings` unchanged. The command-line overrides
// use it too.
bool apply_setting(Settings *settings, std::string_view key, std::string_view value,
                   std::string *why);

// Parses a settings file's text over `settings`. Returns the problems found, one message per
// ignored line ("line 3: unknown key 'scael_mode'"), in order; empty if there were none.
std::vector<std::string> parse_settings(std::string_view text, Settings *settings);

// The file's text for `settings`. With `old_text` (the file as it is) empty: every key with a
// comment line, in the order above. Otherwise `old_text` is kept byte for byte (comments,
// unknown keys, malformed lines, a byte order mark, its line endings), except that the value of
// each line that sets a known key is replaced by the setting's value, and the keys it lacks are
// appended, each after its comment line. parse_settings(serialise_settings(s, t)) gives s for
// any t; an old text over kMaxSettingsBytes (which parse_settings ignores) is replaced whole.
std::string serialise_settings(const Settings &settings, std::string_view old_text = {});

// Text for a log message: at most `max` bytes of `s`, bytes outside printable ASCII as \xNN,
// "..." if cut.
std::string printable(std::string_view s, std::size_t max = 48);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_SETTINGS_H
