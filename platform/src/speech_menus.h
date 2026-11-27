// The menus read aloud (#393, ADR 0190, docs/re/speech.md): what the focus or the selection
// lands on, as SpeechKind::kMenu utterances (speech.h, #390), each said once when it changes.
//
//   the port's settings menu   the row's label and value when the selected row or the tab
//                              changes ("tab: label, value" on a new tab or when it opens),
//                              the value alone when only it changes; nothing for a change of a
//                              speech row, which says its own sample in the new voice (#390)
//   the shell's UIKit screens  while the controller focus is active (focus.h, ADR 0051): the
//                              focused control's name, once per move. The shell's buttons are
//                              images without a title, so a button is named by its image
//                              (kShellButtons, port_text.def's button_* texts, which a text pack
//                              translates); else by the visible label it holds. A screen's labels
//                              that appear (a screen or an alert opens) are said before it, the
//                              first of them, as the screen's title.
//   the popup menu (panel 1)   the item, equipment or skill under the list's cursor (CDataMgr
//                              +0xD8, +0xDC, +0xD4, read_menu_spot): its name from the data
//                              tables, by id; its description when the game's own detail view
//                              opens on it (OK: the row's sub-list, +0xBC)
//   the shop (game mode 2)     the Buy or Sell row chosen: its item's name and price
//
// Nothing a player cannot see is said: a hidden label, a list the engine has not shown (the
// skill tab's list before its touch, #164), a settings menu that is closed. With speech off or
// no voice, nothing is read and the last thing said is forgotten, so turning speech on says
// where the focus is.
//
// Single thread: the frontend's.
#ifndef MAPLETHIEF_PLATFORM_SPEECH_MENUS_H
#define MAPLETHIEF_PLATFORM_SPEECH_MENUS_H

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "focus.h"
#include "maplethief/recomp/recomp.h"
#include "port_text.h"
#include "settings_menu.h"
#include "speech.h"

namespace maplethief::frontend {

// The name of each of the shell's image buttons, by the name its image was loaded by
// (FocusTarget::image, "AlertYesButton.png"; a Korean twin "_kor" before the extension shares
// its English twin's entry).
struct ShellButton {
    const char *image;
    Txt name;
};
std::span<const ShellButton> shell_buttons();
// The button's name for an image (shell_button_key's key), or nullptr if the table has none.
const char *shell_button_name(std::string_view image);
// The table's key of an image name: the file name without a folder, ".png" or "_kor".
std::string shell_button_key(std::string_view image);

// A visible label (maplethief/uikit.h mt_uikit_labels).
struct ScreenLabel {
    std::uint32_t object = 0;
    std::uint32_t order = 0;
    float x = 0, y = 0, w = 0, h = 0;
    std::string text; // UTF-8
};
// The labels the runtime sees now.
std::vector<ScreenLabel> runtime_labels(mt_cpu *cpu);

// What a focused control is called: its image's name, else the text of the frontmost label
// whose centre lies inside it, else "" (nothing is said of it).
std::string control_name(const FocusTarget &t, const std::vector<ScreenLabel> &labels);

// The settings menu's selected row as said: "label, value"; with `tab`, "tab: label, value".
std::string settings_menu_line(const SettingsMenu &menu, bool tab);

// What the popup menu's or the shop's cursor is on (read_menu_spot).
struct MenuSpot {
    enum class Kind : std::uint8_t { kNone, kItem, kEquip, kSkill, kShopBuy, kShopSell };
    Kind kind = Kind::kNone;
    std::int32_t row = -1;  // the list's row or cell
    std::int32_t id = -1;   // the item's id (item_etc or item_equip) or the skill's
    bool equip = false;     // the id is item_equip's (else item_etc's or a skill's)
    std::int32_t price = -1; // the shop's price, -1 when not known
    bool detail = false;    // the game's detail view of the row is open (CDataMgr +0xBC)
    bool operator==(const MenuSpot &) const = default;
};
const char *menu_spot_kind_name(MenuSpot::Kind kind); // "none", "item", "equip", ...
// Reads it from guest memory: the popup menu (game mode 1, CDataMgr at 0x11A984) and the shop
// (game mode 2, the shop at *(*0xF1454)). Kind kNone when no list's cursor is on a row.
MenuSpot read_menu_spot(mt_cpu *cpu);

// The name and description of a spot's item or skill from the data tables (UTF-8).
struct SpotText {
    std::string name, description;
};
SpotText spot_text(mt_cpu *cpu, const MenuSpot &spot);
// What is said of a spot: its name (with the shop's price, "name, 120 meso").
std::string menu_spot_line(const MenuSpot &spot, const SpotText &text);

class SpeechMenus {
  public:
    // The settings menu, after every event it may have taken (and when it opens or closes).
    void settings_turn(const SettingsMenu &menu, Speech &speech, double now);
    // Once per run-loop turn, after Focus::turn: the shell's focus (`labels` the visible
    // labels; read only while the focus is active) and the popup menu and the shop (`spot`).
    void shell_turn(const Focus &focus, const std::vector<ScreenLabel> &labels, Speech &speech,
                    double now);
    void spot_turn(const MenuSpot &spot, const SpotText &text, Speech &speech, double now);
    // Whether the frontend needs to read the labels and the spot this turn (speech on, with a
    // voice): the reads cost nothing otherwise.
    static bool listening(const Speech &speech) { return speech.enabled() && speech.available(); }

    struct Stats {
        std::uint64_t settings = 0; // settings menu utterances
        std::uint64_t controls = 0; // focused controls said
        std::uint64_t unnamed = 0;  // focused controls without a name (not said)
        std::uint64_t titles = 0;   // screen titles said
        std::uint64_t spots = 0;    // popup menu and shop rows said
        std::uint64_t details = 0;  // descriptions said
    };
    const Stats &stats() const { return stats_; }

  private:
    void say(Speech &speech, std::string text, double now);

    // The settings menu as last said.
    bool menu_open_ = false;
    std::size_t menu_tab_ = 0, menu_row_ = 0;
    std::string menu_label_, menu_value_;
    // The shell.
    std::uint32_t control_ = 0; // the focused control last said (0: none)
    std::string control_action_;
    std::vector<std::uint32_t> labels_; // the labels seen last turn
    // The popup menu and the shop.
    MenuSpot spot_;
    Stats stats_;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_SPEECH_MENUS_H
