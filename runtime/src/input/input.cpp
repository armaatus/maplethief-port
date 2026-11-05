// Game actions (#67, ADR 0048); see maplethief/input.h. The guest facts are from
// docs/controls/keypad.md (#66), docs/re/program-flow.md and the ivar offsets of objc.json.
#include "maplethief/input.h"

#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "maplethief/diag.h"
#include "maplethief/hle/abi.h"
#include "maplethief/memory.h"
#include "maplethief/objc/host.h"
#include "maplethief/objc/objc.h"
#include "snapshot/snapshot_internal.h"
#include "uikit/uikit_internal.h"

namespace {

using u32 = std::uint32_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

// The fixed table, in mt_input_action order. Pad keys are TouchKeyPadView's BOOL properties
// that SkyCanvas::Tick 0x2590D polls; codes are the phone key codes of Clet_HandleEvent.
const mt_input_action_info k_actions[MT_ACTION_COUNT] = {
    {nullptr, nullptr, nullptr, 0},
    {"up", "Up", "onUpKey", -1},
    {"down", "Down", "onDownKey", -2},
    {"left", "Left", "onLeftKey", -3},
    {"right", "Right", "onRightKey", -4},
    {"jump", "Jump", "onJumpKey", '2'},
    {"attack", "Attack", "onAttackKey", '5'},
    {"slot1", "Quick slot 1", "onShort1Key", '7'},
    {"slot2", "Quick slot 2", "onShort2Key", '9'},
    {"slot3", "Quick slot 3", "onShort3Key", '0'},
    {"slot4", "Quick slot 4", "onShort4Key", '#'},
    {"ok", "OK", nullptr, -5},
    {"back", "Back", nullptr, -16},
    {"menu", "Menu", nullptr, -16},
    {"map", "Map", nullptr, '*'},
    {"cash_shop", "Cash shop", nullptr, -26},
    {"minus", "Minus", nullptr, '4'},
    {"plus", "Plus", nullptr, '6'},
    {"revive", "Revive", nullptr, -25},
    {"grade_revive", "Grade revive", nullptr, -35},
    // The port's own (the frontend's; never delivered).
    {"fast_forward", "Fast-forward", nullptr, 0},
    {"auto_run", "Auto-run", nullptr, 0},
    {"page", "Page (with up / down)", nullptr, 0},
    {"pointer_tap", "Pointer tap", nullptr, 0},
};

// EAGLView's ivars (objc.json): touchKeyPadView, statusOfTouchKeyPad.
constexpr u32 k_eagl_touch_keypad = 84;
constexpr u32 k_eagl_status = 92;
// TouchKeyPadView's touchKeyPadEnable.
constexpr u32 k_pad_enable = 228;
// SkyCanvas's pending press and release; -1000 = none (program-flow.md, SkyCanvas::Tick).
constexpr u32 k_canvas_press = 0xD8;
constexpr u32 k_canvas_release = 0xE8;
constexpr i32 k_no_key = -1000;
// Clet_HandleEvent's event types.
constexpr u32 k_event_press = 2;
constexpr u32 k_event_release = 3;

enum class Route { none, pad, key };

struct Event {
    u32 action;
    bool down;
};

struct State {
    mt_input_config config = mt_input_config_default();
    mt_objc *rt = nullptr;
    // The guest objects, re-read every turn.
    u32 eagl = 0;
    i32 panel = -1;
    u32 pad = 0;
    bool live = false;

    u32 held[MT_ACTION_COUNT] = {};
    std::deque<Event> queue;
    // How each held action reached the guest, and the pad object its BOOL was set on.
    Route route[MT_ACTION_COUNT] = {};
    u32 route_pad[MT_ACTION_COUNT] = {};

