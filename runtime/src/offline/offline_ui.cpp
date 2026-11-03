// The dead online UI of the title menu, hidden by host overrides (#154, ADR 0084); see
// maplethief/offline.h.
//
// Facts (Ghidra 12.2 decompiles of the armv7 slice, docs/re/objc-shell.md#mainmenuview):
//   -[MainMenuView initWithFrame:] 0xAD9F9 makes the menu's image views, among them `moregame`
//       (ivar +152, MainMenuMoreGame.png) and, when -[EAGLView getGameCenter], `gamecenter`
//       (+156, MainMenuGameCenter.png). -drawRect: 0xABC85 only moves and fades them.
//   -[MainMenuView addButtonWithImageName:selectedImageName:withFrame:withEdgeInsets:
//       actionForTouchDown:actionForTouchUpInside:] 0xAB58D makes every button of the menu and
//       of About (-clickAbout: 0xA8A0D): r2, r3 the two image names, then on the stack a CGRect
//       and a UIEdgeInsets (8 words), the touch-down SEL and the touch-up-inside SEL. It returns
//       the UIButton, which the caller adds as a subview (and keeps in an ivar for the banner).
// The promo banner is the button of MainMenuAD.png (-clickADButton:, ivar cygnusAD +140) with
// TouchCloseButton.png (-clickADClose:, +144); More Games is the image view plus a pressed-state
// button (-clickMoreGame:); About's links are invisible buttons (-clickLinkURL1:-4:) over the
// addresses written as text. Every one of them opens a dead page (docs/re/network.md#urls).
//
// Each override runs the game's own method first, then hides what it made with -setHidden:YES.
// The game never shows them again (-clickADOrigin and -clickADClose: only move the banner), and
// UIKit neither draws, hits nor focuses a hidden view.
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "maplethief/hle/abi.h"
#include "maplethief/memory.h"
#include "maplethief/objc/host.h"
#include "maplethief/objc/objc.h"
#include "maplethief/offline.h"
#include "maplethief/recomp/calls.h"
#include "objc/objc_internal.h"

