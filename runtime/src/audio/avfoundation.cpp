// AVAudioSession (silent, ADR 0038) and AVAudioPlayer (#63, ADR 0046: the MP3s through the
// mixer's music voices), with exactly the selectors the game sends (docs/re/host-surface.md,
// AVFoundation):
//
//   +[AVAudioSession sharedInstance]  -setCategory:error:
//   +[AVAudioPlayer alloc] (NSObject's)  -initWithContentsOfURL:error:  -prepareToPlay  -play
//   -stop  -isPlaying  -setVolume:  -setNumberOfLoops:  -setCurrentTime:
//
// CMSoundPlayer makes one player per music track (LoadBGM: volume 0.7 or 0, loops -1,
// prepareToPlay), and Play stops it if it plays, rewinds it with setCurrentTime:0 unless it
// resumes, sets the volume and plays it; StopAllBGM stops every player that plays
// (docs/re/audio.md). A player is an Mp3Stream (mp3_stream.h) over the file's bytes, played by
// the music voice keyed by the player's address:
//
//   -initWithContentsOfURL:error:  reads the file and opens the stream; nil if it can't be
//                     read or holds no MP3 audio (*outError is left alone: the game passes nil)
//   -play             starts or resumes the voice where the stream is (after the end of its
//                     last pass: from the start, as AVAudioPlayer rewinds when it finishes)
//   -stop             stops the voice, keeping the exact frame (a later -play goes on there)
//   -isPlaying        YES from -play until -stop or the end of the last pass
//   -setVolume:       the voice's volume, clamped to [0, 1]
//   -setNumberOfLoops: n < 0 for ever, else n more passes after the first
//   -setCurrentTime:  seeks to floor(t x rate) of the pass (clamped), dropping what the voice
//                     had buffered
//   dealloc           forgets the voice and the stream
//
// No delegate: the game sets none, so nothing is told when a player finishes.
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "audio/audio_internal.h"
#include "audio/mixer.h"
#include "audio/mp3_stream.h"
#include "cf/cf_objects.h"
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

constexpr std::size_t k_max_file = 64u << 20; // the largest shipped MP3 is under 1 MB

struct PlayerState {
    u32 url; // retained
    bool playing;
    bool finished; // its voice played out the last pass by itself (not stopped)
    float volume;
};

// The streams of this process's players, by player (a new process forgets them; the mixer
// forgets their voices then too).
struct Players {
    mt_objc *rt = nullptr;
    u32 binding = 0;
    std::unordered_map<u32, std::shared_ptr<Mp3Stream>> streams;
};
Players g_players;

std::unordered_map<u32, std::shared_ptr<Mp3Stream>> &streams() {
    if (g_players.rt != mt_objc_current() || g_players.binding != mt_cf::binding()) {
        g_players.streams.clear();
        g_players.rt = mt_objc_current();
        g_players.binding = mt_cf::binding();
    }
    return g_players.streams;
}

Mp3Stream *stream_of(u32 player) {
    auto &m = streams();
    const auto it = m.find(player);
    return it != m.end() ? it->second.get() : nullptr;
}

// Runs `fn` on the stream of `player`: under the mixer's lock while a voice has it (after
// dropping what the voice had buffered), else directly (no render thread can reach it).
void edit(u32 player, Mp3Stream &stream, const std::function<void(Mp3Stream &)> &fn) {
    if (!mix::music_edit(player, [&](mix::MusicStream &) { fn(stream); })) {
        fn(stream);
    }
}

void player_destroy(mt_cpu *cpu, u32 self, void *block) {
    auto *s = static_cast<PlayerState *>(block);
    if (s->playing) {
        stats().players_playing--;
    }
    mix::music_remove(self);
    streams().erase(self);
    mt_objc_release(cpu, s->url);
}
const mt_objc_state_type k_player_state = {"AVAudioPlayer", sizeof(PlayerState), player_destroy};

bool read_file(const std::string &path, std::vector<std::uint8_t> *out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size <= 0 || static_cast<std::uint64_t>(size) > k_max_file) {
        return false;
    }
    in.seekg(0);
    out->resize(static_cast<std::size_t>(size));
    return static_cast<bool>(in.read(reinterpret_cast<char *>(out->data()), size));
}

// The state of the player the message is sent to, with its play state brought up to date: a
// player whose voice has played out its last pass no longer plays.
PlayerState *player(mt_cpu *cpu) {
    stats().calls++;
    const u32 self = mt_objc_self(cpu);
    auto *s =
        static_cast<PlayerState *>(mt_objc_find_state(mt_objc_current(), self, &k_player_state));
    if (s != nullptr && s->playing && !mix::music_playing(self)) {
        s->playing = false;
        s->finished = true;
        stats().players_playing--;
        mt_tracef("audio", "AVAudioPlayer 0x%08X finished playing", static_cast<unsigned>(self));
    }
    return s;
}

