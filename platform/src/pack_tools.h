// The pack tools (#371, ADR 0205): making and checking packs from the game itself, so that a
// player with only the download can do what the repository's Python tools do. The work is the
// runtime's (maplethief/packcheck.h, texpack.h), with its loaders' own limits; this is the
// frontend's side: the folders, the port's own text, the report's words, the menu's rows and
// the command line.
//
// Where the tools write, beside game/ (ADR 0149's pack folders; DataLocation::bundle_parent()):
//
//   <parent>/texture-dumps/dump/        Dump textures (the menu) writes here
//   <parent>/texture-packs/dump-<k>x/   Make texture pack builds the dump k times larger here
//   <parent>/text-packs/text-<lang>/    Export text writes the game's and the port's text here
//
// Check packs looks at every folder the pack rows name (texture_pack, audio_pack, text_pack;
// ADR 0149's '|' lists too) and every pack in the three pack folders.
//
// Single thread: the frontend's, while the game is paused (the menu) or before it boots (the
// command line).
#ifndef MAPLETHIEF_PLATFORM_PACK_TOOLS_H
#define MAPLETHIEF_PLATFORM_PACK_TOOLS_H

#include <cstdint>
#include <string>
#include <vector>

#include "maplethief/packcheck.h"
#include "settings.h"
#include "settings_menu.h"

namespace maplethief::frontend {

inline constexpr const char *kTextureDumpsDir = "texture-dumps";
inline constexpr unsigned kMenuPackScale = 2; // Make texture pack's factor

struct PackToolPlaces {
    std::string dump;        // <parent>/texture-dumps/dump
    std::string built_pack;  // <parent>/texture-packs/dump-2x
    std::string text_export; // <parent>/text-packs/text-<lang>
};
// "" `parent`: every place "".
PackToolPlaces pack_tool_places(const std::string &parent, Language language);

// What one tool said: its lines (level, text) and the runtime's report.
struct PackToolLine {
    int level = MT_PACKCHECK_NOTE; // mt_packcheck_level
    std::string text;
};
struct PackToolResult {
    mt_packcheck_report report{};
    std::vector<PackToolLine> lines;
    int count = 0; // the build's files or the export's entries; -1: it did not start
};

// The pack check (kind "" tells it from the folder). `bundle`: the game's MapleThief.app ("":
// not at hand).
PackToolResult check_pack(const std::string &dir, const std::string &bundle, const char *kind = "");
// The texture pack builder (mt_texpack_build).
PackToolResult build_texture_pack(const std::string &dump, const std::string &pack, unsigned k,
                                  int scaler, bool ui);
// The text export: the runtime's files (mt_textpack_export), then Port.strings, the port's own
// text in English.
PackToolResult export_text(const std::string &bundle, Language language, const std::string &dir);

// Port.strings for the export: every key of the port's text (port_text.def, the registry's
// labels and help, the menu's action rows) with its English.
std::string port_strings_text();
// Hands the port's text to the runtime's text check (mt_packcheck_set_port_text): once.
void install_port_text_check();

// The report as text, one line each: "refused: ...", "rejected: ...", "ignored: ...",
// "note: ...", then "<dir>: <kind> pack: <verdict>".
std::string pack_tool_text(const std::string &dir, const PackToolResult &r);
// The verdict alone: "opens, 812 usable" / "opens, 810 usable, 2 rejected" / "refused".
std::string pack_check_verdict(const PackToolResult &r);

// The packs Check packs looks at: (kind, folder), the rows' first, then each pack folder's
// entries, each folder once.
struct PackToCheck {
    std::string kind; // "texture", "audio", "text"
    std::string dir;
};
std::vector<PackToCheck> packs_to_check(const Settings &s, const std::string &parent);

// The menu's actions (their status lines). Dump textures turns the dump on and off; the others
// run at once, the game paused. `bundle`: the game's MapleThief.app.
std::string dump_textures_action(const std::string &dir);
bool dumping_textures();
std::string build_texture_pack_action(const std::string &dump, const std::string &pack);
std::string export_text_action(const std::string &bundle, Language language,
                               const std::string &dir);
// Check packs: a list of packs_to_check with each verdict (the whole reports to the log);
// choosing a row shows its first problem. No rows: nothing to check.
MenuList check_packs_list(const Settings &s, const std::string &parent, const std::string &bundle);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_PACK_TOOLS_H