    bool have_tick = false;
    i64 tick = 0;         // lastFrame at the last turn
    bool stepped = false; // a step was applied and its tick has not come yet
    i64 step_tick = 0;    // lastFrame when it was applied
    mt_input_stats stats{};
};

State g;

bool valid_action(u32 a) { return a > MT_ACTION_NONE && a < MT_ACTION_COUNT; }
// An action the guest gets (not one of the port's own, which the frontend handles).
bool guest_action(u32 a) { return a > MT_ACTION_NONE && a < MT_ACTION_FIRST_PORT; }

// Whether [addr, addr + size) is guest memory that is there: an image segment or the committed
// heap. The game's globals are in its image; a synthetic guest (a test) may have no image, and
// its arena's other pages are not readable.
bool readable(const mt_cpu *cpu, u32 addr, u32 size) {
    const std::uint64_t end = static_cast<std::uint64_t>(addr) + size;
    if (addr == 0 || end > cpu->mem_size) {
        return false;
    }
    mt_mem *mem = mt_mem_from_cpu(cpu);
    if (mem == nullptr) {
        return false;
    }
    u32 n = 0;
    const mt_segment *segs = mt_mem_segments(mem, &n);
    for (u32 i = 0; segs != nullptr && i < n; i++) {
        if (addr >= segs[i].vmaddr &&
            end <= static_cast<std::uint64_t>(segs[i].vmaddr) + segs[i].vmsize) {
            return true;
        }
    }
    mt_heap_stats heap{};
    mt_heap_get_stats(mem, &heap);
    return addr >= MT_GUEST_HEAP_BASE && end <= heap.committed;
}
u32 rd32(const mt_cpu *cpu, u32 addr) {
    u32 v = 0;
    if (readable(cpu, addr, 4)) {
        std::memcpy(&v, cpu->mem + addr, 4);
    }
    return v;
}
i64 rd64(const mt_cpu *cpu, u32 addr) {
    i64 v = 0;
    if (readable(cpu, addr, 8)) {
        std::memcpy(&v, cpu->mem + addr, 8);
    }
    return v;
}
std::uint8_t rd8(const mt_cpu *cpu, u32 addr) {
    return readable(cpu, addr, 1) ? cpu->mem[addr] : 0;
}

const char *name_of(u32 a) { return valid_action(a) ? k_actions[a].name : "?"; }

// The EAGLView in the windows (depth first), or 0. The game has exactly one.
u32 find_eagl(mt_cpu *cpu) {
    const mt_ui::State &st = mt_ui::state(cpu);
    std::vector<u32> stack(st.windows.rbegin(), st.windows.rend());
    while (!stack.empty() && stack.size() < 100000) {
        const u32 obj = stack.back();
        stack.pop_back();
        const mt_ui::View *v = mt_ui::view_of(cpu, obj);
        if (v == nullptr) {
            continue;
        }
        if (mt_ui::is_kind(cpu, obj, "EAGLView")) {
            return obj;
        }
        stack.insert(stack.end(), v->subviews.rbegin(), v->subviews.rend());
    }
    return 0;
}

// The panel the key pad still shows where statusOfTouchKeyPad is -1, or -1 (#127): a popup over
// the shop (enableAlertKey, panel 7) is closed by disableAlertKey, which sets the status to -1
// and hides panel 7 but leaves the shop's panel 6 shown and live, and the shop goes on without
// enabling it again. The screen is still the shop, so its keys are the shop's. Panels 1-10
// but 7, by the views' own values (hidden, alpha), the key pad's view included.
i32 shown_panel(u32 pad) {
    const mt_ui::View *v = mt_ui::view_of(nullptr, pad);
    if (v == nullptr || v->hidden || v->model.alpha < 0.01) {
        return -1;
    }
    for (std::size_t i = 1; i < v->subviews.size() && i <= 10; i++) {
        const mt_ui::View *p = mt_ui::view_of(nullptr, v->subviews[i]);
        if (i != 7 && p != nullptr && !p->hidden && p->model.alpha >= 0.01) {
            return static_cast<i32>(i);
        }
    }
    return -1;
}

// Reads the panel, the pad and whether it is live.
void refresh(mt_cpu *cpu) {
    mt_objc *rt = mt_objc_current();
    if (rt != g.rt) {
        // A new runtime (a new process or test): its objects are not the old ones.
        g.rt = rt;
        g.eagl = 0;
    }
    if (g.eagl == 0 || !mt_ui::is_kind(cpu, g.eagl, "EAGLView")) {
        g.eagl = rt != nullptr ? find_eagl(cpu) : 0u;
    }
    if (g.eagl == 0) {
        g.panel = -1;
        g.pad = 0;
        g.live = false;
        return;
    }
    g.panel = static_cast<i32>(rd32(cpu, g.eagl + k_eagl_status));
    g.pad = rd32(cpu, g.eagl + k_eagl_touch_keypad);
    g.live = g.pad != 0 && rd8(cpu, g.pad + k_pad_enable) != 0;
    if (g.panel == -1 && g.pad != 0 && mt_objc_current() == g.rt) {
        g.panel = shown_panel(g.pad);
    }
}

void enqueue(u32 action, bool down) {
    g.queue.push_back(Event{action, down});
    g.stats.queued = static_cast<u32>(g.queue.size());
}

std::string setter_of(const char *key) {
    std::string s = "set";
    s += key;
    s[3] = static_cast<char>(s[3] - 'a' + 'A');
    s += ':';
    return s;
}

bool is_direction(u32 a) {
    return a == MT_ACTION_UP || a == MT_ACTION_DOWN || a == MT_ACTION_LEFT || a == MT_ACTION_RIGHT;
}

// The route the front event takes now: a press by what the pad is now, a release the way its
// press went.
Route route_now(const Event &e) {
    if (!e.down) {
        return g.route[e.action] == Route::pad ? Route::pad : Route::key;
    }
    return g.live && k_actions[e.action].pad_key != nullptr ? Route::pad : Route::key;
}

void count(bool down) {
    if (down) {
        g.stats.presses++;
    } else {
        g.stats.releases++;
    }
}

// One step: a batch of BOOL changes for different keys, or one key event.
void step(mt_cpu *cpu) {
    const Route first = route_now(g.queue.front());
    if (first == Route::pad) {
        bool touched[MT_ACTION_COUNT] = {};
        bool direction = false;
        std::vector<u32> pads;
        while (!g.queue.empty()) {
            const Event e = g.queue.front();
            if (route_now(e) != Route::pad || touched[e.action]) {
                break;
            }
            touched[e.action] = true;
            g.queue.pop_front();
            const u32 pad = e.down ? g.pad : g.route_pad[e.action];
            g.route[e.action] = e.down ? Route::pad : Route::none;
            g.route_pad[e.action] = e.down ? pad : 0u;
            const std::string setter = setter_of(k_actions[e.action].pad_key);
            mt_objc_send1(cpu, pad, setter.c_str(), e.down ? 1u : 0u);
            mt_tracef("input", "%s %s: [TouchKeyPadView %s%u] (tick %lld)",
                      e.down ? "press" : "release", name_of(e.action), setter.c_str(),
                      e.down ? 1u : 0u, static_cast<long long>(g.tick));
            count(e.down);
            g.stats.pad_edges++;
            direction = direction || is_direction(e.action);
            bool seen = false;
            for (const u32 p : pads) {
                seen = seen || p == pad;
            }
            if (!seen) {
                pads.push_back(pad);
            }
        }
        // As the touch handlers do: the d-pad's pressed images follow the BOOLs.
        if (direction) {
            for (const u32 p : pads) {
                if (mt_ui::responds(cpu, p, "checkCrossKeyPad")) {
                    mt_objc_send0(cpu, p, "checkCrossKeyPad");
                }
            }
        }
    } else {
        const Event e = g.queue.front();
        g.queue.pop_front();
        g.route[e.action] = e.down ? Route::key : Route::none;
        g.route_pad[e.action] = 0;
        const u32 args[3] = {e.down ? k_event_press : k_event_release,
                             static_cast<u32>(k_actions[e.action].code), 0u};
        const u32 fn = g.config.handle_event;
        mt_call_guest(cpu, fn, args, 3, fn);
        mt_tracef("input", "%s %s: Clet_HandleEvent(%u, %d, 0) (tick %lld)",
                  e.down ? "press" : "release", name_of(e.action), args[0],
                  k_actions[e.action].code, static_cast<long long>(g.tick));
        count(e.down);
        g.stats.key_events++;
    }
    g.stats.steps++;
    g.stats.queued = static_cast<u32>(g.queue.size());
    g.stepped = true;
    g.step_tick = g.tick;
}

// --- save states (#203, ADR 0105) ---------------------------------------------------------------
//
// The delivery between turns: the guest objects seen, what is held and how it went in, the
// queue, and the tick the last step waits for. At a safe point nothing is held or queued; a
// restore puts the state's (released) input in place of whatever the present holds, so the
// next step is timed as it was in the saved run. The configuration and counters stay.

bool save_input(mt_cpu *, mt_snap::Writer &w, std::string *) {
    w.u32(g.eagl);
    w.u32(static_cast<u32>(g.panel));
    w.u32(g.pad);
    w.boolean(g.live);
    for (u32 a = 0; a < MT_ACTION_COUNT; a++) {
        w.u32(g.held[a]);
        w.u8v(static_cast<std::uint8_t>(g.route[a]));
        w.u32(g.route_pad[a]);
    }
    w.u32(static_cast<u32>(g.queue.size()));
    for (const Event &e : g.queue) {
        w.u32(e.action);
        w.boolean(e.down);
    }
    w.boolean(g.have_tick);
    w.i64(g.tick);
    w.boolean(g.stepped);
    w.i64(g.step_tick);
    return true;
}

bool load_input(mt_cpu *, mt_snap::Reader &r, std::function<void(mt_cpu *)> *apply) {
    State s;
    u32 panel = 0, n = 0;
    if (!r.u32(&s.eagl) || !r.u32(&panel) || !r.u32(&s.pad) || !r.boolean(&s.live)) {
        return false;
    }
    s.panel = static_cast<i32>(panel);
    for (u32 a = 0; a < MT_ACTION_COUNT; a++) {
        std::uint8_t route = 0;
        if (!r.u32(&s.held[a]) || !r.u8v(&route) || !r.u32(&s.route_pad[a])) {
            return false;
        }
        if (route > static_cast<std::uint8_t>(Route::key)) {
            return r.fail("an input route of no kind");
        }
        s.route[a] = static_cast<Route>(route);
    }
    if (!r.u32(&n) || !r.count_fits(n, 5)) {
        return false;
    }
    for (u32 i = 0; i < n; i++) {
        Event e{};
        if (!r.u32(&e.action) || !r.boolean(&e.down)) {
            return false;
        }
        if (!valid_action(e.action)) {
            return r.fail("a queued input of no action");
        }
        s.queue.push_back(e);
    }
    if (!r.boolean(&s.have_tick) || !r.i64(&s.tick) || !r.boolean(&s.stepped) ||
        !r.i64(&s.step_tick)) {
        return false;
    }
    *apply = [s](mt_cpu *) {
        const mt_input_config config = g.config;
        mt_objc *rt = g.rt;
        mt_input_stats stats = g.stats;
        g = s;
        g.config = config;
        g.rt = rt;
        stats.queued = static_cast<u32>(g.queue.size());
        g.stats = stats;
    };
    return true;
}

} // namespace

