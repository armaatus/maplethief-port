// The host's audio session on iOS (#197); see audio_session.h.
#include "audio_session.h"

#include <SDL3/SDL.h>

namespace maplethief::frontend {

const char *ios_audio_category(IosAudio mode) {
    return mode == IosAudio::kOwn ? "playback" : "ambient";
}

void set_sdl_audio_category(void *, const char *category) {
    SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, category);
}

const char *request_ios_audio_category(IosAudio mode, bool for_ios, AudioCategorySetter set,
                                       void *user) {
    if (!for_ios) {
        return nullptr;
    }
    const char *category = ios_audio_category(mode);
    set(user, category);
    return category;
}

void AudioInterruption::began() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (interrupted_) {
        return;
    }
    interrupted_ = true;
    stats_.began++;
    host_->pause_output();
    SDL_Log("audio: interrupted (a call, Siri, an alarm): the output is paused");
}

bool AudioInterruption::ended(const char *why) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!interrupted_) {
        return false;
    }
    if (!host_->activate_session()) {
        stats_.refused++;
        SDL_Log("audio: the session cannot be active yet (%s): the output stays paused", why);
        return false;
    }
    interrupted_ = false;
    stats_.ended++;
    host_->resume_output();
    SDL_Log("audio: the interruption is over (%s): the session is active, the output resumes", why);
    return true;
}

bool AudioInterruption::interrupted() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return interrupted_;
}

AudioInterruptionStats AudioInterruption::stats() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void AudioInterruption::reset() {
    const std::lock_guard<std::mutex> lock(mutex_);
    interrupted_ = false;
}

#if !defined(SDL_PLATFORM_IOS)
// iOS's is in platform/ios/audio_session_ios.mm.
std::unique_ptr<AudioSessionWatch> make_platform_audio_session(AudioInterruption *) {
    return nullptr;
}
#endif

} // namespace maplethief::frontend