// -[AVAudioPlayer initWithContentsOfURL:(NSURL *)url error:(NSError **)outError]: a player
// for a file that holds MP3 audio (nil otherwise, as AVFoundation fails to open it).
void player_init(mt_cpu *cpu) {
    stats().calls++;
    const u32 self = mt_objc_self(cpu);
    mt_args a = mt_objc_args(cpu);
    const u32 url = mt_arg_u32(&a);
    const std::string path = url_host_path(cpu, url);
    std::vector<std::uint8_t> bytes;
    std::shared_ptr<Mp3Stream> stream;
    if (!path.empty() && read_file(path, &bytes)) {
        stream = Mp3Stream::open(std::move(bytes));
    }
    auto *s =
        stream ? static_cast<PlayerState *>(mt_objc_state(mt_objc_current(), self, &k_player_state))
               : nullptr;
    if (s == nullptr) {
        mt_tracef("audio", "-[AVAudioPlayer initWithContentsOfURL:%s error:]: %s: nil",
                  path.c_str(), path.empty() || bytes.empty() ? "no such file" : "not an MP3");
        mt_objc_release(cpu, self);
        mt_ret_u32(cpu, 0);
        return;
    }
    mix::music_remove(self); // a voice left by an object that lived here before (none in
                             // practice: this object's own dealloc removed it)
    s->url = mt_objc_retain(cpu, url);
    s->volume = 1.0f;
    streams()[self] = stream;
    stats().players++;
    mt_tracef("audio",
              "-[AVAudioPlayer initWithContentsOfURL:%s error:] -> 0x%08X: %u Hz, %u channels, "
              "%llu frames",
              path.c_str(), static_cast<unsigned>(self), static_cast<unsigned>(stream->rate()),
              static_cast<unsigned>(stream->channels()),
              static_cast<unsigned long long>(stream->length()));
    mt_ret_u32(cpu, self);
}

void player_prepare(mt_cpu *cpu) {
    player(cpu);
    mt_ret_u32(cpu, 1);
}

void player_play(mt_cpu *cpu) {
    PlayerState *s = player(cpu);
    const u32 self = mt_objc_self(cpu);
    Mp3Stream *stream = s != nullptr ? stream_of(self) : nullptr;
    if (stream == nullptr) {
        mt_ret_u32(cpu, 0);
        return;
    }
    if (s->finished) { // AVAudioPlayer is back at the start once it has finished
        edit(self, *stream, [](Mp3Stream &m) { m.restart(); });
        s->finished = false;
    }
    // The position before the voice starts (the render thread reads the stream after that).
    const unsigned long long at = s->playing ? 0ull : stream->position();
    mix::music_play(self, streams()[self], s->volume);
    if (!s->playing) {
        s->playing = true;
        stats().players_playing++;
        mt_tracef("audio", "-[AVAudioPlayer play] 0x%08X at frame %llu",
                  static_cast<unsigned>(self), at);
    } else {
        mt_tracef("audio", "-[AVAudioPlayer play] 0x%08X (playing)", static_cast<unsigned>(self));
    }
    mt_ret_u32(cpu, 1);
}

// -stop keeps the position (a later -play resumes there), as AVAudioPlayer does.
void player_stop(mt_cpu *cpu) {
    PlayerState *s = player(cpu);
    const u32 self = mt_objc_self(cpu);
    if (s == nullptr) {
        return;
    }
    mix::music_stop(self);
    if (s->playing) {
        s->playing = false;
        stats().players_playing--;
    }
    const Mp3Stream *stream = stream_of(self);
    mt_tracef("audio", "-[AVAudioPlayer stop] 0x%08X at frame %llu", static_cast<unsigned>(self),
              static_cast<unsigned long long>(stream != nullptr ? stream->position() : 0));
}

void player_is_playing(mt_cpu *cpu) {
    const PlayerState *s = player(cpu);
    mt_ret_u32(cpu, s != nullptr && s->playing ? 1u : 0u);
}

void player_set_volume(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const float v = mt_arg_f32(&a);
    if (PlayerState *s = player(cpu)) {
        s->volume = v > 0.0f ? (v < 1.0f ? v : 1.0f) : 0.0f; // NaN: 0
        mix::music_set_volume(mt_objc_self(cpu), s->volume);
        mt_tracef("audio", "-[AVAudioPlayer setVolume:%g] 0x%08X", static_cast<double>(v),
                  static_cast<unsigned>(mt_objc_self(cpu)));
    }
}

void player_set_loops(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const std::int32_t n = mt_arg_i32(&a);
    if (player(cpu) != nullptr) {
        if (Mp3Stream *stream = stream_of(mt_objc_self(cpu))) {
            stream->set_loops(n);
        }
        mt_tracef("audio", "-[AVAudioPlayer setNumberOfLoops:%d] 0x%08X", static_cast<int>(n),
                  static_cast<unsigned>(mt_objc_self(cpu)));
    }
}

// -setCurrentTime:(NSTimeInterval)t, seconds into the pass.
void player_set_current_time(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const double t = mt_arg_f64(&a);
    const u32 self = mt_objc_self(cpu);
    PlayerState *s = player(cpu);
    Mp3Stream *stream = s != nullptr ? stream_of(self) : nullptr;
    if (stream == nullptr) {
        return;
    }
    s->finished = false;                                          // the next -play starts here
    const double at = t > 0 ? std::floor(t * stream->rate()) : 0; // NaN: 0
    const std::uint64_t frame = at < static_cast<double>(stream->length())
                                    ? static_cast<std::uint64_t>(at)
                                    : stream->length();
    edit(self, *stream, [frame](Mp3Stream &m) { m.seek(frame); });
    mt_tracef("audio", "-[AVAudioPlayer setCurrentTime:%g] 0x%08X: frame %llu", t,
              static_cast<unsigned>(self), static_cast<unsigned long long>(frame));
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
