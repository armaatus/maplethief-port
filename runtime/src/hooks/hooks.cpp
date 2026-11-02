// Guest function hooks (#143, ADR 0069): the registry and mt_hook_entry, the generated code's
// way in (MT_FN_HOOK); see maplethief/hooks.h.
#include "maplethief/hooks.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "maplethief/diag.h"
#include "maplethief/recomp/calls.h"

namespace {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

// The functions the generated code hooks (recomp/hooks.py's HOOKED; tests/test_recomp_hooks.py
// checks that this table names every MT_HOOK_FN_* of maplethief/hooks.h).
constexpr u32 k_hookable[] = {
    MT_HOOK_FN_FILL_RECT,                   // 0x8079
    MT_HOOK_FN_CAMERA_CLAMP,                // 0xC069
    MT_HOOK_FN_CAMERA_FOLLOW,               // 0xC175
    MT_HOOK_FN_OPEN_PORTAL_URL_AND_EXIT,    // 0x22955
    MT_HOOK_FN_DRAW_IMAGE,                  // 0x24DC9
    MT_HOOK_FN_SAVE_QUEST_DATA,             // 0x38F55
    MT_HOOK_FN_QUICK_SLOTS_PAINT,           // 0x4CAED
    MT_HOOK_FN_POPUP_MENU_SKILL_TAB_ON_KEY, // 0x54051
    MT_HOOK_FN_POPUP_MENU_QUEST_TAB_ON_KEY, // 0x5462D
    MT_HOOK_FN_POPUP_MENU_PAINT,            // 0x56B99
    MT_HOOK_FN_SHOP_PAINT,                  // 0x56E91
    MT_HOOK_FN_MAP_PAINT_2,                 // 0x58AE9
    MT_HOOK_FN_MAP_PAINT_1,                 // 0x59119
    MT_HOOK_FN_MAP_PAINT_0,                 // 0x59BB1
    MT_HOOK_FN_SCENE_RUNNER,                // 0x83C71
    MT_HOOK_FN_STATE_GAME_PAINT,            // 0x87501
    MT_HOOK_FN_MAIN_MENU_CLICK_PLAY,        // 0xAAAB9
    MT_HOOK_FN_KEYPAD_COLOR_AT_PIXEL,       // 0xB4DB5
};
constexpr std::size_t k_count = sizeof k_hookable / sizeof k_hookable[0];

// A wrapper around the function's handler (mt_hook_wrap); `handler` nullptr: a free place.
struct Wrap {
    std::atomic<mt_hook_fn> handler{nullptr};
    std::atomic<void *> user{nullptr};
    std::atomic<bool> enabled{false};
};
constexpr std::size_t k_wraps = 4; // wrappers per function

struct Slot {
    std::atomic<mt_hook_fn> handler{nullptr};
    std::atomic<void *> user{nullptr};
    std::atomic<bool> enabled{false};
    Wrap wraps[k_wraps]; // [0] the innermost (next to the handler), [k_wraps - 1] the outermost
    std::atomic<u64> entries{0};
    std::atomic<u64> handled{0};
    std::atomic<u64> passed{0};
};

Slot g_slots[k_count];
// Handlers and wrappers registered: 0 lets mt_hook_entry answer at once.
std::atomic<u32> g_registered{0};

Slot *slot(u32 fn) {
    for (std::size_t i = 0; i < k_count; i++) {
        if (k_hookable[i] == fn) {
            return &g_slots[i];
        }
    }
    return nullptr;
}

Wrap *find_wrap(Slot *s, mt_hook_fn wrapper) {
    for (Wrap &w : s->wraps) {
        if (w.handler.load() == wrapper) {
            return &w;
        }
    }
    return nullptr;
}

// One call's chain (#166 x #167): the wrappers that are on, from the outermost in, then the
// handler if it is on, then the original. A wrapper's call->original is chain_next, which goes
// on one level in. The level it goes on at is the calling wrapper's cursor, kept per thread and
// restored on the way out, so a hooked call made inside a handler has a chain of its own.
struct Cursor {
    Slot *s;
    u32 fn;
    mt_guest_fn original;
    int next; // the next level in: a wrapper's index, or -1 the handler
};
thread_local const Cursor *t_cursor = nullptr;

int dispatch(mt_cpu *cpu, Slot *s, u32 fn, mt_guest_fn original, int from);

void chain_next(mt_cpu *cpu) {
    const Cursor c = *t_cursor;
    if (dispatch(cpu, c.s, c.fn, c.original, c.next) == 0) {
        c.original(cpu);
    }
}

// Runs the levels from `from` in (a wrapper's index, -1 the handler) until one handles the
// call: 1. 0 when each one passed or none is on: the original is still to run.
int dispatch(mt_cpu *cpu, Slot *s, u32 fn, mt_guest_fn original, int from) {
    for (int i = from; i >= -1; i--) {
        mt_hook_fn handler = nullptr;
        void *user = nullptr;
        if (i >= 0) {
            const Wrap &w = s->wraps[i];
            handler = w.enabled.load() ? w.handler.load() : nullptr;
            user = w.user.load();
        } else {
            handler = s->enabled.load() ? s->handler.load() : nullptr;
            user = s->user.load();
        }
        if (handler == nullptr) {
            continue;
        }
        if (i < 0) { // the handler: its original is the function's own
            const mt_hook_call call{fn, original, user};
            return handler(cpu, &call) == MT_HOOK_PASS ? 0 : 1;
        }
        const Cursor cursor{s, fn, original, i - 1};
        const Cursor *outer = t_cursor;
        t_cursor = &cursor;
        const mt_hook_call call{fn, chain_next, user};
        const int r = handler(cpu, &call);
        t_cursor = outer;
        if (r != MT_HOOK_PASS) {
            return 1;
        }
    }
    return 0;
}

} // namespace

