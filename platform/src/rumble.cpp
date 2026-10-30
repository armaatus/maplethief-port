// Rumble (#72, ADR 0053); see rumble.h.
#include "rumble.h"

#include <algorithm>
#include <cmath>

#include "gamepad.h"
#include "maplethief/audio.h"

namespace maplethief::frontend {

namespace {

void vibrate_hook(void *user) { static_cast<Rumble *>(user)->on_vibrate(); }

void event_hook(void *user, const mt_game_event *e) {
    if (e != nullptr) {
        static_cast<Rumble *>(user)->on_event(*e);
    }
}

// An amplitude 0..1 at `strength` percent as SDL's 0..0xFFFF.
Uint16 scaled(float amplitude, std::uint32_t strength) {
    const float a = std::clamp(amplitude, 0.0f, 1.0f) * static_cast<float>(strength) / 100.0f;
    return static_cast<Uint16>(std::lround(a * 65535.0f));
}

} // namespace

Rumble::~Rumble() {
    uninstall();
    stop();
}

void Rumble::configure(bool on, std::uint32_t strength) {
    on_ = on;
    strength_ = std::min<std::uint32_t>(strength, 100);
    if (!on_ || strength_ == 0) {
        pending_ = false;
        merged_ = {};
    }
}

void Rumble::install() {
    mt_audio_set_vibrate_hook(vibrate_hook, this);
    mt_game_events_set_hook(event_hook, this);
    installed_ = true;
}

void Rumble::uninstall() {
    if (installed_) {
        mt_audio_set_vibrate_hook(nullptr, nullptr);
        mt_game_events_set_hook(nullptr, nullptr);
        installed_ = false;
    }
}

void Rumble::request(const RumbleEffect &e) {
    if (!on_ || strength_ == 0) {
        stats_.ignored++;
        return;
    }
    stats_.requests++;
    merged_.low = std::max(merged_.low, e.low);
    merged_.high = std::max(merged_.high, e.high);
    merged_.ms = std::max(merged_.ms, e.ms);
    pending_ = true;
}

void Rumble::on_event(const mt_game_event &e) {
    switch (e.kind) {
    case MT_GAME_EVENT_HERO_DAMAGE:
        request(kRumbleDamage);
        break;
    case MT_GAME_EVENT_LEVEL_UP:
        request(kRumbleLevelUp);
        break;
    default:
        break;
    }
}

void Rumble::turn() {
    if (!pending_) {
        return;
    }
    const RumbleEffect e = merged_;
    pending_ = false;
    merged_ = {};
    SDL_Gamepad *pad = pads_ != nullptr ? pads_->active() : nullptr;
    if (pad == nullptr) {
        stats_.no_pad++;
        return;
    }
    if (!SDL_RumbleGamepad(pad, scaled(e.low, strength_), scaled(e.high, strength_), e.ms)) {
        if (stats_.failed++ == 0) {
            SDL_Log("rumble: the gamepad cannot rumble: %s", SDL_GetError());
        }
        return;
    }
    stats_.sent++;
    rumbling_ = pads_->active_id();
}

void Rumble::stop() {
    if (rumbling_ == 0) {
        return;
    }
    // The pad may have been unplugged (and closed) since: then there is nothing to stop.
    if (SDL_Gamepad *pad = SDL_GetGamepadFromID(rumbling_); pad != nullptr) {
        SDL_RumbleGamepad(pad, 0, 0, 0);
    }
    rumbling_ = 0;
}

} // namespace maplethief::frontend
