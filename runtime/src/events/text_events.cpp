// Text events (#391, ADR 0187); see maplethief/text_events.h. The guest facts are from the
// decompiler (docs/re/speech.md): CChatBox::FirstPage 0x34CD1 / NextPage 0x34A05 free the old
// lines, set the page (+0x10) and its kind (+0x0C), and wrap the CMsg's text with Text_Wrap
// 0x22EC5 into +0x18 / +0x1C; CChatBox::paint 0x32BBD names the speaker; CStateGame::Paint
// 0x87501 hands CChatBox::drawChatBox 0x35189 the scene box's speaker (+0x4B4) and its two lines.
// #392: CNotifyTextList::Add 0x78339 (list, text, colour) adds nothing while the list's most
// entries (+0x0C) is below 1, else appends a CNotifyText (0x7798D) of the text; Popup_Open 0x3EAA1
// (mgr, id, text, kind) closes the last popup, copies the message into the CMPopup at *0x11A96C
// (+0x04: the preset at 0x108C70 + id * 0x200 when id < 100 and it is set, else the text), sets
// the kind (+0x10) and its two button labels (+0x70, +0x74; CMPopup::SetKind 0xE799) and wraps the
// message into +0x08 / +0x0C (CMPopup::Layout 0xE8B5).
#include "maplethief/text_events.h"

#include <cstdint>
#include <string>
#include <vector>

#include "cf/cf_objects.h"
#include "events/guest_read.h"
#include "foundation/foundation.h"
#include "maplethief/diag.h"
#include "maplethief/hooks.h"

namespace {

using u32 = std::uint32_t;
using i32 = std::int32_t;
using mt_events::rd32;
using mt_events::rd_cstr;
using mt_events::rdptr;
using mt_events::readable;

// CChatBox (docs/re/qol.md#fast-text).
constexpr u32 k_box_page_kind = 0x0C; // the page's CMsg kind: 0 the hero, 1 an NPC
constexpr u32 k_box_mode = 0x08;      // 0 OK, 1 and 3 Accept / Refuse, 2 a list of rows
constexpr u32 k_box_page = 0x10;
constexpr u32 k_box_pages = 0x14; // the CMsg list
constexpr u32 k_box_lines = 0x18; // the page's wrapped lines (new[] of new[] strings)
constexpr u32 k_box_line_count = 0x1C;
constexpr u32 k_box_rows = 0x20;  // the CSelItem list: the row's Scripts key +0x04
constexpr u32 k_box_state = 0x48; // the CStateGame
constexpr u32 k_row_key = 0x04;
// The game's lists (0x25380 count, 0x25148 item).
constexpr u32 k_list_count = 0x04;
constexpr u32 k_list_items = 0x08;
// CStateGame.
constexpr u32 k_state_npc = 0xAC; // the NPC the box talks for, -1 the boss
constexpr u32 k_scene_box_word = 0x1E8;
constexpr u32 k_scene_box_shift = 8; // +0x1E9: a speech box waits for a key
constexpr u32 k_scene_speaker = 0x4B4;
constexpr u32 k_scene_first = 0x4C8;
constexpr u32 k_scene_lines = 0x4D0;
constexpr u32 k_scene_count = 0x4D4;
// CMob (docs/re/mechanics.md): the id, and the name CChatBox::paint draws for a boss.
constexpr u32 k_mob_id = 0xA0;
constexpr u32 k_mob_name = 0x9C;
// The NPC table's bytes: rows +0x02 (u16), stride +0x04 (u16), the rows from +0x06; column 0
// the id (a byte), the name at column 1, 32 bytes (docs/re/text.md#the-data-tables).
constexpr u32 k_table_rows = 0x06;
constexpr u32 k_npc_name = 1;
constexpr u32 k_npc_name_width = 0x20;
// CStateGame's notify lists (docs/re/hud-extras.md): +0x4C under the compass, +0x50 and +0x54
// (the map's name) at the top centre.
constexpr u32 k_state_notify[] = {0x4C, 0x50, 0x54};
constexpr u32 k_notify_most = 0x0C; // CNotifyTextList: the most entries; 0 adds nothing
// The CMPopup (docs/re/speech.md#popups).
constexpr u32 k_popup_lines = 0x08; // the message's wrapped lines (new[] of new[] strings)
constexpr u32 k_popup_line_count = 0x0C;
constexpr u32 k_popup_kind = 0x10;
constexpr u32 k_popup_label_1 = 0x70; // the button labels (char *), kinds 1 to 6
constexpr u32 k_popup_label_2 = 0x74;
constexpr i32 k_popup_kinds = 6;
// SkyCanvas::Instance and its current state (docs/re/program-flow.md).
constexpr u32 k_canvas_global = 0x11A950u;
constexpr u32 k_canvas_state = 0x0C;

constexpr i32 k_max_lines = 64;
constexpr std::size_t k_max_line = 512;
constexpr std::size_t k_max_name = 128;

struct Listener {
    mt_text_event_fn fn = nullptr;
    void *user = nullptr;
};
Listener g_listeners[MT_TEXT_EVENTS_MAX_LISTENERS];
mt_text_events_stats g_stats{};

// --- UTF-8 ----------------------------------------------------------------------------------

// The length of the UTF-8 sequence at s[i], or 0 if it is not a whole valid one.
std::size_t utf8_len(const std::string &s, std::size_t i) {
    const auto b = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned c = b(i);
    std::size_t n = 0;
    std::uint32_t cp = 0;
    if (c < 0x80) {
        return 1;
    } else if (c >= 0xC2 && c <= 0xDF) {
        n = 2;
        cp = c & 0x1F;
    } else if (c >= 0xE0 && c <= 0xEF) {
        n = 3;
        cp = c & 0x0F;
    } else if (c >= 0xF0 && c <= 0xF4) {
        n = 4;
        cp = c & 0x07;
    } else {
        return 0;
    }
    if (i + n > s.size()) {
        return 0;
    }
    for (std::size_t k = 1; k < n; k++) {
        if ((b(i + k) & 0xC0) != 0x80) {
            return 0;
        }
        cp = (cp << 6) | (b(i + k) & 0x3F);
    }
    const bool overlong = (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000);
    const bool bad = overlong || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF);
    return bad ? 0 : n;
}