extern "C" int mt_hook_hookable(u32 fn) noexcept { return slot(fn) != nullptr ? 1 : 0; }

extern "C" int mt_hook_register(u32 fn, mt_hook_fn handler, void *user, int enabled) noexcept {
    Slot *s = slot(fn);
    if (s == nullptr || handler == nullptr) {
        return 0;
    }
    s->enabled.store(false);
    s->user.store(user);
    if (s->handler.exchange(handler) == nullptr) {
        g_registered.fetch_add(1);
    }
    s->enabled.store(enabled != 0);
    mt_tracef("hooks", "0x%08X: handler registered, %s", static_cast<unsigned>(fn),
              enabled != 0 ? "on" : "off");
    return 1;
}

extern "C" void mt_hook_unregister(u32 fn) noexcept {
    Slot *s = slot(fn);
    if (s == nullptr) {
        return;
    }
    s->enabled.store(false);
    if (s->handler.exchange(nullptr) != nullptr) {
        g_registered.fetch_sub(1);
    }
    s->user.store(nullptr);
}

extern "C" int mt_hook_set_enabled(u32 fn, int enabled) noexcept {
    Slot *s = slot(fn);
    if (s == nullptr || s->handler.load() == nullptr) {
        return 0;
    }
    s->enabled.store(enabled != 0);
    mt_tracef("hooks", "0x%08X: %s", static_cast<unsigned>(fn), enabled != 0 ? "on" : "off");
    return 1;
}

extern "C" int mt_hook_enabled(u32 fn) noexcept {
    const Slot *s = slot(fn);
    return s != nullptr && s->handler.load() != nullptr && s->enabled.load() ? 1 : 0;
}

extern "C" int mt_hook_wrap(u32 fn, mt_hook_fn wrapper, void *user, int enabled) noexcept {
    Slot *s = slot(fn);
    if (s == nullptr || wrapper == nullptr) {
        return 0;
    }
    Wrap *w = find_wrap(s, wrapper);
    if (w == nullptr) {
        // A new wrapper goes outside every one there: the first free place above the last used.
        std::size_t at = k_wraps;
        for (std::size_t i = k_wraps; i-- > 0;) {
            if (s->wraps[i].handler.load() != nullptr) {
                break;
            }
            at = i;
        }
        if (at == k_wraps) {
            return 0; // no place left
        }
        w = &s->wraps[at];
        w->enabled.store(false);
        w->user.store(user);
        w->handler.store(wrapper);
        g_registered.fetch_add(1);
    } else {
        w->user.store(user);
    }
    w->enabled.store(enabled != 0);
    mt_tracef("hooks", "0x%08X: wrapper registered, %s", static_cast<unsigned>(fn),
              enabled != 0 ? "on" : "off");
    return 1;
}

extern "C" void mt_hook_unwrap(u32 fn, mt_hook_fn wrapper) noexcept {
    Slot *s = slot(fn);
    if (s == nullptr || wrapper == nullptr) {
        return;
    }
    if (Wrap *w = find_wrap(s, wrapper); w != nullptr) {
        w->enabled.store(false);
        w->handler.store(nullptr);
        w->user.store(nullptr);
        g_registered.fetch_sub(1);
    }
}

extern "C" int mt_hook_wrapped(u32 fn, mt_hook_fn wrapper) noexcept {
    Slot *s = slot(fn);
    if (s == nullptr || wrapper == nullptr) {
        return 0;
    }
    const Wrap *w = find_wrap(s, wrapper);
    return w != nullptr && w->enabled.load() ? 1 : 0;
}

extern "C" mt_hook_stats mt_hook_get_stats(u32 fn) noexcept {
    mt_hook_stats st{};
    if (const Slot *s = slot(fn); s != nullptr) {
        st.entries = s->entries.load();
        st.handled = s->handled.load();
        st.passed = s->passed.load();
    }
    return st;
}

extern "C" void mt_hook_reset() noexcept {
    for (std::size_t i = 0; i < k_count; i++) {
        mt_hook_unregister(k_hookable[i]);
        for (Wrap &w : g_slots[i].wraps) {
            mt_hook_unwrap(k_hookable[i], w.handler.load());
        }
        g_slots[i].entries.store(0);
        g_slots[i].handled.store(0);
        g_slots[i].passed.store(0);
    }
}

extern "C" int mt_hook_entry(mt_cpu *cpu, u32 fn, mt_guest_fn original) noexcept {
    Slot *s = slot(fn);
    if (s == nullptr) {
        return 0; // generated code hooks a function this runtime does not know: the original
    }
    s->entries.fetch_add(1, std::memory_order_relaxed);
    if (g_registered.load() == 0 ||
        dispatch(cpu, s, fn, original, static_cast<int>(k_wraps) - 1) == 0) {
        s->passed.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    s->handled.fetch_add(1, std::memory_order_relaxed);
    mt_tracef("hooks", "0x%08X: handled (lr 0x%08X)", static_cast<unsigned>(fn),
              static_cast<unsigned>(cpu->r[MT_LR]));
    return 1;
}