namespace mt_input_snap {
extern const mt_snap::Module k_snap_input;
const mt_snap::Module k_snap_input = {"input", save_input, load_input};
} // namespace mt_input_snap

extern "C" mt_input_config mt_input_config_default(void) noexcept {
    mt_input_config c{};
    c.handle_event = 0x5A75u;    // Clet_HandleEvent
    c.canvas_global = 0x11A950u; // SkyCanvas::GetInstance's instance
    c.last_frame = 0x11A4D8u;    // CMTimer's lastFrame
    return c;
}

extern "C" void mt_input_configure(const mt_input_config *config) noexcept {
    g = State();
    g.config = config != nullptr ? *config : mt_input_config_default();
}

extern "C" const mt_input_action_info *mt_input_action_get_info(uint32_t action) noexcept {
    return valid_action(action) ? &k_actions[action] : nullptr;
}

extern "C" uint32_t mt_input_action_named(const char *name) noexcept {
    if (name == nullptr) {
        return MT_ACTION_NONE;
    }
    for (u32 a = 1; a < MT_ACTION_COUNT; a++) {
        if (std::strcmp(k_actions[a].name, name) == 0) {
            return a;
        }
    }
    return MT_ACTION_NONE;
}

extern "C" void mt_input_press(uint32_t action) noexcept {
    if (!guest_action(action)) {
        return;
    }
    if (g.held[action]++ == 0) {
        enqueue(action, true);
    }
}

