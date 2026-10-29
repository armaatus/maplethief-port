// AVAudioSession and AVAudioPlayer, silent (#117, ADR 0038, maplethief/audio.h), with exactly
// the selectors the game sends (docs/re/host-surface.md, AVFoundation):
//
//   +[AVAudioSession sharedInstance]  -setCategory:error:
//   +[AVAudioPlayer alloc] (NSObject's)  -initWithContentsOfURL:error:  -prepareToPlay  -play
//   -stop  -isPlaying  -setVolume:  -setNumberOfLoops:  -setCurrentTime:
//
// CMSoundPlayer makes one player per music track, looping for ever (setNumberOfLoops:-1), and
// stops and plays them as the maps change (docs/re/audio.md). A player plays from -play until
// -stop: the music loops, so the play state is what the device would say. #63 plays the MP3s.
#include <cstdint>
#include <fstream>
#include <string>

#include "audio/audio_internal.h"
#include "foundation/foundation.h"
#include "maplethief/diag.h"

namespace mt_audio {
namespace {

// --- AVAudioSession ---------------------------------------------------------------------------

struct SessionState {
    u32 category; // NSString, retained
};
void session_destroy(mt_cpu *cpu, u32, void *block) {
    mt_objc_release(cpu, static_cast<SessionState *>(block)->category);
}
const mt_objc_state_type k_session_state = {"AVAudioSession", sizeof(SessionState),
                                            session_destroy};

// +[AVAudioSession sharedInstance]
void session_shared(mt_cpu *cpu) {
    stats().calls++;
    mt_ret_u32(cpu, mt_ns::singleton(cpu, "AVAudioSession"));
}

// -[AVAudioSession setCategory:(NSString *)category error:(NSError **)outError] -> BOOL
void session_set_category(mt_cpu *cpu) {
    stats().calls++;
    const u32 self = mt_objc_self(cpu);
    mt_args a = mt_objc_args(cpu);
    const u32 category = mt_arg_u32(&a);
    auto *s = static_cast<SessionState *>(mt_objc_state(mt_objc_current(), self, &k_session_state));
    if (s != nullptr && category != s->category) {
        mt_objc_retain(cpu, category);
        mt_objc_release(cpu, s->category);
        s->category = category;
    }
    mt_tracef("audio", "-[AVAudioSession setCategory:%s error:]",
              mt_ns::string_utf8(cpu, category).c_str());
    mt_ret_u32(cpu, 1);
}

// --- AVAudioPlayer ----------------------------------------------------------------------------

struct PlayerState {
    u32 url; // retained
    bool playing;
    float volume;
    std::int32_t loops;
    double time;
};
void player_destroy(mt_cpu *cpu, u32, void *block) {
    auto *s = static_cast<PlayerState *>(block);
    if (s->playing) {
        stats().players_playing--;
    }
    mt_objc_release(cpu, s->url);
}
const mt_objc_state_type k_player_state = {"AVAudioPlayer", sizeof(PlayerState), player_destroy};

PlayerState *player(mt_cpu *cpu) {
    stats().calls++;
    return static_cast<PlayerState *>(
        mt_objc_find_state(mt_objc_current(), mt_objc_self(cpu), &k_player_state));
}

// -[AVAudioPlayer initWithContentsOfURL:(NSURL *)url error:(NSError **)outError]: a player
// for a file that exists (nil otherwise, as AVFoundation fails to open it).
void player_init(mt_cpu *cpu) {
    stats().calls++;
    const u32 self = mt_objc_self(cpu);
    mt_args a = mt_objc_args(cpu);
    const u32 url = mt_arg_u32(&a);
    const std::string path = url_host_path(cpu, url);
    const bool exists = !path.empty() && std::ifstream(path, std::ios::binary).good();
    auto *s =
        exists ? static_cast<PlayerState *>(mt_objc_state(mt_objc_current(), self, &k_player_state))
               : nullptr;
    if (s == nullptr) {
        mt_tracef("audio", "-[AVAudioPlayer initWithContentsOfURL:%s error:]: no such file: nil",
                  path.c_str());
        mt_objc_release(cpu, self);
        mt_ret_u32(cpu, 0);
        return;
    }
    s->url = mt_objc_retain(cpu, url);
    s->volume = 1.0f;
    stats().players++;
    mt_tracef("audio", "-[AVAudioPlayer initWithContentsOfURL:%s error:] -> 0x%08X", path.c_str(),
              static_cast<unsigned>(self));
    mt_ret_u32(cpu, self);
}

void player_prepare(mt_cpu *cpu) {
    player(cpu);
    mt_ret_u32(cpu, 1);
}

void player_play(mt_cpu *cpu) {
    PlayerState *s = player(cpu);
    if (s != nullptr && !s->playing) {
        s->playing = true;
        stats().players_playing++;
    }
    mt_tracef("audio", "-[AVAudioPlayer play] 0x%08X", static_cast<unsigned>(mt_objc_self(cpu)));
    mt_ret_u32(cpu, s != nullptr ? 1u : 0u);
}

// -stop keeps the position (a later -play resumes there), as AVAudioPlayer does.
void player_stop(mt_cpu *cpu) {
    PlayerState *s = player(cpu);
    if (s != nullptr && s->playing) {
        s->playing = false;
        stats().players_playing--;
    }
}

void player_is_playing(mt_cpu *cpu) {
    const PlayerState *s = player(cpu);
    mt_ret_u32(cpu, s != nullptr && s->playing ? 1u : 0u);
}

void player_set_volume(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const float v = mt_arg_f32(&a);
    if (PlayerState *s = player(cpu)) {
        s->volume = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    }
}

void player_set_loops(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const std::int32_t n = mt_arg_i32(&a);
    if (PlayerState *s = player(cpu)) {
        s->loops = n;
    }
}

void player_set_current_time(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const double t = mt_arg_f64(&a);
    if (PlayerState *s = player(cpu)) {
        s->time = t > 0 ? t : 0;
    }
}

const mt_objc_host_method k_session_methods[] = {
    {"setCategory:error:", session_set_category},
    {"retain", mt_ns::singleton_retain},
    {"release", mt_ns::singleton_release},
    {"retainCount", mt_ns::singleton_retain_count},
};
const mt_objc_host_method k_session_class_methods[] = {
    {"sharedInstance", session_shared},
};
const mt_objc_host_method k_player_methods[] = {
    {"initWithContentsOfURL:error:", player_init},
    {"prepareToPlay", player_prepare},
    {"play", player_play},
    {"stop", player_stop},
    {"isPlaying", player_is_playing},
    {"setVolume:", player_set_volume},
    {"setNumberOfLoops:", player_set_loops},
    {"setCurrentTime:", player_set_current_time},
};

template <typename T, std::size_t N> constexpr u32 countof(const T (&)[N]) {
    return static_cast<u32>(N);
}

} // namespace

// SDK instance sizes: isa and one private pointer each.
const mt_objc_host_class k_avaudiosession = {"AVAudioSession",
                                             "NSObject",
                                             8,
                                             k_session_methods,
                                             countof(k_session_methods),
                                             k_session_class_methods,
                                             countof(k_session_class_methods)};
const mt_objc_host_class k_avaudioplayer = {
    "AVAudioPlayer", "NSObject", 8, k_player_methods, countof(k_player_methods), nullptr, 0};

} // namespace mt_audio
