// The frontend's settings file (#59, ADR 0043): settings.ini in the data directory
// (MAPLETHIEF_DATA_DIR, else SDL_GetPrefPath("", "MapleThief")). Pure parsing, no SDL; game.cpp
// reads the file, main.cpp applies the command line over it.
//
//   # MapleThief settings
//   # integer | fit | smooth | sharp
//   scale_mode = integer
//   # the bars around the image
//   border_color = #000000
//   # the renderbuffers' internal scale, 1 to 8
//   render_scale = 1
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

struct Settings {
    ScaleMode scale_mode = ScaleMode::kInteger; // scale_mode, --scale-mode
    Color border_color;                         // border_color, --border-color (black)
    std::uint32_t render_scale = 1;             // render_scale, --render-scale (1 to 8)
};

// Sets one setting from its key and value (both already trimmed). Returns false, with `why`,
// for an unknown key or a bad value, leaving `settings` unchanged. The command-line overrides
// use it too.
bool apply_setting(Settings *settings, std::string_view key, std::string_view value,
                   std::string *why);

// Parses a settings file's text over `settings`. Returns the problems found, one message per
// ignored line ("line 3: unknown key 'scael_mode'"), in order; empty if there were none.
std::vector<std::string> parse_settings(std::string_view text, Settings *settings);

// Text for a log message: at most `max` bytes of `s`, bytes outside printable ASCII as \xNN,
// "..." if cut.
std::string printable(std::string_view s, std::size_t max = 48);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_SETTINGS_H
