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
    MT_HOOK_FN_OPEN_PORTAL_URL_AND_EXIT,
    MT_HOOK_FN_MAIN_MENU_CLICK_PLAY,
};
constexpr std::size_t k_count = sizeof k_hookable / sizeof k_hookable[0];

struct Slot {
    std::atomic<mt_hook_fn> handler{nullptr};
    std::atomic<void *> user{nullptr};
    std::atomic<bool> enabled{false};
    std::atomic<u64> entries{0};
    std::atomic<u64> handled{0};
    std::atomic<u64> passed{0};
};

Slot g_slots[k_count];
// Handlers registered: 0 lets mt_hook_entry answer at once.
std::atomic<u32> g_registered{0};

Slot *slot(u32 fn) {
    for (std::size_t i = 0; i < k_count; i++) {
        if (k_hookable[i] == fn) {
            return &g_slots[i];
        }
    }
    return nullptr;
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
    const mt_hook_fn handler = g_registered.load() != 0 ? s->handler.load() : nullptr;
    if (handler == nullptr || !s->enabled.load()) {
        s->passed.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    const mt_hook_call call{fn, original, s->user.load()};
    if (handler(cpu, &call) == MT_HOOK_PASS) {
        s->passed.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    s->handled.fetch_add(1, std::memory_order_relaxed);
    mt_tracef("hooks", "0x%08X: handled (lr 0x%08X)", static_cast<unsigned>(fn),
              static_cast<unsigned>(cpu->r[MT_LR]));
    return 1;
}