bool valid_utf8(const std::string &s) {
    for (std::size_t i = 0; i < s.size();) {
        const std::size_t n = utf8_len(s, i);
        if (n == 0) {
            return false;
        }
        i += n;
    }
    return true;
}

// `s` with each byte that starts no valid sequence as U+FFFD.
std::string sanitized(const std::string &s) {
    std::string out;
    for (std::size_t i = 0; i < s.size();) {
        const std::size_t n = utf8_len(s, i);
        if (n == 0) {
            out += "\xEF\xBF\xBD";
            i++;
        } else {
            out.append(s, i, n);
            i += n;
        }
    }
    return out;
}

// A data table's text: ASCII (English) or CP949 (Korean, never valid UTF-8).
std::string table_text(const std::string &bytes) {
    if (valid_utf8(bytes)) {
        return bytes;
    }
    std::u16string u;
    if (mt_cf::decode(reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size(),
                      mt_cf::k_enc_dos_korean, u)) {
        return sanitized(mt_cf::to_utf8(u));
    }
    return sanitized(bytes);
}

bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Text_Wrap's lines as one text. A line cut at a space keeps the space; one cut at a newline or
// '$' loses it, so a space goes between two lines that have none. Runs of white space become one
// space, and none is left at either end.
std::string joined(const std::vector<std::string> &lines) {
    std::string raw;
    for (const std::string &l : lines) {
        if (!raw.empty() && !l.empty() && !space(raw.back()) && !space(l.front())) {
            raw.push_back(' ');
        }
        raw += l;
    }
    std::string out;
    for (const char c : raw) {
        if (space(c)) {
            if (!out.empty() && out.back() != ' ') {
                out.push_back(' ');
            }
        } else {
            out.push_back(c);
        }
    }
    if (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return sanitized(out);
}

// --- speakers -------------------------------------------------------------------------------

struct Speaker {
    u32 kind = MT_TEXT_SPEAKER_NONE;
    i32 id = 0;
    std::string name;
};

std::string localized(mt_cpu *cpu, const char *table, const std::string &key) {
    std::string out;
    return mt_ns::localized_utf8(cpu, table, key, &out) ? sanitized(out) : std::string();
}

Speaker hero(mt_cpu *cpu, const char *key) {
    return Speaker{MT_TEXT_SPEAKER_HERO, -1, localized(cpu, "ScriptExtra", key)};
}

// The NPC's name in the NPC table (0x3990C finds the row, 0x37998 copies the field).
Speaker npc(mt_cpu *cpu, i32 id) {
    Speaker s{MT_TEXT_SPEAKER_NPC, id, {}};
    const u32 table = rdptr(cpu, MT_TEXT_NPC_TABLE);
    if (table == 0 || !readable(cpu, table, k_table_rows) || id < 0 || id > 0xFF) {
        return s;
    }
    const u32 rows = cpu->mem[table + 2] | (cpu->mem[table + 3] << 8);
    const u32 stride = cpu->mem[table + 4] | (cpu->mem[table + 5] << 8);
    if (stride <= k_npc_name + k_npc_name_width - 1) {
        return s;
    }
    for (u32 r = 0; r < rows; r++) {
        const u32 row = table + k_table_rows + stride * r;
        if (!readable(cpu, row, k_npc_name + k_npc_name_width)) {
            break;
        }
        if (cpu->mem[row] == static_cast<u32>(id)) {
            std::string name;
            rd_cstr(cpu, row + k_npc_name, k_npc_name_width, &name);
            s.name = table_text(name);
            break;
        }
    }
    return s;
}

// The first mob in the list with an id of 0x40 or more other than 0x41 (0x75710: the boss the
// box and the scene name), and its name (CChatBox::paint localizes it as a Scripts key; the key
// itself if it is none).
Speaker boss(mt_cpu *cpu) {
    Speaker s{MT_TEXT_SPEAKER_MOB, 0, {}};
    const u32 list = rdptr(cpu, MT_TEXT_MOB_LIST);
    i32 count = 0;
    if (list == 0 || !rd32(cpu, list + k_list_count, &count)) {
        return s;
    }
    const u32 items = rdptr(cpu, list + k_list_items);
    for (i32 i = 0; items != 0 && i < count && i < 4096; i++) {
        const u32 mob = rdptr(cpu, items + 4u * static_cast<u32>(i));
        i32 id = 0;
        if (mob == 0 || !rd32(cpu, mob + k_mob_id, &id) || id <= 0x3F || id == 0x41) {
            continue;
        }
        s.id = id;
        std::string name;
        if (rd_cstr(cpu, rdptr(cpu, mob + k_mob_name), k_max_name, &name)) {
            const std::string text = valid_utf8(name) ? localized(cpu, "Scripts", name) : "";
            s.name = text.empty() ? table_text(name) : text;
        }
        break;
    }
    return s;
}

// CChatBox::drawChatBox's speaker (CStateGame +0x4B4).
Speaker scene_speaker(mt_cpu *cpu, i32 code) {
    if (code >= 0) {
        return npc(cpu, code);
    }
    if (code == -1) {
        return hero(cpu, "CChatBox.drawChatBox.1");
    }
    if (code == -2) {
        return boss(cpu);
    }
    const char *key = code == -0x40   ? "CChatBox.drawChatBox.2"
                      : code == -0x42 ? "CChatBox.drawChatBox.3"
                      : code == -0x43 ? "CChatBox.drawChatBox.4"
                      : code == -0x44 ? "CChatBox.drawChatBox.5"
                      : code == -0x45 ? "CChatBox.drawChatBox.6"
                                      : nullptr;
    if (key == nullptr) {
        return Speaker{};
    }
    return Speaker{MT_TEXT_SPEAKER_MOB, -code, localized(cpu, "ScriptExtra", key)};
}

// --- events ---------------------------------------------------------------------------------

void emit(u32 kind, const Speaker &who, i32 index, i32 count, i32 value, const std::string &text) {
    mt_text_event e{};
    e.kind = kind;
    e.speaker_kind = who.kind;
    e.speaker_id = who.id;
    e.index = index;
    e.count = count;
    e.value = value;
    e.speaker = who.name.c_str();
    e.text = text.c_str();
    mt_tracef("text", "%s: speaker %u %d (%zu bytes), %d of %d, value %d, %zu bytes",
              kind == MT_TEXT_EVENT_PAGE        ? "page"
              : kind == MT_TEXT_EVENT_ANSWER    ? "answer"
              : kind == MT_TEXT_EVENT_SCENE_BOX ? "scene box"
              : kind == MT_TEXT_EVENT_NOTICE    ? "notice"
                                                : "popup",
              static_cast<unsigned>(who.kind), who.id, who.name.size(), index, count, value,
              text.size());
    for (const Listener &l : g_listeners) {
        if (l.fn != nullptr) {
            l.fn(l.user, &e);
        }
    }
}

// The strings of a pointer array; false if one is not readable.
bool strings_at(const mt_cpu *cpu, u32 array, i32 count, std::vector<std::string> *out) {
    for (i32 i = 0; i < count; i++) {
        std::string line;
        if (!rd_cstr(cpu, rdptr(cpu, array + 4u * static_cast<u32>(i)), k_max_line, &line)) {
            return false;
        }
        out->push_back(std::move(line));
    }
    return true;
}

// The rows of a mode 2 box, localized, one per line.
bool rows_text(mt_cpu *cpu, u32 box, std::string *out) {
    const u32 list = rdptr(cpu, box + k_box_rows);
    i32 n = 0;
    if (list == 0 || !rd32(cpu, list + k_list_count, &n) || n < 0 || n > k_max_lines) {
        return false;
    }
    const u32 items = n > 0 ? rdptr(cpu, list + k_list_items) : 0;
    out->clear();
    for (i32 i = 0; i < n; i++) {
        std::string key;
        const u32 row = rdptr(cpu, items + 4u * static_cast<u32>(i));
        if (row == 0 || !rd_cstr(cpu, rdptr(cpu, row + k_row_key), k_max_line, &key)) {
            return false;
        }
        const std::string text = valid_utf8(key) ? localized(cpu, "Scripts", key) : "";
        *out += (i > 0 ? "\n" : "") + (text.empty() ? sanitized(key) : text);
    }
    return true;
}

// The page FirstPage or NextPage just set, and the answer its box asks for on the last.
void page(mt_cpu *cpu, u32 box) {
    i32 kind = 0, mode = 0, index = 0, count = 0, lines = 0, npc_id = 0;
    const u32 array = rdptr(cpu, box + k_box_lines);
    const u32 pages = rdptr(cpu, box + k_box_pages);
    const u32 state = rdptr(cpu, box + k_box_state);
    std::vector<std::string> text;
    if (array == 0 || pages == 0 || !rd32(cpu, box + k_box_page_kind, &kind) ||
        !rd32(cpu, box + k_box_mode, &mode) || !rd32(cpu, box + k_box_page, &index) ||
        !rd32(cpu, pages + k_list_count, &count) || !rd32(cpu, box + k_box_line_count, &lines) ||
        lines < 0 || lines > k_max_lines || !strings_at(cpu, array, lines, &text) ||
        (kind != 0 && (state == 0 || !rd32(cpu, state + k_state_npc, &npc_id)))) {
        g_stats.ignored++;
        return;
    }
    const Speaker who = kind == 0      ? hero(cpu, "CChatBox.paint.1")
                        : npc_id == -1 ? boss(cpu)
                                       : npc(cpu, npc_id);
    g_stats.pages++;
    emit(MT_TEXT_EVENT_PAGE, who, index, count, 0, joined(text));
    if (index != count - 1 || (mode != 1 && mode != 2 && mode != 3)) {
        return;
    }
    std::string rows;
    if (mode == 2 && !rows_text(cpu, box, &rows)) {
        g_stats.ignored++;
        return;
    }
    g_stats.answers++;
    emit(MT_TEXT_EVENT_ANSWER, who, index, count, mode, rows);
}

int page_wrap(mt_cpu *cpu, const mt_hook_call *call) {
    const u32 box = cpu->r[0];
    call->original(cpu);
    g_stats.entries++;
    if ((cpu->r[0] & 0xFFu) != 0) { // false: past the last page, nothing set
        page(cpu, box);
    }
    return MT_HOOK_HANDLED;
}

// The scene box last seen.
struct SceneBox {
    bool on = false;
    u32 state = 0, lines = 0;
    i32 first = -1;
    std::string text;
} g_box;

// The engine's current state, 0 when there is none or no canvas global (a synthetic guest).
u32 current_state(const mt_cpu *cpu) {
    const u32 canvas = rdptr(cpu, k_canvas_global);
    return canvas != 0 ? rdptr(cpu, canvas + k_canvas_state) : 0u;
}

void scene_box(mt_cpu *cpu, u32 state) {
    i32 word = 0;
    if (!rd32(cpu, state + k_scene_box_word, &word) ||
        ((static_cast<u32>(word) >> k_scene_box_shift) & 0xFFu) == 0) {
        g_box.on = false;
        return;
    }
    i32 first = 0, count = 0, code = 0;
    const u32 lines = rdptr(cpu, state + k_scene_lines);
    std::vector<std::string> text;
    const bool ok = rd32(cpu, state + k_scene_first, &first) &&
                    rd32(cpu, state + k_scene_count, &count) &&
                    rd32(cpu, state + k_scene_speaker, &code) && lines != 0 && first >= 0 &&
                    count >= 0 && count <= 4096 &&
                    // Paint's two lines: first and first + 1, those below count.
                    strings_at(cpu, lines + 4u * static_cast<u32>(first),
                               first < count ? (first + 1 < count ? 2 : 1) : 0, &text);
    const std::string line = ok ? joined(text) : std::string();
    if (g_box.on && g_box.state == state && g_box.first == first && g_box.lines == lines &&
        g_box.text == line) {
        return; // the box still shows the same lines
    }
    g_box = SceneBox{true, state, lines, first, line};
    if (!ok) {
        g_stats.ignored++;
        return;
    }
    if (line.empty()) {
        return; // Paint draws no line
    }
    g_stats.scene_boxes++;
    emit(MT_TEXT_EVENT_SCENE_BOX, scene_speaker(cpu, code), first, count, 0, line);
}

int paint_wrap(mt_cpu *cpu, const mt_hook_call *call) {
    const u32 state = cpu->r[0];
    const u32 vptr = rdptr(cpu, state);
    call->original(cpu);
    g_stats.entries++;
    // Paint can end the state (a scene's exit to the main menu deletes it): read only a state
    // that is still the engine's.
    const u32 now = current_state(cpu);
    if (vptr != 0 && (now == state || now == 0) && rdptr(cpu, state) == vptr) {
        scene_box(cpu, state);
    } else {
        g_box.on = false;
    }
    return MT_HOOK_HANDLED;
}

// --- notify lines and popups (#392) --------------------------------------------------------

// Which of the current CStateGame's lists `list` is (0 to 2), -1 if none.
i32 notify_list(const mt_cpu *cpu, u32 list) {
    const u32 state = current_state(cpu);
    for (i32 i = 0; state != 0 && i < 3; i++) {
        if (rdptr(cpu, state + k_state_notify[i]) == list) {
            return i;
        }
    }
    return -1;
}

int notify_wrap(mt_cpu *cpu, const mt_hook_call *call) {
    const u32 list = cpu->r[0], at = cpu->r[1], colour = cpu->r[2];
    // The caller's text (its stack buffer or a localized string), read before Add runs.
    std::string text;
    i32 most = 0;
    const bool ok =
        rd32(cpu, list + k_notify_most, &most) && rd_cstr(cpu, at, k_max_line * 2, &text);
    call->original(cpu);
    g_stats.entries++;
    if (!ok) {
        g_stats.ignored++;
        return MT_HOOK_HANDLED;
    }
    const std::string line = joined({table_text(text)});
    if (most < 1 || line.empty()) {
        return MT_HOOK_HANDLED; // Add added nothing, or a line with nothing to say
    }
    g_stats.notices++;
    emit(MT_TEXT_EVENT_NOTICE, Speaker{}, static_cast<i32>(colour & 0xFFFFFFu), 0,
         notify_list(cpu, list), line);
    return MT_HOOK_HANDLED;
}

// The popup Popup_Open just set.
void popup(mt_cpu *cpu) {
    const u32 box = rdptr(cpu, MT_TEXT_POPUP);
    i32 id = 0, kind = 0, lines = 0;
    std::vector<std::string> text;
    if (box == 0 || !rd32(cpu, MT_TEXT_POPUP_ID, &id) || !rd32(cpu, box + k_popup_kind, &kind) ||
        !rd32(cpu, box + k_popup_line_count, &lines) || lines < 0 || lines > k_max_lines) {
        g_stats.ignored++;
        return;
    }
    const u32 array = rdptr(cpu, box + k_popup_lines);
    if ((lines > 0 && array == 0) || !strings_at(cpu, array, lines, &text)) {
        g_stats.ignored++;
        return;
    }
    // The labels CMPopup::SetKind set for this kind; kind 0 has none (what +0x70 holds is the last
    // one's).
    Speaker buttons;
    if (kind >= 1 && kind <= k_popup_kinds) {
        for (const u32 field : {k_popup_label_1, k_popup_label_2}) {
            std::string label;
            const u32 at = rdptr(cpu, box + field);
            if (at != 0 && rd_cstr(cpu, at, k_max_name, &label)) {
                label = joined({table_text(label)});
                if (!label.empty()) {
                    buttons.name += (buttons.name.empty() ? "" : "\n") + label;
                }
            }
        }
    }
    g_stats.popups++;
    emit(MT_TEXT_EVENT_POPUP, buttons, id, 0, kind, joined(text));
}

int popup_wrap(mt_cpu *cpu, const mt_hook_call *call) {
    call->original(cpu);
    g_stats.entries++;
    popup(cpu);
    return MT_HOOK_HANDLED;
}

} // namespace

