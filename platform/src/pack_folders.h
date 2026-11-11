// The pack folders (#241, ADR 0142): where a platform without file dialogs or a typed path
// (platform_caps.h's kCapPackFolders: the Nintendo Switch) keeps the packs and border images
// the settings menu offers. Each is a folder of its own in <base>, beside game/ and outside
// data/ (packs are large and made from the player's own copy, as game/ is: ADR 0127):
//
//   <base>/texture-packs/<name>/   texture_pack (ADR 0081): a pack's folder
//   <base>/audio-packs/<name>/     audio_pack (ADR 0106)
//   <base>/text-packs/<name>/      text_pack (ADR 0108)
//   <base>/borders/<name>.png      border_image (ADR 0085)
//
// On the Switch <base> is sdmc:/switch/maplethief (data_location.h's kSwitchBaseDir). The menu
// offers "" (none) and every entry by its whole path, sorted by name, and shows a value by its
// last part (pack_value_text). Elsewhere nothing is listed: the rows keep their values as before.
#ifndef MAPLETHIEF_PLATFORM_PACK_FOLDERS_H
#define MAPLETHIEF_PLATFORM_PACK_FOLDERS_H

#include <string>
#include <string_view>
#include <vector>

namespace maplethief::frontend {

struct PackFolder {
    const char *key;    // the registry row
    const char *folder; // its folder's name in <base>
    const char *ext;    // nullptr: the folder's subfolders; else its files with this ending
};
inline constexpr PackFolder kPackFolders[] = {
    {"texture_pack", "texture-packs", nullptr},
    {"audio_pack", "audio-packs", nullptr},
    {"text_pack", "text-packs", nullptr},
    {"border_image", "borders", ".png"},
};

// The pack folder of a row's key; nullptr for any other row.
const PackFolder *find_pack_folder(std::string_view key);

// What `dir` holds for `f`: its subfolders, or its files ending in f.ext (any case), as whole
// paths (dir + '/' + name), sorted by name; hidden entries (a leading '.') left out. Empty if
// `dir` is missing or unreadable.
std::vector<std::string> list_pack_folder(const std::string &dir, const PackFolder &f);

// Lists each pack folder of `base` (made if missing, so the player sees where packs go) and
// gives the rows their choices (settings.h's set_pack_choices). "" `base`: none.
void offer_pack_folders(const std::string &base);

// A pack row's value as the menu shows it: "none" for "", else the path's last part.
std::string pack_value_text(std::string_view value, const char *none);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_PACK_FOLDERS_H
