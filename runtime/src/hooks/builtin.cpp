// The port's own guest function hooks (#143, ADR 0069); see maplethief/hooks.h.
//
// OpenPortalURLAndExit 0x22955 (docs/re/network.md): `void (owner, const char *url)`, called
// by CStateMainMenu::OnKeyPressed 0x9462D for the main menu's popup 1, sub-menu 3, items 2-4
// with http://w.nexonmobile.com/CP/{winc581,gift,gamezone}.php?GV=35. It calls 0x6015, an empty
// `return 0` stub where the Korean "NGB" (Nexon Game Box) hand-off was, with ("NGB", 2, "URL",
// url), and then exit(1) through 0x8FF9. Its callers go on with the switch's break after the
// call (b 0x961A2 -> b 0x961EA), which the original never reaches.
//
// -[MainMenuView clickPlay:] 0xAAAB9 (#122, docs/ANALYSIS.md -> Program flow): sends
// clickOkButton: to the key pad (a pending OK that the empty CStateTitle::OnKeyPressed 0x97E65
// ignores), then makes a ChooseCharacterView and adds it at once. Its drawRect: 0xA632D reads
// [[objView robinImages] objectAtIndex:0] (0xA6584, return address 0xA6589) with no bounds
// check, and robinImages stays empty until CStateMainMenu::Enter 0x97201 runs
// LoadCharacterSlots 0x96845, on CStateTitle's 5th paint. The fix drops the click while the
// engine is still in CStateTitle, as the controller focus does for a confirm (#129,
// platform/src/focus.cpp reads the same three words).
//
// Both are fixes of original bugs (docs/re/original-bugs.md, #155): on with fix_original_bugs.
//
// PopupMenu_QuestTabOnKey 0x5462D (#167, docs/re/original-bugs.md 11): OK on a quest opens its
// page without storing the quest in CDataMgr +0x530, which the page's paint reads. The wrapper
// stores it. The trigger is a key the device never sends to that tab, so it is on in every
// mode.
#include "maplethief/hle/abi.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "maplethief/diag.h"
#include "maplethief/hooks.h"
#include "maplethief/memory.h"

