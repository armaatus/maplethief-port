// Settings applied while the game runs (#89, ADR 0076); see live_settings.h.
#include "live_settings.h"

#include <cstring>
#include <utility>

#include "audio_out.h"
#include "focus.h"
#include "gfx_backend.h"
#include "input.h"
#include "keypad_hide.h"
#include "maplethief/audio.h"
#include "maplethief/input.h"
#include "maplethief/uikit.h"
#include "perf_overlay.h"
#include "present.h"
#include "rumble.h"

namespace maplethief::frontend {

namespace {

bool is(const SettingInfo &row, const char *key) { return std::strcmp(row.key, key) == 0; }

// The popup menu's option tab (CDataMgr +0xB0: 0 status ... 5 option).
constexpr std::int32_t kOptionTab = 5;
constexpr std::int32_t kFieldPanel = 0;
constexpr std::int32_t kPopupMenuPanel = 1;

} // namespace

bool apply_live_setting(const LiveTargets &t, const SettingInfo &row, const Settings &s) {
    if (is(row, "scale_mode") || is(row, "border_color")) {
        if (t.presenter == nullptr) {
            return false;
        }
        t.presenter->set_scaling(s.scale_mode, s.border_color);
        return true;
    }
    if (is(row, "filter")) {
        // A pass needs SDL's GPU renderer, which the window has only if a filter or the GPU
        // backend was asked for at start-up.
        if (t.presenter == nullptr || t.filter_pass == nullptr || t.renderer == nullptr ||
            SDL_GetGPURendererDevice(t.renderer) == nullptr) {
            return false;
        }
        t.filter_pass->close();
        t.presenter->set_stage(PresentStage{});
        if (s.filter == ShaderFilter::kNone) {
            SDL_Log("present: filter none (the scale mode's)");
            return true;
        }
        if (!t.filter_pass->open_filter(t.renderer, s.filter)) {
            return false;
        }
        t.presenter->set_stage(t.filter_pass->stage());
        SDL_Log("present: filter %s (%s)", shader_filter_name(s.filter),
                shader_filter_shader(s.filter));
        return true;
    }
    if (is(row, "master_volume")) {
        mt_audio_set_gain(MT_AUDIO_BUS_MASTER, volume_gain(s.master_volume));
        return true;
    }
    if (is(row, "music_volume")) {
        mt_audio_set_gain(MT_AUDIO_BUS_MUSIC, volume_gain(s.music_volume));
        return true;
    }
    if (is(row, "effects_volume")) {
        mt_audio_set_gain(MT_AUDIO_BUS_EFFECTS, volume_gain(s.effects_volume));
        return true;
    }
    if (is(row, "mute_unfocused")) {
        if (t.focus_mute == nullptr) {
            return false;
        }
        t.focus_mute->set_enabled(s.mute_unfocused && t.focus_mute_allowed);
        return true;
    }
    if (is(row, "right_touch")) {
        if (t.input == nullptr) {
            return false;
        }
        t.input->set_right_touch(s.right_touch);
        return true;
    }
    if (is(row, "hide_keypad")) {
        if (t.keypad == nullptr) {
            return false;
        }
        t.keypad->set_mode(s.hide_keypad, t.now);
        return true;
    }
    if (is(row, "rumble") || is(row, "rumble_strength")) {
        if (t.rumble == nullptr) {
            return false;
        }
        t.rumble->configure(s.rumble, s.rumble_strength);
        return true;
    }
    if (is(row, "perf_overlay")) {
        if (t.perf == nullptr) {
            return false;
        }
        t.perf->set_on(s.perf_overlay);
        return true;
    }
    return false;
}

GameOptionsOpener::GameOptionsOpener(Press press, Panel panel, Touch touch)
    : press_(std::move(press)), panel_(std::move(panel)), touch_(std::move(touch)) {}

bool GameOptionsOpener::available() const {
    return state_ == State::kIdle && panel_ && panel_() == kFieldPanel;
}

bool GameOptionsOpener::start() {
    if (!available()) {
        outcome_ = Outcome::kNotInField;
        return false;
    }
    popup_menu_tab_point(kOptionTab, &x_, &y_);
    outcome_ = Outcome::kNone;
    state_ = State::kPress;
    return true;
}

void GameOptionsOpener::turn(double now) {
    switch (state_) {
    case State::kIdle:
        return;
    case State::kPress:
        press_(MT_ACTION_MENU, true);
        state_ = State::kRelease;
        return;
    case State::kRelease:
        press_(MT_ACTION_MENU, false);
        state_ = State::kWaitMenu;
        until_ = now + kWaitSeconds;
        return;
    case State::kWaitMenu:
        if (panel_() == kPopupMenuPanel) {
            state_ = State::kSettle;
            until_ = now + kSettleSeconds;
        } else if (now >= until_) {
            SDL_Log("settings: game options: the popup menu did not come up");
            state_ = State::kIdle;
            outcome_ = Outcome::kNoMenu;
        }
        return;
    case State::kSettle:
        if (panel_() != kPopupMenuPanel) {
            state_ = State::kIdle;
            outcome_ = Outcome::kNoMenu;
        } else if (now >= until_) {
            touch_(MT_UIKIT_TOUCH_BEGAN, x_, y_);
            state_ = State::kLift;
        }
        return;
    case State::kLift:
        touch_(MT_UIKIT_TOUCH_ENDED, x_, y_);
        state_ = State::kIdle;
        outcome_ = Outcome::kOpened;
        SDL_Log("settings: game options: the popup menu's option tab");
        return;
    }
}

} // namespace maplethief::frontend
