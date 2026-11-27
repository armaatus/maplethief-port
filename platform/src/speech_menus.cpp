// The menus read aloud (#393, ADR 0190); see speech_menus.h.
#include "speech_menus.h"

#include <algorithm>
#include <cstring>

#include "maplethief/text_events.h"
#include "maplethief/uikit.h"

namespace maplethief::frontend {

namespace {

using u32 = std::uint32_t;
using i32 = std::int32_t;

// The shell's image buttons a tap reaches (the focus's targets on the main menu, the terms,
// About, the character select and their alerts; `input.speech_menus` lists every image it met).
constexpr ShellButton kShellButtons[] = {
    {"AlertNoButton", Txt::button_no},
    {"AlertOKButton", Txt::button_ok},
    {"AlertYesButton", Txt::button_yes},
    {"ChooseCharacterBackButton", Txt::button_back},
};

// --- guest reads (docs/re/speech.md, "The menus") -----------------------------------------------

constexpr u32 kCanvas = 0x11A950;     // SkyCanvas*: +0xC the engine's state
constexpr u32 kGameVptr = 0x119B78;   // a CStateGame's vtable pointer
constexpr u32 kGameMode = 0x11AA00;   // 1 the popup menu, 2 a shop
constexpr u32 kDataMgr = 0x11A984;    // CDataMgr*
constexpr u32 kShopHolder = 0xF1454;  // -> -> the shop (Shop_OnKey's object)
constexpr u32 kStateHero = 0x30;      // CStateGame: the hero
constexpr u32 kHeroEquips = 0xD4;     // CHero: 48 equipment items, packed from the front
constexpr u32 kHeroItems = 0xD8;      // CHero: 48 etc items, packed from the front
constexpr u32 kHeroSkills = 0x114;    // CHero: the skills; slot i id +0x08 + 8 i
constexpr u32 kItemId = 0x90;         // an item: its id in its table
constexpr u32 kItemPrice = 0x98;      // a shop's Buy row: its price
constexpr u32 kItemSellPrice = 0xA4;  // a carried item: what a shop pays for it
constexpr i32 kMaxItems = 48;
constexpr i32 kMaxSkills = 13;
// CDataMgr: the open tab (1 items, 2 equipment, 3 skills), the quest page, the cursors, the
// row's sub-list (the detail: use, equip, level up ...), the keys' scroll.
constexpr u32 kMgrOpen = 0xA8, kMgrPage = 0xAC, kMgrDetail = 0xBC, kMgrSkillRow = 0xD4,
              kMgrItemCell = 0xD8, kMgrEquipCell = 0xDC, kMgrScroll = 0x10C;
// The shop: its tab (0 Buy, 1 Sell), each tab's chosen row (-1 none), its kind (0 equipment,
// 1 etc items), the Buy rows and their count.
constexpr u32 kShopTab = 0x5C, kShopRow = 0x60, kShopKind = 0x90, kShopRows = 0x94,
              kShopCount = 0x98;
// The data tables' text fields (docs/formats/data-tables.md): offset, width.
constexpr u32 kEquipName = 1, kEquipNameW = 32;
constexpr u32 kEtcName = 1, kEtcNameW = 64, kEtcDesc = 90, kEtcDescW = 128;
constexpr u32 kSkillName = 1, kSkillNameW = 16, kSkillDesc = 41, kSkillDescW = 128;

bool mapped(const mt_cpu *cpu, u32 addr, u32 n) {
    return cpu != nullptr && cpu->mem != nullptr && addr != 0 &&
           addr + std::uint64_t{n} <= cpu->mem_size;
}
u32 rd32(const mt_cpu *cpu, u32 addr) {
    u32 v = 0;
    if (mapped(cpu, addr, 4)) {
        std::memcpy(&v, cpu->mem + addr, 4);
    }
    return v;
}
i32 rdi(const mt_cpu *cpu, u32 addr) { return static_cast<i32>(rd32(cpu, addr)); }
std::uint8_t rd8(const mt_cpu *cpu, u32 addr) { return mapped(cpu, addr, 1) ? cpu->mem[addr] : 0; }

// The hero of the engine's CStateGame, or 0.
u32 game_hero(const mt_cpu *cpu) {
    const u32 state = rd32(cpu, rd32(cpu, kCanvas) + 0x0C);
    return state != 0 && rd32(cpu, state) == kGameVptr ? rd32(cpu, state + kStateHero) : 0u;
}

// Item `n` of one of the hero's lists (0 past its end).
u32 hero_item(const mt_cpu *cpu, u32 hero, u32 list, i32 n) {
    const u32 items = hero != 0 ? rd32(cpu, hero + list) : 0u;
    return items != 0 && n >= 0 && n < kMaxItems ? rd32(cpu, items + 4 * static_cast<u32>(n))
                                                 : 0u;
}

std::string field(mt_cpu *cpu, u32 table, i32 id, u32 offset, u32 width) {
    char buf[512];
    return mt_text_table_field(cpu, table, id, offset, width, buf, sizeof buf) != 0
               ? std::string(buf)
               : std::string();
}

} // namespace

std::span<const ShellButton> shell_buttons() { return kShellButtons; }

std::string shell_button_key(std::string_view image) {
    if (const std::size_t slash = image.rfind('/'); slash != std::string_view::npos) {
        image.remove_prefix(slash + 1);
    }
    for (const std::string_view suffix : {std::string_view(".png"), std::string_view("_kor")}) {
        if (image.size() > suffix.size() && image.substr(image.size() - suffix.size()) == suffix) {
            image.remove_suffix(suffix.size());
        }
    }
    return std::string(image);
}

const char *shell_button_name(std::string_view image) {
    const std::string key = shell_button_key(image);
    for (const ShellButton &b : kShellButtons) {
        if (key == b.image) {
            return tr(b.name);
        }
    }
    return nullptr;
}

std::vector<ScreenLabel> runtime_labels(mt_cpu *cpu) {
    std::vector<ScreenLabel> out;
    if (cpu == nullptr) {
        return out;
    }
    std::vector<mt_uikit_label> l(16);
    u32 n = mt_uikit_labels(cpu, l.data(), static_cast<u32>(l.size()));
    if (n > l.size()) {
        l.resize(n);
        n = mt_uikit_labels(cpu, l.data(), n);
    }
    for (u32 i = 0; i < n && i < l.size(); i++) {
        out.push_back(ScreenLabel{l[i].object, l[i].order, l[i].rect[0], l[i].rect[1],
                                  l[i].rect[2], l[i].rect[3], l[i].text});
    }
    return out;
}

std::string control_name(const FocusTarget &t, const std::vector<ScreenLabel> &labels) {
    if (const char *name = shell_button_name(t.image); name != nullptr && *name != 0) {
        return name;
    }
    const ScreenLabel *best = nullptr;
    for (const ScreenLabel &l : labels) {
        const float cx = l.x + l.w / 2, cy = l.y + l.h / 2;
        if (cx >= t.x && cx < t.x + t.w && cy >= t.y && cy < t.y + t.h &&
            (best == nullptr || l.order > best->order)) {
            best = &l;
        }
    }
    return best != nullptr ? best->text : std::string();
}

std::string settings_menu_line(const SettingsMenu &menu, bool tab) {
    const std::size_t row = menu.selected();
    const std::string line =
        tr(Txt::speech_row, {menu.row_label(row), menu.row_value(row)});
    if (!tab || menu.tab() >= menu.tabs().size()) {
        return line;
    }
    return tr(Txt::speech_speaker, {setting_group_name(menu.tabs()[menu.tab()]), line});
}

const char *menu_spot_kind_name(MenuSpot::Kind kind) {
    switch (kind) {
    case MenuSpot::Kind::kNone:
        return "none";
    case MenuSpot::Kind::kItem:
        return "item";
    case MenuSpot::Kind::kEquip:
        return "equip";
    case MenuSpot::Kind::kSkill:
        return "skill";
    case MenuSpot::Kind::kShopBuy:
        return "shop buy";
    case MenuSpot::Kind::kShopSell:
        return "shop sell";
    }
    return "?";
}

MenuSpot read_menu_spot(mt_cpu *cpu) {
    MenuSpot s;
    const u32 hero = game_hero(cpu);
    if (hero == 0) {
        return s;
    }
    const i32 mode = rdi(cpu, kGameMode);
    if (mode == 1) {
        const u32 mgr = rd32(cpu, kDataMgr);
        const i32 open = rdi(cpu, mgr + kMgrOpen);
        if (mgr == 0 || rdi(cpu, mgr + kMgrPage) != 0 || popup_menu_list_hidden(cpu, open)) {
            return s;
        }
        s.detail = rd8(cpu, mgr + kMgrDetail) != 0;
        if (open == 1 || open == 2) {
            s.row = rdi(cpu, mgr + (open == 1 ? kMgrItemCell : kMgrEquipCell));
            const u32 item = hero_item(cpu, hero, open == 1 ? kHeroItems : kHeroEquips, s.row);
            if (item != 0) {
                s.kind = open == 1 ? MenuSpot::Kind::kItem : MenuSpot::Kind::kEquip;
                s.equip = open == 2;
                s.id = rdi(cpu, item + kItemId);
            }
        } else if (open == 3) {
            const u32 scroll = rd32(cpu, mgr + kMgrScroll);
            s.row = rdi(cpu, mgr + kMgrSkillRow) + (scroll != 0 ? rdi(cpu, scroll + 4) : 0);
            const u32 skills = rd32(cpu, hero + kHeroSkills);
            const i32 id = skills != 0 && s.row >= 0 && s.row < kMaxSkills
                               ? rdi(cpu, skills + 0x08 + 8 * static_cast<u32>(s.row))
                               : -1;
            if (id >= 0) {
                s.kind = MenuSpot::Kind::kSkill;
                s.id = id;
            }
        }
        if (s.kind == MenuSpot::Kind::kNone) {
            return MenuSpot{};
        }
        return s;
    }
    if (mode == 2) {
        const u32 shop = rd32(cpu, rd32(cpu, kShopHolder));
        const i32 tab = rdi(cpu, shop + kShopTab);
        if (shop == 0 || (tab != 0 && tab != 1)) {
            return s;
        }
        s.row = rdi(cpu, shop + kShopRow + 4 * static_cast<u32>(tab));
        s.equip = rdi(cpu, shop + kShopKind) == 0;
        u32 item = 0;
        if (tab == 0) {
            const u32 rows = rd32(cpu, shop + kShopRows);
            const i32 count = std::min(rdi(cpu, shop + kShopCount), 1000);
            item = rows != 0 && s.row >= 0 && s.row < count
                       ? rd32(cpu, rows + 4 * static_cast<u32>(s.row))
                       : 0u;
        } else {
            item = hero_item(cpu, hero, s.equip ? kHeroEquips : kHeroItems, s.row);
        }
        if (item == 0) {
            return MenuSpot{};
        }
        s.kind = tab == 0 ? MenuSpot::Kind::kShopBuy : MenuSpot::Kind::kShopSell;
        s.id = rdi(cpu, item + kItemId);
        s.price = rdi(cpu, item + (tab == 0 ? kItemPrice : kItemSellPrice));
        return s;
    }
    return s;
}

SpotText spot_text(mt_cpu *cpu, const MenuSpot &spot) {
    SpotText t;
    switch (spot.kind) {
    case MenuSpot::Kind::kNone:
        break;
    case MenuSpot::Kind::kSkill:
        t.name = field(cpu, MT_TEXT_SKILL_TABLE, spot.id, kSkillName, kSkillNameW);
        t.description = field(cpu, MT_TEXT_SKILL_TABLE, spot.id, kSkillDesc, kSkillDescW);
        break;
    default:
        if (spot.equip) {
            t.name = field(cpu, MT_TEXT_EQUIP_TABLE, spot.id, kEquipName, kEquipNameW);
        } else {
            t.name = field(cpu, MT_TEXT_ETC_TABLE, spot.id, kEtcName, kEtcNameW);
            t.description = field(cpu, MT_TEXT_ETC_TABLE, spot.id, kEtcDesc, kEtcDescW);
        }
        break;
    }
    return t;
}

std::string menu_spot_line(const MenuSpot &spot, const SpotText &text) {
    if (text.name.empty()) {
        return {};
    }
    const bool shop = spot.kind == MenuSpot::Kind::kShopBuy || spot.kind == MenuSpot::Kind::kShopSell;
    if (shop && spot.price >= 0) {
        return tr(Txt::speech_price, {text.name, std::to_string(spot.price)});
    }
    return text.name;
}

void SpeechMenus::say(Speech &speech, std::string text, double now) {
    if (!text.empty()) {
        speech.say(SpeechKind::kMenu, std::move(text), now);
    }
}

void SpeechMenus::settings_turn(const SettingsMenu &menu, Speech &speech, double now) {
    if (!menu.is_open() || !listening(speech) || menu.selected() >= menu.items().size()) {
        menu_open_ = false;
        return;
    }
    const std::size_t tab = menu.tab(), row = menu.selected();
    std::string label = menu.row_label(row), value = menu.row_value(row);
    const bool moved = !menu_open_ || tab != menu_tab_ || row != menu_row_;
    if (moved || label != menu_label_) {
        stats_.settings++;
        say(speech, settings_menu_line(menu, !menu_open_ || tab != menu_tab_), now);
    } else if (value != menu_value_) {
        // A speech row says its sample in the new voice (live_settings.cpp): not cut off here.
        const SettingInfo *s = menu.items()[row].setting;
        if (s == nullptr || std::string_view(s->key).substr(0, 6) != "speech") {
            stats_.settings++;
            say(speech, value, now);
        }
    }
    menu_open_ = true;
    menu_tab_ = tab;
    menu_row_ = row;
    menu_label_ = std::move(label);
    menu_value_ = std::move(value);
}

void SpeechMenus::shell_turn(const Focus &focus, const std::vector<ScreenLabel> &labels,
                             Speech &speech, double now) {
    if (!focus.active() || !listening(speech)) {
        control_ = 0;
        control_action_.clear();
        labels_.clear();
        return;
    }
    // The first label that appeared since the last turn: a screen's or an alert's title.
    const ScreenLabel *title = nullptr;
    std::vector<std::uint32_t> seen;
    seen.reserve(labels.size());
    for (const ScreenLabel &l : labels) {
        seen.push_back(l.object);
        if (std::find(labels_.begin(), labels_.end(), l.object) == labels_.end() &&
            (title == nullptr || l.order < title->order)) {
            title = &l;
        }
    }
    labels_ = std::move(seen);
    const FocusTarget *t = focus.focused();
    std::string name;
    if (t != nullptr && (t->object != control_ || t->action != control_action_)) {
        control_ = t->object;
        control_action_ = t->action;
        name = control_name(*t, labels);
        if (name.empty()) {
            stats_.unnamed++;
        } else {
            stats_.controls++;
        }
    }
    std::string line;
    if (title != nullptr && title->text != name) {
        stats_.titles++;
        line = title->text;
    }
    if (!name.empty()) {
        line = line.empty() ? name : tr(Txt::speech_then, {line, name});
    }
    say(speech, std::move(line), now);
}

void SpeechMenus::spot_turn(const MenuSpot &spot, const SpotText &text, Speech &speech,
                            double now) {
    if (!listening(speech)) {
        spot_ = MenuSpot{};
        return;
    }
    const bool same_row = spot.kind == spot_.kind && spot.row == spot_.row && spot.id == spot_.id;
    if (spot.kind != MenuSpot::Kind::kNone && !same_row) {
        stats_.spots++;
        say(speech, menu_spot_line(spot, text), now);
    } else if (spot.kind != MenuSpot::Kind::kNone && spot.detail && !spot_.detail &&
               !text.description.empty()) {
        stats_.details++;
        say(speech, text.description, now);
    }
    spot_ = spot;
}

} // namespace maplethief::frontend
