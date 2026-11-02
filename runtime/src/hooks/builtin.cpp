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

int screen_shake(mt_cpu *cpu, const mt_hook_call *call) {
    const u32 self = cpu->r[0];
    const u32 percent = g_shake_percent.load(std::memory_order_relaxed);
    if (self == 0u || static_cast<std::uint64_t>(self) + k_shake_amp + 4u > cpu->mem_size ||
        cpu->mem[self + k_shake_on] == 0u || percent >= 100u) {
        return MT_HOOK_PASS; // no shake in this frame: the original
    }
    const std::int32_t amp = static_cast<std::int32_t>(read_word(cpu, self + k_shake_amp));
    const std::int32_t scaled = scale_shake(amp, percent);
    if (scaled == amp) {
        return MT_HOOK_PASS;
    }
    const u32 vptr = read_word(cpu, self);
    write_word(cpu, self + k_shake_amp, static_cast<u32>(scaled));
    g_shake_scaled.fetch_add(1, std::memory_order_relaxed);
    call->original(cpu); // r0 and the callee-saved registers are the original's
    // Paint can end the state (a scene's exit to the main menu deletes it inside the call): put
    // the amplitude back only into the same live CStateGame, and only if the frame did not set
    // a new one (a command 19 or 20 of its scene runner).
    const u32 now_state = current_state(cpu);
    const bool same = (now_state == self || now_state == 0u) && read_word(cpu, self) == vptr;
    if (same && cpu->mem[self + k_shake_on] != 0u &&
        static_cast<std::int32_t>(read_word(cpu, self + k_shake_amp)) == scaled) {
        write_word(cpu, self + k_shake_amp, static_cast<u32>(amp));
    }
    return MT_HOOK_HANDLED;
}

} // namespace

extern "C" void mt_hooks_install_screen_shake(std::uint32_t percent) noexcept {
    const u32 p = percent > 100u ? 100u : percent;
    g_shake_percent.store(p, std::memory_order_relaxed);
    mt_hook_register(MT_HOOK_FN_STATE_GAME_PAINT, screen_shake, nullptr, p < 100u ? 1 : 0);
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

extern "C" void mt_hooks_install_original_bug_fixes(int enabled) noexcept {
    mt_hooks_install_portal_no_exit(enabled);
    mt_hooks_install_early_play_guard(enabled);
}