namespace {

using u32 = std::uint32_t;

// Whether [addr, addr + 1) is an image segment's byte (the URL is a __cstring constant).
bool in_image(const mt_cpu *cpu, u32 addr, u32 *end) {
    mt_mem *mem = mt_mem_from_cpu(cpu);
    u32 n = 0;
    const mt_segment *segs = mem != nullptr ? mt_mem_segments(mem, &n) : nullptr;
    for (u32 i = 0; segs != nullptr && i < n; i++) {
        const std::uint64_t seg_end = static_cast<std::uint64_t>(segs[i].vmaddr) + segs[i].vmsize;
        if (addr >= segs[i].vmaddr && addr < seg_end && seg_end <= cpu->mem_size) {
            *end = static_cast<u32>(seg_end);
            return true;
        }
    }
    return false;
}

// The guest C string at addr, at most 255 bytes, if it lies in the image; else "?".
void read_url(const mt_cpu *cpu, u32 addr, char *out, std::size_t size) {
    std::snprintf(out, size, "?");
    u32 end = 0;
    if (!in_image(cpu, addr, &end)) {
        return;
    }
    std::size_t n = 0;
    while (n + 1 < size && addr + n < end && cpu->mem[addr + n] != 0) {
        const char c = static_cast<char>(cpu->mem[addr + n]);
        out[n++] = (c >= 0x20 && c < 0x7F) ? c : '?';
    }
    out[n] = '\0';
}

int portal_no_exit(mt_cpu *cpu, const mt_hook_call *) {
    char url[256];
    read_url(cpu, cpu->r[1], url, sizeof url);
    std::fprintf(stderr,
                 "hooks: OpenPortalURLAndExit(%s): not opened, the game goes on (the original "
                 "quits with exit(1))\n",
                 url);
    mt_tracef("hooks", "OpenPortalURLAndExit(0x%08X, %s) from 0x%08X: returns",
              static_cast<unsigned>(cpu->r[0]), url, static_cast<unsigned>(cpu->r[MT_LR]));
    return MT_HOOK_HANDLED; // void: nothing to return; r4-r11, sp and lr are untouched
}

// The engine's current state (docs/re/program-flow.md): SkyCanvas::GetInstance 0x2567D keeps
// the canvas at 0x11A950, its current state at +0xC; a CStateTitle's vtable pointer
// (docs/re/cpp-classes.md: the vtable at 0x119C90, its slots from 0x119C98).
constexpr u32 k_canvas_global = 0x11A950u;
constexpr u32 k_canvas_state = 0xCu;
constexpr u32 k_title_vptr = 0x119C98u;

// The guest word at addr, or 0 when it lies outside guest memory.
u32 read_word(const mt_cpu *cpu, u32 addr) {
    u32 v = 0;
    if (addr != 0u && static_cast<std::uint64_t>(addr) + 4u <= cpu->mem_size) {
        std::memcpy(&v, cpu->mem + addr, 4);
    }
    return v;
}

// Whether the engine's current state is a CStateTitle. False when the image does not hold the
// canvas global (a guest without the game's image) or there is no canvas or state yet.
bool engine_at_title(const mt_cpu *cpu) {
    u32 end = 0;
    if (cpu->mem == nullptr || !in_image(cpu, k_canvas_global, &end) ||
        k_canvas_global + 4u > end) {
        return false;
    }
    const u32 canvas = read_word(cpu, k_canvas_global);
    const u32 state = canvas != 0u ? read_word(cpu, canvas + k_canvas_state) : 0u;
    return state != 0u && read_word(cpu, state) == k_title_vptr;
}

int early_play_guard(mt_cpu *cpu, const mt_hook_call *) {
    if (!engine_at_title(cpu)) {
        return MT_HOOK_PASS; // the main menu is ready: Play as the original
    }
    std::fprintf(stderr, "hooks: -[MainMenuView clickPlay:] before the engine's main menu: "
                         "dropped (the original raises NSRangeException from 0xA6589)\n");
    mt_tracef("hooks", "-[MainMenuView clickPlay:](0x%08X) from 0x%08X in CStateTitle: dropped",
              static_cast<unsigned>(cpu->r[0]), static_cast<unsigned>(cpu->r[MT_LR]));
    return MT_HOOK_HANDLED; // void: nothing to return; r4-r11, sp and lr are untouched
}

// PopupMenu_QuestTabOnKey's data manager fields (docs/controls/keypad.md, "The skill, quest
// and option tabs' cursors").
constexpr u32 k_page_open = 0xACu;        // a quest's or an option's page is open
constexpr u32 k_main_quest = 0xB4u;       // byte: the cursor is on the main quest
constexpr u32 k_quest_row = 0xE4u;        // the side quest row under the cursor
constexpr u32 k_quest_scroll = 0x10Cu;    // the keys' scroll object: +4 its first row
constexpr u32 k_page_quest = 0x530u;      // the quest PopupMenu_PaintQuestTab 0x4ED75 pages
constexpr u32 k_owner_quests = 0x58u;     // the owner's quest lists (their +0x32C)
constexpr u32 k_main_quest_fn = 0x7A19Du; // (quests): the main quest, or 0
constexpr u32 k_side_quest_fn = 0x7A10Du; // (quests, index): the index-th side quest, or 0

bool in_guest(const mt_cpu *cpu, u32 addr, u32 size) {
    return addr != 0u && static_cast<std::uint64_t>(addr) + size <= cpu->mem_size;
}

int quest_page_fix(mt_cpu *cpu, const mt_hook_call *call) {
    const u32 mgr = cpu->r[0];
    const u32 key = cpu->r[1];
    const u32 owner = cpu->r[2];
    const bool was_open = read_word(cpu, mgr + k_page_open) != 0u;
    call->original(cpu);
    if (was_open || !in_guest(cpu, mgr, k_page_quest + 4u) ||
        read_word(cpu, mgr + k_page_open) != 1u) {
        return MT_HOOK_HANDLED; // no page opened by this key
    }
    // The page was opened: look up the quest as the original did (0x548D4-0x5491C) and store
    // it where the paint reads it. r0-r3 and r12 are free after the call; the original's
    // result (r0, r1) is kept.
    const u32 r0 = cpu->r[0];
    const u32 r1 = cpu->r[1];
    const u32 quests = read_word(cpu, owner + k_owner_quests);
    u32 quest = 0;
    if (cpu->mem[mgr + k_main_quest] != 0u) {
        const u32 args[] = {quests};
        quest =
            mt_call_guest(cpu, k_main_quest_fn, args, 1, MT_HOOK_FN_POPUP_MENU_QUEST_TAB_ON_KEY);
    } else {
        const u32 scroll = read_word(cpu, mgr + k_quest_scroll);
        const u32 args[] = {quests, read_word(cpu, mgr + k_quest_row) +
                                        (scroll != 0u ? read_word(cpu, scroll + 4u) : 0u)};
        quest =
            mt_call_guest(cpu, k_side_quest_fn, args, 2, MT_HOOK_FN_POPUP_MENU_QUEST_TAB_ON_KEY);
    }
    cpu->r[0] = r0;
    cpu->r[1] = r1;
    if (quest != 0u) {
        std::memcpy(cpu->mem + mgr + k_page_quest, &quest, 4);
    }
    mt_tracef("hooks", "PopupMenu_QuestTabOnKey(0x%08X, %d): the page of quest 0x%08X",
              static_cast<unsigned>(mgr), static_cast<int>(key), static_cast<unsigned>(quest));
    return MT_HOOK_HANDLED;
}

// CStateGame::Paint 0x87501 (#158, docs/re/screen-shake.md): the scene's camera shake. The
// fields of CStateGame (`this`, r0 at the entry):
constexpr u32 k_shake_on = 0x47Eu;  // byte: command 19 sets 1, command 20 clears it
constexpr u32 k_shake_amp = 0x488u; // int: the amplitude in screen points (command 19's P2)
std::atomic<u32> g_shake_percent{100};
std::atomic<std::uint64_t> g_shake_scaled{0};

void write_word(mt_cpu *cpu, u32 addr, u32 v) { std::memcpy(cpu->mem + addr, &v, 4); }

// The engine's current state, 0 when there is none or no canvas global (a synthetic guest).
u32 current_state(const mt_cpu *cpu) {
    u32 end = 0;
    if (cpu->mem == nullptr || !in_image(cpu, k_canvas_global, &end) ||
        k_canvas_global + 4u > end) {
        return 0;
    }
    const u32 canvas = read_word(cpu, k_canvas_global);
    return canvas != 0u ? read_word(cpu, canvas + k_canvas_state) : 0u;
}

// amp * percent / 100, rounded half away from zero.
std::int32_t scale_shake(std::int32_t amp, u32 percent) {
    const std::int64_t p = static_cast<std::int64_t>(amp) * percent;
    return static_cast<std::int32_t>(p >= 0 ? (p + 50) / 100 : -((-p + 50) / 100));
}

// The frame being painted (guest thread only): the CStateGame, the game's own amplitude and
// the scaled one the frame draws with.
struct ShakeFrame {
    bool active = false;
    bool counted = false;
    u32 self = 0, vptr = 0;
    std::int32_t raw = 0, scaled = 0;
};
ShakeFrame g_frame;

std::int32_t read_amp(const mt_cpu *cpu, u32 self) {
    return static_cast<std::int32_t>(read_word(cpu, self + k_shake_amp));
}

// The frame's CStateGame is still the engine's live state (Paint and the runner can end it: a
// scene's exit to the main menu deletes it inside the call; its memory may be reused).
bool frame_alive(const mt_cpu *cpu) {
    const u32 now = current_state(cpu);
    return (now == g_frame.self || now == 0u) && read_word(cpu, g_frame.self) == g_frame.vptr;
}

// The scaled amplitude in, while the shake is on (the frame then counts as scaled).
void scale_in(mt_cpu *cpu) {
    if (cpu->mem[g_frame.self + k_shake_on] != 0u && g_frame.scaled != g_frame.raw) {
        write_word(cpu, g_frame.self + k_shake_amp, static_cast<u32>(g_frame.scaled));
        if (!g_frame.counted) {
            g_frame.counted = true;
            g_shake_scaled.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// The game's own amplitude back, where the frame's scaled one still is.
void scale_out(mt_cpu *cpu) {
    if (cpu->mem[g_frame.self + k_shake_on] != 0u && g_frame.scaled != g_frame.raw &&
        read_amp(cpu, g_frame.self) == g_frame.scaled) {
        write_word(cpu, g_frame.self + k_shake_amp, static_cast<u32>(g_frame.raw));
    }
}

int shake_paint(mt_cpu *cpu, const mt_hook_call *call) {
    const u32 self = cpu->r[0];
    const u32 percent = g_shake_percent.load(std::memory_order_relaxed);
    if (g_frame.active || percent >= 100u || self == 0u ||
        static_cast<std::uint64_t>(self) + k_shake_amp + 4u > cpu->mem_size) {
        return MT_HOOK_PASS;
    }
    const std::int32_t raw = read_amp(cpu, self);
    g_frame = ShakeFrame{true, false, self, read_word(cpu, self), raw, scale_shake(raw, percent)};
    scale_in(cpu);
    call->original(cpu); // r0 and the callee-saved registers are the original's
    if (g_frame.active && frame_alive(cpu)) {
        scale_out(cpu);
    }
    g_frame.active = false;
    return MT_HOOK_HANDLED;
}

int shake_runner(mt_cpu *cpu, const mt_hook_call *call) {
    if (!g_frame.active || cpu->r[0] != g_frame.self) {
        return MT_HOOK_PASS; // not from the frame's Paint
    }
    scale_out(cpu); // the commands see (and set) the game's own value
    call->original(cpu);
    if (!frame_alive(cpu)) {
        g_frame.active = false; // the scene left the game: nothing more to write
        return MT_HOOK_HANDLED;
    }
    g_frame.raw = read_amp(cpu, g_frame.self);
    g_frame.scaled = scale_shake(g_frame.raw, g_shake_percent.load(std::memory_order_relaxed));
    scale_in(cpu); // for the offsets Paint draws next
    return MT_HOOK_HANDLED;
}

} // namespace

extern "C" void mt_hooks_install_screen_shake(std::uint32_t percent) noexcept {
    const u32 p = percent > 100u ? 100u : percent;
    g_shake_percent.store(p, std::memory_order_relaxed);
    mt_hook_register(MT_HOOK_FN_STATE_GAME_PAINT, shake_paint, nullptr, p < 100u ? 1 : 0);
    mt_hook_register(MT_HOOK_FN_SCENE_RUNNER, shake_runner, nullptr, p < 100u ? 1 : 0);
}

extern "C" std::uint64_t mt_hooks_screen_shake_scaled(void) noexcept {
    return g_shake_scaled.load(std::memory_order_relaxed);
}

extern "C" void mt_hooks_install_portal_no_exit(int enabled) noexcept {
    mt_hook_register(MT_HOOK_FN_OPEN_PORTAL_URL_AND_EXIT, portal_no_exit, nullptr, enabled);
}

extern "C" void mt_hooks_install_early_play_guard(int enabled) noexcept {
    mt_hook_register(MT_HOOK_FN_MAIN_MENU_CLICK_PLAY, early_play_guard, nullptr, enabled);
}

extern "C" void mt_hooks_install_quest_page_fix(int enabled) noexcept {
    mt_hook_register(MT_HOOK_FN_POPUP_MENU_QUEST_TAB_ON_KEY, quest_page_fix, nullptr, enabled);
}

extern "C" void mt_hooks_install_original_bug_fixes(int enabled) noexcept {
    mt_hooks_install_portal_no_exit(enabled);
    mt_hooks_install_early_play_guard(enabled);
}