extern "C" void mt_input_release(uint32_t action) noexcept {
    if (!guest_action(action) || g.held[action] == 0) {
        return;
    }
    if (--g.held[action] == 0) {
        enqueue(action, false);
    }
}

extern "C" uint32_t mt_input_held(uint32_t action) noexcept {
    return valid_action(action) ? g.held[action] : 0u;
}

extern "C" void mt_input_release_all(void) noexcept {
    for (u32 a = 1; a < MT_ACTION_COUNT; a++) {
        if (g.held[a] != 0) {
            g.held[a] = 0;
            enqueue(a, false);
        }
    }
}

extern "C" int32_t mt_input_panel(void) noexcept { return g.panel; }

extern "C" int mt_input_pad_live(void) noexcept { return g.live ? 1 : 0; }

extern "C" std::uint32_t mt_input_pad_panel_view(std::int32_t index) noexcept {
    if (g.pad == 0 || index < 0 || mt_objc_current() == nullptr || mt_objc_current() != g.rt) {
        return 0;
    }
    const mt_ui::View *v = mt_ui::view_of(nullptr, g.pad);
    if (v == nullptr || static_cast<std::size_t>(index) >= v->subviews.size()) {
        return 0;
    }
    return v->subviews[static_cast<std::size_t>(index)];
}