namespace {

using u32 = std::uint32_t;

constexpr const char *k_class = "MainMenuView";
constexpr const char *k_init = "initWithFrame:";
constexpr const char *k_add_button = "addButtonWithImageName:selectedImageName:withFrame:"
                                     "withEdgeInsets:actionForTouchDown:actionForTouchUpInside:";
// MainMenuView's ivars (work/re/objc.json; image ivars never slide, ADR 0026).
constexpr u32 k_ivar_moregame = 152;
constexpr u32 k_ivar_gamecenter = 156;

// The touch-up actions of the dead buttons, and the flag that hides each.
struct DeadAction {
    const char *selector;
    u32 flag;
};
constexpr DeadAction k_dead[] = {
    {"clickADButton:", MT_OFFLINE_HIDE_LINKS}, {"clickADClose:", MT_OFFLINE_HIDE_LINKS},
    {"clickMoreGame:", MT_OFFLINE_HIDE_LINKS}, {"clickLinkURL1:", MT_OFFLINE_HIDE_LINKS},
    {"clickLinkURL2:", MT_OFFLINE_HIDE_LINKS}, {"clickLinkURL3:", MT_OFFLINE_HIDE_LINKS},
    {"clickLinkURL4:", MT_OFFLINE_HIDE_LINKS}, {"clickGameCenter:", MT_OFFLINE_HIDE_GAME_CENTER},
};

// The method an override replaced: an image IMP (the game's), or a host function (a test's).
struct Original {
    u32 guest_imp;
    mt_guest_fn fn;
};

u32 g_flags = 0;
Original g_init{};
Original g_add_button{};
mt_offline_ui_stats g_stats{};

void log(mt_objc *rt, const char *text) {
    char line[256];
    std::snprintf(line, sizeof line, "offline: %s", text);
    if (rt != nullptr) {
        mt_objc_impl::log_line(rt, line);
    } else {
        std::fprintf(stderr, "%s\n", line);
    }
}

// Runs the replaced method with the registers and stack exactly as the sender left them (the
// arguments past r3 are on the guest stack at sp); lr is kept for the override's own caller.
void call_original(mt_cpu *cpu, const Original &o) {
    const u32 lr = cpu->r[MT_LR];
    cpu->r[MT_LR] = MT_HLE_RETURN;
    if (o.guest_imp != 0) {
        mt_dispatch(cpu, o.guest_imp, lr)(cpu);
    } else if (o.fn != nullptr) {
        o.fn(cpu);
    }
    cpu->r[MT_LR] = lr;
}

bool hide(mt_cpu *cpu, u32 view) {
    if (view == 0) {
        return false;
    }
    mt_objc_send1(cpu, view, "setHidden:", 1u);
    return true;
}

u32 ivar(mt_cpu *cpu, u32 self, u32 offset) {
    u32 v = 0;
    std::memcpy(&v, mt_g2h(cpu, self + offset, 4), 4);
    return v;
}

// -[MainMenuView initWithFrame:] -> self
void init_with_frame(mt_cpu *cpu) {
    call_original(cpu, g_init);
    const u32 self = cpu->r[0];
    if (self != 0) {
        if ((g_flags & MT_OFFLINE_HIDE_LINKS) != 0 && hide(cpu, ivar(cpu, self, k_ivar_moregame))) {
            g_stats.views_hidden++;
        }
        if ((g_flags & MT_OFFLINE_HIDE_GAME_CENTER) != 0 &&
            hide(cpu, ivar(cpu, self, k_ivar_gamecenter))) {
            g_stats.views_hidden++;
        }
    }
    cpu->r[0] = self;
}

// -[MainMenuView addButtonWithImageName:...actionForTouchUpInside:] -> the UIButton
void add_button(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    for (int i = 0; i < 2 + 8 + 1; i++) { // the image names, the frame and insets, touch down
        mt_arg_u32(&a);
    }
    const u32 up = mt_arg_u32(&a);
    call_original(cpu, g_add_button);
    const u32 button = cpu->r[0];
    const char *name = mt_objc_sel_name(mt_objc_current(), up);
    if (name != nullptr && mt_offline_ui_hides_action(g_flags, name) != 0 && hide(cpu, button)) {
        g_stats.buttons_hidden++;
    }
    cpu->r[0] = button;
}

bool wrap(mt_objc *rt, u32 cls, const char *selector, mt_guest_fn fn, Original *out) {
    mt_objc_method m{};
    const u32 sel = mt_objc_sel(rt, selector);
    if (sel == 0 || mt_objc_lookup(rt, cls, sel, &m) == 0 ||
        (m.guest_imp == 0 && m.fn == nullptr)) {
        char text[200];
        std::snprintf(text, sizeof text, "-[%s %.120s] not found: not overridden", k_class,
                      selector);
        log(rt, text);
        return false;
    }
    *out = Original{m.guest_imp, m.guest_imp != 0 ? nullptr : m.fn};
    return mt_objc_override(rt, k_class, 0, selector, fn) == 0;
}

} // namespace

extern "C" void mt_offline_ui_configure(std::uint32_t flags) noexcept {
    g_flags = flags & (MT_OFFLINE_HIDE_LINKS | MT_OFFLINE_HIDE_GAME_CENTER);
}

extern "C" std::uint32_t mt_offline_ui_flags(void) noexcept { return g_flags; }

extern "C" int mt_offline_ui_hides_action(std::uint32_t flags, const char *selector) noexcept {
    if (selector == nullptr) {
        return 0;
    }
    for (const DeadAction &d : k_dead) {
        if ((flags & d.flag) != 0 && std::strcmp(d.selector, selector) == 0) {
            return 1;
        }
    }
    return 0;
}

extern "C" std::uint32_t mt_offline_ui_install(mt_objc *rt) noexcept {
    g_stats.installed = 0;
    if (g_flags == 0 || rt == nullptr) {
        return 0;
    }
    const u32 cls = mt_objc_class_named(rt, k_class);
    if (cls == 0) {
        log(rt, "no MainMenuView in the image: the online UI is not hidden");
        return 0;
    }
    u32 n = 0;
    n += wrap(rt, cls, k_init, init_with_frame, &g_init) ? 1u : 0u;
    n += wrap(rt, cls, k_add_button, add_button, &g_add_button) ? 1u : 0u;
    g_stats.installed = n;
    char text[200];
    std::snprintf(
        text, sizeof text, "%u MainMenuView overrides hide %s%s", static_cast<unsigned>(n),
        (g_flags & MT_OFFLINE_HIDE_LINKS) != 0 ? "the banner, More Games and About's links" : "",
        (g_flags & MT_OFFLINE_HIDE_GAME_CENTER) != 0
            ? ((g_flags & MT_OFFLINE_HIDE_LINKS) != 0 ? " and the Game Center button"
                                                      : "the Game Center button")
            : "");
    log(rt, text);
    return n;
}

extern "C" mt_offline_ui_stats mt_offline_ui_get_stats(void) noexcept { return g_stats; }
