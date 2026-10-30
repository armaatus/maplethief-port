// Menu repeat and the popup confirm guard (#147, ADR 0051's addendum); see menu_input.h.
#include "menu_input.h"

#include <SDL3/SDL.h>

#include <cstring>

#include "maplethief/cf.h"
#include "maplethief/memory.h"

namespace maplethief::frontend {

namespace {

// The open popup's id (docs/controls/keypad.md): Popup_Open 0x3EAA1 sets it, Popup_Close
// 0x3E945 sets -1.
constexpr std::uint32_t kPopupId = 0x108C64;

void runtime_forward(void *, std::uint32_t action, bool down) {
    if (down) {
        mt_input_press(action);
    } else {
        mt_input_release(action);
    }
}

std::int32_t runtime_panel(void *) { return mt_input_panel(); }
std::int32_t runtime_popup(void *, mt_cpu *cpu) { return menu_popup_id(cpu); }
double runtime_clock(void *) { return mt_cf_absolute_time(); }
bool runtime_busy(void *) { return mt_input_get_stats().queued > 0; }

// The direction of an action (0 up ... 3 right, Direction's order), or -1.
int direction_of(std::uint32_t action) {
    switch (action) {
    case MT_ACTION_UP:
        return static_cast<int>(Direction::kUp);
    case MT_ACTION_DOWN:
        return static_cast<int>(Direction::kDown);
    case MT_ACTION_LEFT:
        return static_cast<int>(Direction::kLeft);
    case MT_ACTION_RIGHT:
        return static_cast<int>(Direction::kRight);
    default:
        return -1;
    }
}

constexpr std::uint32_t kDirectionActions[4] = {MT_ACTION_UP, MT_ACTION_DOWN, MT_ACTION_LEFT,
                                                MT_ACTION_RIGHT};
constexpr std::uint32_t kGuarded[4] = {MT_ACTION_OK, MT_ACTION_BACK, MT_ACTION_REVIVE,
                                       MT_ACTION_GRADE_REVIVE};

const char *action_name(std::uint32_t action) {
    const mt_input_action_info *info = mt_input_action_get_info(action);
    return info != nullptr ? info->name : "none";
}

} // namespace

bool menu_repeat_panel(std::int32_t panel) {
    return panel == 1 || panel == 5 || panel == 6 || panel == 8;
}

bool confirm_guarded(std::uint32_t action) {
    for (const std::uint32_t a : kGuarded) {
        if (a == action) {
            return true;
        }
    }
    return false;
}

std::int32_t menu_popup_id(const mt_cpu *cpu) {
    if (cpu == nullptr) {
        return -1;
    }
    if (const mt_mem *mem = mt_mem_from_cpu(cpu)) {
        std::uint32_t n = 0;
        const mt_segment *segs = mt_mem_segments(mem, &n);
        bool mapped = false;
        for (std::uint32_t i = 0; i < n && !mapped; i++) {
            mapped = kPopupId >= segs[i].vmaddr &&
                     kPopupId + 4ull <= std::uint64_t{segs[i].vmaddr} + segs[i].vmsize;
        }
        if (!mapped) {
            return -1;
        }
    }
    if (kPopupId + 4ull > cpu->mem_size) {
        return -1;
    }
    std::int32_t id = -1;
    std::memcpy(&id, cpu->mem + kPopupId, 4);
    return id;
}

MenuInput::MenuInput()
    : forward_(runtime_forward), panel_(runtime_panel), popup_source_(runtime_popup),
      clock_(runtime_clock), busy_(runtime_busy) {}

void MenuInput::sink(void *user, std::uint32_t action, bool down) {
    static_cast<MenuInput *>(user)->action(action, down);
}

void MenuInput::set_repeat(bool on) {
    if (repeat_ != on) {
        SDL_Log("menu: menu_repeat %s", on ? "on" : "off");
    }
    repeat_ = on;
    if (!on) {
        repeat_dir_ = -1;
    }
}

void MenuInput::set_guard(bool on) {
    if (guard_ != on) {
        SDL_Log("menu: confirm_guard %s", on ? "on" : "off");
    }
    guard_ = on;
}

void MenuInput::set_forward(ActionSink sink, void *user) {
    forward_ = sink != nullptr ? sink : runtime_forward;
    forward_user_ = sink != nullptr ? user : nullptr;
}

void MenuInput::set_panel_source(PanelSource source, void *user) {
    panel_ = source != nullptr ? source : runtime_panel;
    panel_user_ = source != nullptr ? user : nullptr;
}

void MenuInput::set_popup_source(PopupSource source, void *user) {
    popup_source_ = source != nullptr ? source : runtime_popup;
    popup_user_ = source != nullptr ? user : nullptr;
}

void MenuInput::set_clock(ClockSource source, void *user) {
    clock_ = source != nullptr ? source : runtime_clock;
    clock_user_ = source != nullptr ? user : nullptr;
}

void MenuInput::set_busy_source(BusySource source, void *user) {
    busy_ = source != nullptr ? source : runtime_busy;
    busy_user_ = source != nullptr ? user : nullptr;
}

void MenuInput::forward(std::uint32_t action, bool down) {
    stats_.forwarded++;
    forward_(forward_user_, action, down);
}

void MenuInput::observe(double now) {
    const std::int32_t id = popup_source_(popup_user_, cpu_);
    if (id >= 0 && id != popup_) {
        opened_at_ = now;
        stats_.popups++;
        if (guard_) {
            SDL_Log("menu: popup %d opened: no confirm or back for %.0f ms", id, kGuardTime * 1000);
        }
    }
    popup_ = id;
}

bool MenuInput::guarding(double now) const {
    if (!guard_ || popup_ < 0) {
        return false;
    }
    if (now - opened_at_ < kGuardTime) {
        return true;
    }
    if (actions_ != nullptr) {
        for (const std::uint32_t a : kGuarded) {
            if (actions_->held_from_field(a)) {
                return true;
            }
        }
    }
    return false;
}

void MenuInput::action(std::uint32_t action, bool down) {
    const int d = direction_of(action);
    if (down) {
        if (guard_ && confirm_guarded(action)) {
            const double now = clock_(clock_user_);
            observe(now);
            if (guarding(now)) {
                dropped_[action]++;
                stats_.guarded++;
                stats_.held += now - opened_at_ < kGuardTime ? 0 : 1;
                SDL_Log("menu: %s dropped on popup %d (%.0f ms after it opened%s)",
                        action_name(action), popup_, (now - opened_at_) * 1000,
                        now - opened_at_ < kGuardTime ? ""
                                                      : ", a button still held from the field");
                return;
            }
        }
        if (d >= 0) {
            held_[d]++;
            const std::int32_t panel = panel_(panel_user_);
            if (repeat_ && menu_repeat_panel(panel)) {
                repeat_dir_ = d;
                repeat_panel_ = panel;
                repeat_at_ = clock_(clock_user_) + kRepeatDelay;
            } else {
                repeat_dir_ = -1;
            }
        }
    } else {
        if (action < MT_ACTION_COUNT && dropped_[action] > 0) {
            dropped_[action]--;
            return;
        }
        if (d >= 0) {
            if (held_[d] > 0) {
                held_[d]--;
            }
            if (repeat_dir_ == d && held_[d] == 0) {
                repeat_dir_ = -1;
            }
        }
    }
    forward(action, down);
}

void MenuInput::turn(mt_cpu *cpu, double now) {
    cpu_ = cpu;
    observe(now);
    if (repeat_dir_ < 0) {
        return;
    }
    const std::int32_t panel = panel_(panel_user_);
    if (!repeat_ || panel != repeat_panel_ || held_[repeat_dir_] == 0) {
        repeat_dir_ = -1;
        return;
    }
    if (now < repeat_at_) {
        return;
    }
    if (busy_(busy_user_)) {
        stats_.waits++;
        return;
    }
    // Another key press: every press of it let go, then pressed again (the runtime counts
    // presses per action, and the guest sees the first press and the last release).
    const std::uint32_t a = kDirectionActions[repeat_dir_];
    const std::uint32_t n = held_[repeat_dir_];
    for (std::uint32_t i = 0; i < n; i++) {
        forward(a, false);
    }
    for (std::uint32_t i = 0; i < n; i++) {
        forward(a, true);
    }
    stats_.repeats++;
    // On the beat while the repeats come on time; after a wait, from now.
    repeat_at_ += kRepeatEvery;
    if (repeat_at_ <= now) {
        repeat_at_ = now + kRepeatEvery;
    }
    SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "menu: %s repeats on panel %d at %.3f s", action_name(a),
                 static_cast<int>(panel), now);
}

} // namespace maplethief::frontend
