// The packs screen (#370, ADR 0149): the settings menu's list of the texture, audio and text
// packs, on every platform, and the checks the start applies to the same lists.
//
//   where     the data folder's pack folders (pack_folders.h: <base>/texture-packs/,
//             audio-packs/, text-packs/, made if missing), plus any folder a setting lists by
//             its path (settings.ini or the command line)
//   a row     per pack: its manifest's name and version (maplethief/pack_manifest.h; a bare
//             folder: its folder's name), and "on, 1st" / "off" / "refused"; the help lines give
//             its author and description, or why the port refuses it. A kind without packs has
//             one row that says where they go.
//   order     enabled packs first, in their setting's order (the first wins, entry by entry:
//             ADR 0149), then the others by name
//   help      while the game runs with the pack open: what it replaced and what an earlier pack
//             hid this run (#421)
//   choose    (South, Enter, Space, a click or tap) turns a pack on (last in its kind's order)
//             or off; a refused pack is not turned on, and the status line says why
//   Left / Right (the d-pad, the stick, the arrows, the row's "<" / ">")
//             move an enabled pack earlier or later in its kind's order
//
// Each kind's setting (texture_pack, audio_pack, text_pack) holds its enabled packs as an
// ordered list, '|' between them, first first (ADR 0149); a single path is a list of one. The
// menu writes it through SettingsMenu::change_setting, so it is saved with the menu's changes.
// A pack is refused (pack_list_check) when its manifest is (ADR 0149's rules), when its folder's
// path holds '|', when an earlier pack of the list has its id, past the 16th of a kind, and when
// a text pack's language differs from the first text pack's.
//
// The start (game.cpp) opens every accepted pack of each list in its order (#421: the runtime
// asks them entry by entry, the first wins); refused ones are logged and shown on the title
// screen (data_notice.h). At the end, the exit log has a line per open pack with what it
// replaced and what an earlier pack hid (log_pack_uses). The command-line flags of the three
// settings may be given more than once (add_setting_flag): their values join into one list, in
// order. Single thread: the frontend's.
#ifndef MAPLETHIEF_PLATFORM_PACKS_H
#define MAPLETHIEF_PLATFORM_PACKS_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "settings.h"
#include "settings_menu.h"