extern "C" std::uint32_t mt_input_pad_view() noexcept {
    return mt_objc_current() != nullptr && mt_objc_current() == g.rt ? g.pad : 0u;
}

extern "C" void mt_input_turn(mt_cpu *cpu) noexcept {
    refresh(cpu);
    const i64 tick = rd64(cpu, g.config.last_frame);
    if (!g.have_tick || tick != g.tick) {
        if (g.have_tick) {
            g.stats.ticks++;
        }
        g.have_tick = true;
        g.tick = tick;
    }
    if (g.queue.empty()) {
        return;
    }
    // One step per tick: the last one has to have been seen.
    if (g.stepped && g.tick == g.step_tick) {
        return;
    }
    g.stepped = false;
    // The engine has to be up, and both of SkyCanvas's slots empty: a key event would be
    // dropped, and the pad is not polled while one waits.
    const u32 canvas = rd32(cpu, g.config.canvas_global);
    if (canvas == 0) {
        return;
    }
    if (static_cast<i32>(rd32(cpu, canvas + k_canvas_press)) != k_no_key ||
        static_cast<i32>(rd32(cpu, canvas + k_canvas_release)) != k_no_key) {
        g.stats.waits_busy++;
        return;
    }
    step(cpu);
}

extern "C" mt_input_stats mt_input_get_stats(void) noexcept {
    mt_input_stats s = g.stats;
    s.queued = static_cast<u32>(g.queue.size());
    return s;
}