int mt_text_events_add_listener(mt_text_event_fn fn, void *user) noexcept {
    if (fn == nullptr) {
        return -1;
    }
    Listener *free_slot = nullptr;
    for (Listener &l : g_listeners) {
        if (l.fn == fn && l.user == user) {
            return 0;
        }
        if (l.fn == nullptr && free_slot == nullptr) {
            free_slot = &l;
        }
    }
    if (free_slot == nullptr) {
        return -1;
    }
    free_slot->fn = fn;
    free_slot->user = user;
    return 0;
}

void mt_text_events_remove_listener(mt_text_event_fn fn, void *user) noexcept {
    for (Listener &l : g_listeners) {
        if (l.fn == fn && l.user == user) {
            l = Listener{};
        }
    }
}

void mt_text_events_install(int enabled) noexcept {
    mt_hook_wrap(MT_HOOK_FN_CHAT_BOX_FIRST_PAGE, page_wrap, nullptr, enabled);
    mt_hook_wrap(MT_HOOK_FN_CHAT_BOX_NEXT_PAGE, page_wrap, nullptr, enabled);
    mt_hook_wrap(MT_HOOK_FN_STATE_GAME_PAINT, paint_wrap, nullptr, enabled);
    mt_hook_wrap(MT_HOOK_FN_NOTIFY_TEXT_ADD, notify_wrap, nullptr, enabled);
    mt_hook_wrap(MT_HOOK_FN_POPUP_OPEN, popup_wrap, nullptr, enabled);
    if (enabled == 0) {
        g_box = SceneBox{};
    }
}

void mt_text_events_reset(void) noexcept { g_box = SceneBox{}; }

mt_text_events_stats mt_text_events_get_stats(void) noexcept { return g_stats; }

void mt_text_events_reset_stats(void) noexcept { g_stats = mt_text_events_stats{}; }