namespace maplethief::frontend {

enum class PackKind : std::uint8_t { kTexture, kAudio, kText };
inline constexpr PackKind kPackKinds[] = {PackKind::kTexture, PackKind::kAudio, PackKind::kText};
inline constexpr std::size_t kMaxPacksPerKind = 16;

const char *pack_kind_name(PackKind kind);   // "texture", "audio", "text": the manifest's kind
const char *pack_kind_key(PackKind kind);    // "texture_pack", ...: its setting
const char *pack_kind_folder(PackKind kind); // "texture-packs", ...: its pack folder
std::string &pack_kind_value(Settings &s, PackKind kind);
const std::string &pack_kind_value(const Settings &s, PackKind kind);
// The kind whose setting is `key`; nullopt for any other row.
std::optional<PackKind> pack_kind_of(std::string_view key);

// A setting's value as its list of folders, empty parts dropped; and back.
std::vector<std::string> split_pack_list(std::string_view value);
std::string join_pack_list(const std::vector<std::string> &paths);

// What a pack says of itself and whether the port takes it.
struct PackInfo {
    std::string path;
    std::string id, name, version, author, description, language, content;
    std::string refused; // why the port does not take it; "" when it does
};
// The pack at `path` read as `kind` for `port_version` (MT_VERSION) by its manifest alone.
PackInfo read_pack(const std::string &path, PackKind kind, const std::string &port_version);
// The packs of one setting's list, in order, each with its manifest's verdict and then the list's
// own rules: a path with '|', an id an earlier pack has, the 17th and later, a text pack whose
// language differs from the first accepted text pack's.
std::vector<PackInfo> check_pack_list(const std::vector<std::string> &paths, PackKind kind,
                                      const std::string &port_version);

// The packs of `kind` the start opens, in order: those of its setting's list that
// check_pack_list accepts. Each refused pack is logged and queued for the title screen with its
// reason (data_notice.h).
std::vector<std::string> packs_to_open(const Settings &s, PackKind kind,
                                       const std::string &port_version);
// Opens `paths` of `kind` in the runtime in order (mt_texpack_add, mt_audio_pack_add,
// mt_textpack_add), after closing the packs open before. Returns the paths that did not open
// (the runtime logged why).
std::vector<std::string> open_packs(PackKind kind, const std::vector<std::string> &paths);

// What one open pack did this run (maplethief/pack_manifest.h's mt_pack_use), with titles.
struct PackUse {
    std::string path, title;
    std::uint64_t replaced = 0, rejected = 0, hidden = 0;
    std::vector<std::pair<std::string, std::uint64_t>> hidden_by; // earlier packs' titles, > 0
};
// The packs of `kind` open in the runtime now, in order, titled by their manifests.
std::vector<PackUse> open_pack_uses(PackKind kind, const std::string &port_version);
// "812 replaced, 40 hidden by UI Redraw" (several: "40 hidden (32 by A, 8 by B)"), then
// ", 2 rejected" when some were.
std::string pack_use_counts(const PackUse &u);
// The exit log's line: "texpack: HD Sprites 1.2: 812 replaced, 40 hidden by UI Redraw".
std::string pack_use_line(PackKind kind, const PackUse &u);
// Logs pack_use_line for every pack of `kind` open now.
void log_pack_uses(PackKind kind, const std::string &port_version);

// A setting's command-line flag into `flags` (key, value); a pack setting's flag given again
// adds its value to the same entry's list ('|'), so --texture-pack A --texture-pack B is A|B.
void add_setting_flag(std::vector<std::pair<std::string, std::string>> &flags,
                      const std::string &key, const std::string &value);

// "HD Sprites 1.2": the name, then the version if any.
std::string pack_title(const PackInfo &p);
// A pack row's value in the settings menu: "none", the first pack's name, "HD + 2" for more.
std::string pack_list_summary(std::string_view value, const char *none);

// The packs screen's state: the packs of each kind, which are on and in what order.
class PacksScreen {
  public:
    // `base`: the data folder (data_location().base; "" lists only the settings' paths).
    PacksScreen(std::string base, std::string port_version);

    // Where the help lines read what the open packs did this run (open_pack_uses unless set).
    using Uses = std::function<std::vector<PackUse>(PackKind)>;
    void set_uses(Uses uses) { uses_ = std::move(uses); }

    // Lists the pack folders and the settings' lists again.
    void scan(const Settings &s);

    struct Row {
        PackKind kind;
        std::size_t pack = 0; // in packs(kind)
        bool empty = false;   // the kind's "none found" row
    };
    const std::vector<Row> &rows() const { return rows_; }
    // The first row of `kind`.
    std::size_t first_row(PackKind kind) const;
    std::vector<MenuListRow> list_rows() const;
    std::string row_help(std::size_t row) const;

    struct Entry {
        PackInfo info;
        bool enabled = false;
    };
    const std::vector<Entry> &packs(PackKind kind) const {
        return kinds_[static_cast<std::size_t>(kind)];
    }

    // A change for the menu to apply (SettingsMenu::change_setting), the status line, and the row
    // the pack is on afterwards.
    struct Change {
        std::string key, value; // "" key: nothing to apply
        std::string status;
        std::size_t row = 0;
    };
    // Turns the pack on `row` on or off. Then `scan` again with the changed settings.
    Change toggle(std::size_t row) const;
    // Moves the enabled pack on `row` earlier (-1) or later (+1) in its kind's order.
    Change move(std::size_t row, int delta) const;

  private:
    std::string base_, port_;
    Uses uses_;
    std::vector<Entry> kinds_[3];
    std::vector<Row> rows_;
};

// Applies a change to the settings (SettingsMenu::change_setting) and returns them afterwards;
// nullptr if the value was refused.
using PacksApply =
    std::function<const Settings *(const std::string &key, const std::string &value)>;
// The settings menu's list for the packs screen (SettingsMenu::open_list), opened at `kind`'s
// first row: its rows, help, toggle and move go through `screen`, its changes through `apply`.
MenuList packs_menu_list(std::shared_ptr<PacksScreen> screen, PackKind kind, PacksApply apply);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_PACKS_H
