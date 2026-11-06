// OpenAL (#61, ADR 0045), the audio session and system sounds (#117, ADR 0038), ExtAudioFile
// (#62, ADR 0045), maplethief/audio.h. The game's use (docs/re/audio.md): CMSoundPlayer opens the
// default device, makes a context current, generates 44 buffers and 44 sources, decodes the 26 WAV
// effects through ExtAudioFile (Apple's MyGetOpenALAudioData) into the buffers, attaches each
// buffer to its source, and later plays an effect with alSourcePlay. Any AL error at start-up
// ends the app with exit(1), so the error state is kept as OpenAL keeps it.
//
// OpenAL plays through the mixer (mixer.h): alBufferData copies the guest's PCM into a host
// Pcm, alSourcePlay (re)starts the source's effect voice, alListenerf(AL_GAIN) is the
// listener gain, and the effects pause while the context is suspended or not current. No mixer
// call runs guest code or reads guest memory under the mixer's lock.
//
// ExtAudioFile decodes the WAVs (#62): ExtAudioFileOpenURL walks the RIFF chunks (bounds-checked:
// an untrusted file) and keeps a host copy of the data chunk; ExtAudioFileRead copies the next
// frames into the guest's buffer in the client format. Supported, as MyGetOpenALAudioData
// needs and no more: integer PCM files of 8 (unsigned), 16, 24 or 32 bits, read either in the
// file's own format or as 16-bit signed packed native-endian interleaved PCM at the file's
// channel count and rate. Any other client format (another rate or channel count, float,
// big-endian, non-interleaved stereo, other depths) is refused with 'fmt?'. Still to come: the
// audio session (#64).
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "audio/audio_internal.h"
#include "audio/audio_pack.h"
#include "audio/mixer.h"
#include "audio/wav.h"
#include "cf/cf_objects.h"
#include "foundation/foundation.h"
#include "maplethief/diag.h"
#include "maplethief/hle/abi.h"
#include "maplethief/memory.h"
#include "snapshot/snapshot_internal.h"

namespace mt_audio {
namespace {

// OpenAL
enum : u32 {
    k_al_no_error = 0,
    k_al_invalid_name = 0xA001,
    k_al_invalid_enum = 0xA002,
    k_al_invalid_value = 0xA003,
    k_al_invalid_operation = 0xA004,
    k_al_out_of_memory = 0xA005,
    k_al_buffer = 0x1009,
    k_al_gain = 0x100A,
    k_al_format_mono8 = 0x1100,
    k_al_format_mono16 = 0x1101,
    k_al_format_stereo8 = 0x1102,
    k_al_format_stereo16 = 0x1103,
};
constexpr u32 k_max_names = 1024;
// The largest alBufferData we copy: AL_OUT_OF_MEMORY beyond (the game's largest is 20.wav's,
// about 271 KB).
constexpr u32 k_max_buffer_bytes = 64u << 20;

// AudioToolbox OSStatus values and property ids
constexpr std::int32_t k_no_err = 0;
constexpr std::int32_t k_param_err = -50;
constexpr std::int32_t k_fnf_err = -43;                            // file not found
constexpr std::int32_t k_unsupported_file = 0x7479703F;            // 'typ?'
constexpr std::int32_t k_session_not_initialized = 0x21696E69;     // '!ini'
constexpr std::int32_t k_session_already_initialized = 0x696E6974; // 'init'
constexpr std::int32_t k_ext_invalid_property = -66561;
constexpr std::int32_t k_ext_invalid_property_size = -66562;
constexpr std::int32_t k_ext_non_pcm_client_format = -66563;
constexpr std::int32_t k_converter_format_not_supported = 0x666D743F; // 'fmt?'
constexpr u32 k_prop_file_format = 0x66666D74;                        // 'ffmt'
constexpr u32 k_prop_client_format = 0x63666D74;                      // 'cfmt'
constexpr u32 k_prop_length_frames = 0x2366726D;                      // '#frm'
constexpr u32 k_format_lpcm = 0x6C70636D;                             // 'lpcm'
// AudioFormatFlags of linear PCM
enum : u32 {
    k_flag_float = 0x1,
    k_flag_big_endian = 0x2,
    k_flag_signed_integer = 0x4,
    k_flag_packed = 0x8,
    k_flag_non_interleaved = 0x20,
};

// AudioStreamBasicDescription (40 bytes)
struct Asbd {
    double rate = 0;
    u32 format_id = 0, flags = 0, bytes_per_packet = 0, frames_per_packet = 0, bytes_per_frame = 0,
        channels = 0, bits = 0, reserved = 0;
};
constexpr u32 k_asbd_size = 40;

struct File {
    Asbd file, client;
    bool has_client = false;
    // The client format is 16-bit signed PCM converted from a file of another depth (false:
    // the file's own bytes are copied).
    bool to_s16 = false;
    std::uint64_t frames = 0, position = 0;
    std::vector<std::uint8_t> data; // the data chunk: frames * file.bytes_per_frame bytes
};

struct State {
    mt_objc *rt = nullptr;
    u32 binding = 0; // CF's binding to rt (a new runtime at the same address starts afresh)
    mt_audio_stats st{};
    // OpenAL
    std::unordered_map<u32, u32> devices;  // handle -> open
    std::unordered_map<u32, u32> contexts; // handle -> device
    std::unordered_set<u32> suspended;     // contexts alcSuspendContext stopped
    u32 current_context = 0;
    u32 al_error = k_al_no_error;
    struct Buffer {
        u32 format = 0, size = 0, rate = 0;
        bool filled = false;
        mix::PcmPtr pcm; // the samples (host memory), shared with the voices playing them
    };
    std::unordered_map<u32, Buffer> buffers;
    struct Source {
        u32 buffer = 0;
        u32 plays = 0;
    };
    std::unordered_map<u32, Source> sources;
    u32 next_buffer = 1, next_source = 1;
    float listener_gain = 1.0f;
    // AudioSession
    bool session_initialized = false;
    u32 session_listener = 0, session_category = 0;
    // ExtAudioFile
    std::unordered_map<u32, File> files;
};
State g;

State &state() {
    if (g.rt != mt_objc_current() || g.binding != mt_cf::binding()) {
        g = State{};
        g.rt = mt_objc_current();
        g.binding = mt_cf::binding();
        mix::reset(); // the previous process's voices stop
    }
    return g;
}

// The effects are mixed while a context is current and not suspended (ADR 0045: the game's
// interruption handler makes no context current and suspends it, then makes it current and
// processes it again).
void update_pause(State &s) {
    mix::set_effects_paused(s.current_context == 0 || s.suspended.count(s.current_context) != 0);
}

u32 rd32(mt_cpu *cpu, u32 addr) {
    u32 v;
    std::memcpy(&v, mt_g2h(cpu, addr, 4), 4);
    return v;
}
void wr32(mt_cpu *cpu, u32 addr, u32 v) { std::memcpy(mt_g2h(cpu, addr, 4), &v, 4); }

// A handle the guest can hold: 16 bytes of the guest heap (never read by the game).
u32 new_handle(mt_cpu *cpu) {
    const u32 h = mt_heap_malloc(mt_mem_from_cpu(cpu), 16);
    if (h != 0) {
        std::memset(cpu->mem + h, 0, 16);
    }
    return h;
}
void free_handle(mt_cpu *cpu, u32 h) {
    if (h != 0) {
        mt_heap_free(mt_mem_from_cpu(cpu), h);
    }
}

State &begin(const char *what) {
    State &s = state();
    s.st.calls++;
    (void)what;
    return s;
}

// An AL call that needs a current context; sets AL_INVALID_OPERATION without one.
bool need_context(State &s, const char *fn) {
    if (s.current_context != 0) {
        return true;
    }
    s.al_error = k_al_invalid_operation;
    mt_tracef("audio", "%s: no current context (AL_INVALID_OPERATION)", fn);
    return false;
}
void set_error(State &s, u32 error, const char *fn) {
    if (s.al_error == k_al_no_error) { // OpenAL keeps the first error until alGetError
        s.al_error = error;
    }
    mt_tracef("audio", "%s: error 0x%X", fn, static_cast<unsigned>(error));
}

void write_asbd(mt_cpu *cpu, u32 addr, const Asbd &a) {
    std::uint8_t *p = mt_g2h(cpu, addr, k_asbd_size);
    std::memcpy(p, &a.rate, 8);
    const u32 words[8] = {
        a.format_id, a.flags, a.bytes_per_packet, a.frames_per_packet, a.bytes_per_frame,
        a.channels,  a.bits,  a.reserved};
    std::memcpy(p + 8, words, sizeof words);
}
Asbd read_asbd(mt_cpu *cpu, u32 addr) {
    const std::uint8_t *p = mt_g2h(cpu, addr, k_asbd_size);
    Asbd a;
    std::memcpy(&a.rate, p, 8);
    u32 words[8];
    std::memcpy(words, p + 8, sizeof words);
    a.format_id = words[0];
    a.flags = words[1];
    a.bytes_per_packet = words[2];
    a.frames_per_packet = words[3];
    a.bytes_per_frame = words[4];
    a.channels = words[5];
    a.bits = words[6];
    a.reserved = words[7];
    return a;
}

// A PCM WAV file's format and length (wav.h: an untrusted file, every read bounds-checked).
// False if it is no PCM WAV.
bool parse_wav(const std::vector<std::uint8_t> &b, File *out) {
    wav::Info w;
    if (!wav::parse(b, &w)) {
        return false;
    }
    Asbd &f = out->file;
    f.rate = w.rate;
    f.format_id = k_format_lpcm;
    // kAudioFormatFlagIsSignedInteger (not for 8-bit WAV, which is unsigned) | IsPacked
    f.flags = (w.bits > 8 ? 0x4u : 0u) | 0x8u;
    f.bytes_per_packet = w.block_align;
    f.frames_per_packet = 1;
    f.bytes_per_frame = w.block_align;
    f.channels = w.channels;
    f.bits = w.bits;
    out->frames = w.frames;
    out->data.assign(b.begin() + static_cast<std::ptrdiff_t>(w.data_at),
                     b.begin() + static_cast<std::ptrdiff_t>(w.data_at + w.frames * w.block_align));
    return true;
}

// An audio pack's decoded effect (#188) as a 16-bit PCM file, which the game reads in its own
// format.
void pcm_file(const pack::Sound &sound, File *out) {
    Asbd &f = out->file;
    f.rate = sound.rate;
    f.format_id = k_format_lpcm;
    f.flags = k_flag_signed_integer | k_flag_packed;
    f.bytes_per_packet = f.bytes_per_frame = 2 * sound.channels;
    f.frames_per_packet = 1;
    f.channels = sound.channels;
    f.bits = 16;
    out->frames = sound.samples.size() / sound.channels;
    out->data.resize(static_cast<std::size_t>(out->frames) * f.bytes_per_frame);
    std::memcpy(out->data.data(), sound.samples.data(), out->data.size()); // little-endian
}

// How a client format reads file `f`: 1 = the file's own bytes, 2 = 16-bit signed integer
// converted from the file's depth, 0 = a conversion we don't make. Linear PCM, packed,
// little-endian (the guest's native order), interleaved (one buffer), at the file's channel
// count and rate.
int client_mode(const Asbd &f, const Asbd &c) {
    const u32 non_interleaved = c.channels == 1 ? k_flag_non_interleaved : 0u; // the same thing
    if (c.rate != f.rate || c.channels != f.channels || c.frames_per_packet != 1 ||
        c.bytes_per_packet != c.bytes_per_frame ||
        (c.flags &
         (k_flag_float | k_flag_big_endian | (k_flag_non_interleaved & ~non_interleaved))) != 0) {
        return 0;
    }
    if (c.bits == f.bits && c.bytes_per_frame == f.bytes_per_frame &&
        (c.flags & k_flag_signed_integer) == (f.flags & k_flag_signed_integer)) {
        return 1;
    }
    if (c.bits == 16 && c.bytes_per_frame == 2 * c.channels &&
        (c.flags & k_flag_signed_integer) != 0) {
        return 2;
    }
    return 0;
}

// `frames` frames of `f` from frame `first` as 16-bit signed little-endian samples at `out`.
void convert_to_s16(const File &f, std::uint64_t first, std::uint64_t frames, std::uint8_t *out) {
    const u32 bytes = f.file.bits / 8;
    const std::uint64_t samples = frames * f.file.channels;
    const std::uint8_t *in = f.data.data() + first * f.file.bytes_per_frame;
    for (std::uint64_t i = 0; i < samples; i++, in += bytes, out += 2) {
        // the top 16 bits of the sample (8-bit WAV is unsigned, offset 128)
        std::uint8_t lo, hi;
        if (bytes == 1) {
            lo = 0;
            hi = static_cast<std::uint8_t>(in[0] ^ 0x80u);
        } else {
            lo = in[bytes - 2];
            hi = in[bytes - 1];
        }
        out[0] = lo;
        out[1] = hi;
    }
}

bool read_file(const std::string &path, std::vector<std::uint8_t> *out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0 || size > (64 << 20)) {
        return false;
    }
    in.seekg(0);
    out->resize(static_cast<std::size_t>(size));
    return size == 0 || static_cast<bool>(in.read(reinterpret_cast<char *>(out->data()), size));
}

File *file_of(State &s, u32 handle) {
    const auto f = s.files.find(handle);
    return handle != 0 && f != s.files.end() ? &f->second : nullptr;
}

// glGen-style name generation for OpenAL.
template <typename Map> void gen_names(mt_cpu *cpu, State &s, Map &map, u32 &next, const char *fn) {
    mt_args a = mt_args_begin(cpu);
    const auto n = mt_arg_i32(&a);
    const u32 out = mt_arg_u32(&a);
    if (!need_context(s, fn)) {
        return;
    }
    if (n < 0 || static_cast<u32>(n) > k_max_names) {
        set_error(s, k_al_invalid_value, fn);
        return;
    }
    for (u32 i = 0; i < static_cast<u32>(n); i++) {
        while (next == 0 || map.count(next) != 0) {
            next++;
        }
        map[next];
        wr32(cpu, out + 4u * i, next);
        next++;
    }
    mt_tracef("audio", "%s(%d) -> names %u..%u", fn, static_cast<int>(n),
              static_cast<unsigned>(next - static_cast<u32>(n)), static_cast<unsigned>(next - 1));
}

// Deletes the names the guest passes. All must be valid (AL_INVALID_NAME) and none in use
// (`in_use(name)`: AL_INVALID_OPERATION), or nothing is deleted; `deleted(name)` runs for each
// name deleted.
template <typename Map, typename InUse, typename Deleted>
void delete_names(mt_cpu *cpu, State &s, Map &map, const char *fn, InUse in_use, Deleted deleted) {
    mt_args a = mt_args_begin(cpu);
    const auto n = mt_arg_i32(&a);
    const u32 names = mt_arg_u32(&a);
    if (!need_context(s, fn)) {
        return;
    }
    if (n < 0 || static_cast<u32>(n) > k_max_names) {
        set_error(s, k_al_invalid_value, fn);
        return;
    }
    for (u32 i = 0; i < static_cast<u32>(n); i++) {
        const u32 name = rd32(cpu, names + 4u * i);
        if (name != 0 && map.count(name) == 0) {
            set_error(s, k_al_invalid_name, fn);
            return;
        }
    }
    for (u32 i = 0; i < static_cast<u32>(n); i++) {
        const u32 name = rd32(cpu, names + 4u * i);
        if (name != 0 && in_use(name)) {
            set_error(s, k_al_invalid_operation, fn);
            return;
        }
    }
    for (u32 i = 0; i < static_cast<u32>(n); i++) {
        const u32 name = rd32(cpu, names + 4u * i);
        if (name != 0 && map.erase(name) != 0) {
            deleted(name);
        }
    }
}

} // namespace

mt_audio_stats &stats() { return state().st; }

mix::PcmPtr buffer_pcm(u32 name) {
    State &s = state();
    const auto b = s.buffers.find(name);
    return b != s.buffers.end() && b->second.filled ? b->second.pcm : nullptr;
}

std::string url_host_path(mt_cpu *cpu, u32 url) {
    std::string text;
    bool is_file = false;
    if (url == 0 || !mt_ns::url_string(cpu, url, &text, &is_file) || !is_file) {
        return "";
    }
    return mt_ns::host_path(text, false);
}

// --- save states (#203, ADR 0105) ---------------------------------------------------------------
//
// The section "audio": OpenAL (devices, contexts, buffers with their PCM, sources, the error
// flag, the listener gain, the audio session), this process's counters, the players' streams
// (avfoundation.cpp) and the mixer's voices: each effect voice's Pcm by the buffer it came from
// (or whole), its 32.32 position; each music voice by its player, with the frames it pulled
// and has not played yet. An open ExtAudioFile is no safe point (the game reads its WAVs at
// start-up, inside one call). The output device, the bus gains, mute and mono are the player's
// and stay.

namespace {

void put_pcm(mt_snap::Writer &w, const mix::Pcm &p) {
    w.u32(p.channels);
    w.u32(p.rate);
    w.u32(p.frames);
    w.u32(static_cast<u32>(p.samples.size()));
    for (const std::int16_t v : p.samples) {
        const auto u = static_cast<std::uint16_t>(v);
        w.u8v(static_cast<std::uint8_t>(u));
        w.u8v(static_cast<std::uint8_t>(u >> 8));
    }
}

bool get_pcm(mt_snap::Reader &r, mix::PcmPtr *out) {
    auto p = std::make_shared<mix::Pcm>();
    u32 n = 0;
    if (!r.u32(&p->channels) || !r.u32(&p->rate) || !r.u32(&p->frames) || !r.u32(&n) ||
        !r.count_fits(n, 2)) {
        return false;
    }
    if (p->channels < 1 || p->channels > 2 || p->rate == 0 ||
        static_cast<std::uint64_t>(p->frames) * p->channels != n) {
        return r.fail("PCM of a bad shape");
    }
    const std::uint8_t *b = r.raw(std::size_t{n} * 2u);
    p->samples.resize(n);
    for (u32 i = 0; i < n; i++) {
        p->samples[i] = static_cast<std::int16_t>(static_cast<std::uint16_t>(b[2 * i]) |
                                                  static_cast<std::uint16_t>(b[2 * i + 1]) << 8);
    }
    *out = std::move(p);
    return true;
}

template <typename Map> std::vector<u32> keys_of(const Map &m) {
    std::vector<u32> keys;
    for (const auto &e : m) {
        keys.push_back(e.first);
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

void put_stats(mt_snap::Writer &w, const mt_audio_stats &st) {
    for (const u32 v :
         {st.calls, st.buffers, st.buffers_filled, st.sources, st.source_plays, st.files_opened,
          st.files_failed, st.players, st.players_playing, st.system_sounds, st.session_active}) {
        w.u32(v);
    }
}
bool get_stats(mt_snap::Reader &r, mt_audio_stats *st) {
    for (u32 *v : {&st->calls, &st->buffers, &st->buffers_filled, &st->sources, &st->source_plays,
                   &st->files_opened, &st->files_failed, &st->players, &st->players_playing,
                   &st->system_sounds, &st->session_active}) {
        if (!r.u32(v)) {
            return false;
        }
    }
    return true;
}

bool save_audio(mt_cpu *, mt_snap::Writer &w, std::string *why) {
    State &s = state();
    if (!s.files.empty()) {
        *why = "a sound file is open";
        return false;
    }
    put_stats(w, s.st);
    w.u32(static_cast<u32>(s.devices.size()));
    for (const u32 k : keys_of(s.devices)) {
        w.u32(k);
        w.u32(s.devices.at(k));
    }
    w.u32(static_cast<u32>(s.contexts.size()));
    for (const u32 k : keys_of(s.contexts)) {
        w.u32(k);
        w.u32(s.contexts.at(k));
    }
    std::vector<u32> suspended(s.suspended.begin(), s.suspended.end());
    std::sort(suspended.begin(), suspended.end());
    w.words(suspended);
    w.u32(s.current_context);
    w.u32(s.al_error);
    w.u32(static_cast<u32>(s.buffers.size()));
    for (const u32 k : keys_of(s.buffers)) {
        const State::Buffer &b = s.buffers.at(k);
        w.u32(k);
        w.u32(b.format);
        w.u32(b.size);
        w.u32(b.rate);
        w.boolean(b.filled);
        w.boolean(b.pcm != nullptr);
        if (b.pcm != nullptr) {
            put_pcm(w, *b.pcm);
        }
    }
    w.u32(static_cast<u32>(s.sources.size()));
    for (const u32 k : keys_of(s.sources)) {
        w.u32(k);
        w.u32(s.sources.at(k).buffer);
        w.u32(s.sources.at(k).plays);
    }
    w.u32(s.next_buffer);
    w.u32(s.next_source);
    w.f32(s.listener_gain);
    w.boolean(s.session_initialized);
    w.u32(s.session_listener);
    w.u32(s.session_category);
    snapshot::players_save(w);
    // The voices, under the mixer's lock (copied).
    const mix::Voices v = mix::voices_copy();
    w.u32(static_cast<u32>(v.effects.size()));
    for (const mix::Voices::Effect &e : v.effects) {
        w.u32(e.key);
        u32 from = 0; // the buffer whose Pcm it plays
        for (const u32 k : keys_of(s.buffers)) {
            if (e.pcm != nullptr && s.buffers.at(k).pcm == e.pcm) {
                from = k;
                break;
            }
        }
        w.u32(from);
        w.boolean(from == 0 && e.pcm != nullptr);
        if (from == 0 && e.pcm != nullptr) {
            put_pcm(w, *e.pcm);
        }
        w.u64(e.pos);
        w.u64(e.step);
        w.boolean(e.playing);
    }
    w.u32(static_cast<u32>(v.music.size()));
    for (const mix::Voices::Music &m : v.music) {
        w.u32(m.key);
        w.boolean(m.stream != nullptr);
        w.f32(m.volume);
        w.boolean(m.playing);
        w.u32(m.frames);
        w.u32(m.channels);
        w.u32(static_cast<u32>(m.buf.size()));
        for (const std::int16_t x : m.buf) {
            const auto u = static_cast<std::uint16_t>(x);
            w.u8v(static_cast<std::uint8_t>(u));
            w.u8v(static_cast<std::uint8_t>(u >> 8));
        }
        w.u64(m.pos);
        w.u64(m.step);
        w.boolean(m.ended);
    }
    w.f32(v.listener_gain);
    w.boolean(v.effects_paused);
    return true;
}

bool load_audio(mt_cpu *, mt_snap::Reader &r, std::function<void(mt_cpu *)> *apply) {
    auto s = std::make_shared<State>();
    u32 n = 0;
    if (!get_stats(r, &s->st) || !r.u32(&n) || !r.count_fits(n, 8)) {
        return false;
    }
    for (u32 i = 0; i < n; i++) {
        u32 k = 0, v = 0;
        r.u32(&k);
        r.u32(&v);
        s->devices[k] = v;
    }
    if (!r.u32(&n) || !r.count_fits(n, 8)) {
        return false;
    }
    for (u32 i = 0; i < n; i++) {
        u32 k = 0, v = 0;
        r.u32(&k);
        r.u32(&v);
        s->contexts[k] = v;
    }
    std::vector<u32> suspended;
    if (!r.words(&suspended) || !r.u32(&s->current_context) || !r.u32(&s->al_error) || !r.u32(&n) ||
        !r.count_fits(n, 18)) {
        return false;
    }
    s->suspended.insert(suspended.begin(), suspended.end());
    for (u32 i = 0; i < n; i++) {
        u32 k = 0;
        State::Buffer b;
        bool has_pcm = false;
        if (!r.u32(&k) || !r.u32(&b.format) || !r.u32(&b.size) || !r.u32(&b.rate) ||
            !r.boolean(&b.filled) || !r.boolean(&has_pcm) || (has_pcm && !get_pcm(r, &b.pcm))) {
            return false;
        }
        s->buffers[k] = std::move(b);
    }
    if (!r.u32(&n) || !r.count_fits(n, 12)) {
        return false;
    }
    for (u32 i = 0; i < n; i++) {
        u32 k = 0;
        State::Source src;
        r.u32(&k);
        r.u32(&src.buffer);
        r.u32(&src.plays);
        s->sources[k] = src;
    }
    if (!r.u32(&s->next_buffer) || !r.u32(&s->next_source) || !r.f32(&s->listener_gain) ||
        !r.boolean(&s->session_initialized) || !r.u32(&s->session_listener) ||
        !r.u32(&s->session_category)) {
        return false;
    }
    std::map<u32, std::shared_ptr<mix::MusicStream>> streams;
    std::function<void()> players_apply;
    if (!snapshot::players_load(r, &streams, &players_apply)) {
        return false;
    }
    auto voices = std::make_shared<mix::Voices>();
    if (!r.u32(&n) || !r.count_fits(n, 30)) {
        return false;
    }
    for (u32 i = 0; i < n; i++) {
        mix::Voices::Effect e;
        u32 from = 0;
        bool inline_pcm = false;
        if (!r.u32(&e.key) || !r.u32(&from) || !r.boolean(&inline_pcm) ||
            (inline_pcm && !get_pcm(r, &e.pcm)) || !r.u64(&e.pos) || !r.u64(&e.step) ||
            !r.boolean(&e.playing)) {
            return false;
        }
        if (from != 0) {
            const auto b = s->buffers.find(from);
            if (b == s->buffers.end() || b->second.pcm == nullptr) {
                return r.fail("an effect voice of no buffer");
            }
            e.pcm = b->second.pcm;
        }
        voices->effects.push_back(std::move(e));
    }
    if (!r.u32(&n) || !r.count_fits(n, 43)) {
        return false;
    }
    for (u32 i = 0; i < n; i++) {
        mix::Voices::Music m;
        bool has_stream = false;
        u32 samples = 0;
        if (!r.u32(&m.key) || !r.boolean(&has_stream) || !r.f32(&m.volume) ||
            !r.boolean(&m.playing) || !r.u32(&m.frames) || !r.u32(&m.channels) ||
            !r.u32(&samples) || !r.count_fits(samples, 2)) {
            return false;
        }
        if (static_cast<std::uint64_t>(m.frames) * m.channels != samples || m.channels > 2) {
            return r.fail("a music voice of a bad shape");
        }
        const std::uint8_t *b = r.raw(std::size_t{samples} * 2u);
        m.buf.resize(samples);
        for (u32 j = 0; j < samples; j++) {
            m.buf[j] = static_cast<std::int16_t>(static_cast<std::uint16_t>(b[2 * j]) |
                                                 static_cast<std::uint16_t>(b[2 * j + 1]) << 8);
        }
        if (!r.u64(&m.pos) || !r.u64(&m.step) || !r.boolean(&m.ended)) {
            return false;
        }
        if (has_stream) {
            const auto it = streams.find(m.key);
            if (it == streams.end()) {
                return r.fail("a music voice of no player");
            }
            m.stream = it->second;
        }
        voices->music.push_back(std::move(m));
    }
    if (!r.f32(&voices->listener_gain) || !r.boolean(&voices->effects_paused)) {
        return false;
    }
    *apply = [s, voices, players_apply](mt_cpu *) {
        State &st = state();
        s->rt = st.rt;
        s->binding = st.binding;
        st = std::move(*s);
        players_apply();
        mix::voices_replace(std::move(*voices));
    };
    return true;
}

} // namespace

extern const mt_snap::Module k_snap_audio;
const mt_snap::Module k_snap_audio = {"audio", save_audio, load_audio};

} // namespace mt_audio

using namespace mt_audio;

extern "C" {

// --- OpenAL: devices and contexts ---------------------------------------------------------------

// ALCdevice *alcOpenDevice(const ALCchar *devicename): the default device (any name).
void mt_hle_alcOpenDevice(mt_cpu *cpu, std::uint32_t) noexcept {
    State &s = begin("alcOpenDevice");
    const u32 h = new_handle(cpu);
    if (h != 0) {
        s.devices[h] = 1;
    }
    mt_tracef("audio", "alcOpenDevice -> 0x%08X", static_cast<unsigned>(h));
    mt_ret_u32(cpu, h);
}

// ALCboolean alcCloseDevice(ALCdevice *device)
void mt_hle_alcCloseDevice(mt_cpu *cpu, std::uint32_t) noexcept {
    State &s = begin("alcCloseDevice");
    const u32 dev = cpu->r[0];
    if (s.devices.erase(dev) == 0) {
        mt_ret_u32(cpu, 0);
        return;
    }
    free_handle(cpu, dev);
    mt_ret_u32(cpu, 1);
}

// ALCcontext *alcCreateContext(ALCdevice *device, const ALCint *attrlist)
void mt_hle_alcCreateContext(mt_cpu *cpu, std::uint32_t) noexcept {
    State &s = begin("alcCreateContext");
    const u32 dev = cpu->r[0];
    if (s.devices.count(dev) == 0) {
        mt_ret_u32(cpu, 0); // ALC_INVALID_DEVICE
        return;
    }
    const u32 h = new_handle(cpu);
    if (h != 0) {
        s.contexts[h] = dev;
    }
    mt_tracef("audio", "alcCreateContext(0x%08X) -> 0x%08X", static_cast<unsigned>(dev),
              static_cast<unsigned>(h));
    mt_ret_u32(cpu, h);
}

// void alcDestroyContext(ALCcontext *context): not the current one. With the last context the
// sources' voices stop (one name space for all contexts: the game makes one).
void mt_hle_alcDestroyContext(mt_cpu *cpu, std::uint32_t) noexcept {
    State &s = begin("alcDestroyContext");
    const u32 ctx = cpu->r[0];
    if (ctx != s.current_context && s.contexts.erase(ctx) != 0) {
        s.suspended.erase(ctx);
        free_handle(cpu, ctx);
        if (s.contexts.empty()) {
            mix::effect_stop_all();
        }
    }
}

// ALCboolean alcMakeContextCurrent(ALCcontext *context): NULL releases the current one.
void mt_hle_alcMakeContextCurrent(mt_cpu *cpu, std::uint32_t) noexcept {
    State &s = begin("alcMakeContextCurrent");
    const u32 ctx = cpu->r[0];
    if (ctx != 0 && s.contexts.count(ctx) == 0) {
        mt_ret_u32(cpu, 0); // ALC_INVALID_CONTEXT
        return;
    }
    s.current_context = ctx;
    update_pause(s);
    mt_tracef("audio", "alcMakeContextCurrent(0x%08X)", static_cast<unsigned>(ctx));
    mt_ret_u32(cpu, 1);
}

// ALCcontext *alcGetCurrentContext(void)
void mt_hle_alcGetCurrentContext(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_ret_u32(cpu, begin("alcGetCurrentContext").current_context);
}

// ALCdevice *alcGetContextsDevice(ALCcontext *context)
void mt_hle_alcGetContextsDevice(mt_cpu *cpu, std::uint32_t) noexcept {
    State &s = begin("alcGetContextsDevice");
    const auto c = s.contexts.find(cpu->r[0]);
    mt_ret_u32(cpu, c != s.contexts.end() ? c->second : 0u);
}

// void alcSuspendContext(ALCcontext *): its effects pause (keeping their positions) while it is
// current; void alcProcessContext(ALCcontext *): they go on. An unknown context is ignored.
void mt_hle_alcProcessContext(mt_cpu *cpu, std::uint32_t) noexcept {
    State &s = begin("alcProcessContext");
    s.suspended.erase(cpu->r[0]);
    update_pause(s);
    mt_tracef("audio", "alcProcessContext(0x%08X)", static_cast<unsigned>(cpu->r[0]));
}
void mt_hle_alcSuspendContext(mt_cpu *cpu, std::uint32_t) noexcept {
    State &s = begin("alcSuspendContext");
    if (s.contexts.count(cpu->r[0]) != 0) {
        s.suspended.insert(cpu->r[0]);
    }
    update_pause(s);
    mt_tracef("audio", "alcSuspendContext(0x%08X)", static_cast<unsigned>(cpu->r[0]));
}

// void *alcGetProcAddress(ALCdevice *device, const ALCchar *funcname): NULL for every name, the
// answer for an extension that is not there (ADR 0045). The game asks only for
// alBufferDataStatic (Apple's alBufferDataStaticProc 0x1D5F5, the name at 0xE3764), which
// checks for NULL and is reached only from SoundPlayer_LoadEffects's id 0 branch, which cannot
// run (0 is a music id): docs/re/audio.md.
void mt_hle_alcGetProcAddress(mt_cpu *cpu, std::uint32_t) noexcept {
    begin("alcGetProcAddress");
    const u32 name = cpu->r[1];
    mt_tracef("audio", "alcGetProcAddress(\"%s\") -> NULL", name != 0 ? mt_g2h_str(cpu, name) : "");
    mt_ret_u32(cpu, 0);
}

// --- OpenAL: buffers, sources, listener ---------------------------------------------------------

// ALenum alGetError(void): the first error since the last call, then AL_NO_ERROR.
void mt_hle_alGetError(mt_cpu *cpu, std::uint32_t) noexcept {
    State &s = begin("alGetError");
    u32 e = s.al_error;
    if (s.current_context == 0) {
        e = k_al_invalid_operation;
    }
    s.al_error = k_al_no_error;
    mt_ret_u32(cpu, e);
}

void mt_hle_alGenBuffers(mt_cpu *cpu, std::uint32_t) noexcept {
    State &s = begin("alGenBuffers");
    gen_names(cpu, s, s.buffers, s.next_buffer, "alGenBuffers");
}
// A buffer attached to a source is in use: AL_INVALID_OPERATION.
void mt_hle_alDeleteBuffers(mt_cpu *cpu, std::uint32_t) noexcept {
    State &s = begin("alDeleteBuffers");
    delete_names(
        cpu, s, s.buffers, "alDeleteBuffers",
        [&s](u32 name) {
            return std::any_of(s.sources.begin(), s.sources.end(),
                               [name](const auto &src) { return src.second.buffer == name; });
        },
        [](u32) {});
}
void mt_hle_alGenSources(mt_cpu *cpu, std::uint32_t) noexcept {
    State &s = begin("alGenSources");
    gen_names(cpu, s, s.sources, s.next_source, "alGenSources");
}
// A deleted source stops.
void mt_hle_alDeleteSources(mt_cpu *cpu, std::uint32_t) noexcept {
    State &s = begin("alDeleteSources");
    delete_names(
        cpu, s, s.sources, "alDeleteSources", [](u32) { return false; },
        [](u32 name) { mix::effect_stop(name); });
}

// void alBufferData(ALuint buffer, ALenum format, const ALvoid *data, ALsizei size,
//     ALsizei freq): the guest's samples are copied into host memory (the game frees its copy
// right after). 8-bit data is unsigned, 16-bit signed native-endian (little on every target);
// size must be a whole number of frames. A null `data` gives `size` bytes of silence. A source
// still playing the buffer's old samples goes on with them.
void mt_hle_alBufferData(mt_cpu *cpu, std::uint32_t) noexcept {
    State &s = begin("alBufferData");
    mt_args a = mt_args_begin(cpu);
    const u32 buffer = mt_arg_u32(&a), format = mt_arg_u32(&a), data = mt_arg_u32(&a);
    const auto size = mt_arg_i32(&a), freq = mt_arg_i32(&a);
    if (!need_context(s, "alBufferData")) {
        return;
    }
    const auto b = s.buffers.find(buffer);
    if (b == s.buffers.end()) {
        set_error(s, k_al_invalid_name, "alBufferData");
        return;
    }
    if (format < k_al_format_mono8 || format > k_al_format_stereo16) {
        set_error(s, k_al_invalid_enum, "alBufferData");
        return;
    }
    const u32 channels = format == k_al_format_stereo8 || format == k_al_format_stereo16 ? 2 : 1;
    const u32 bits = format == k_al_format_mono8 || format == k_al_format_stereo8 ? 8 : 16;
    const u32 frame_bytes = channels * bits / 8;
    if (size < 0 || freq <= 0 || static_cast<u32>(size) % frame_bytes != 0) {
        set_error(s, k_al_invalid_value, "alBufferData");
        return;
    }
    const u32 bytes = static_cast<u32>(size);
    if (bytes > k_max_buffer_bytes) {
        set_error(s, k_al_out_of_memory, "alBufferData");
        return;
    }
    // mt_g2h faults the guest on a bad range, as the device's copy would.
    const std::uint8_t *src = data != 0 && bytes != 0 ? mt_g2h(cpu, data, bytes) : nullptr;
    b->second = State::Buffer{format, bytes, static_cast<u32>(freq), true,
                              mix::make_pcm(src, bytes, channels, bits, static_cast<u32>(freq))};
    mt_tracef("audio", "alBufferData(%u, 0x%X, 0x%08X, %d, %d)", static_cast<unsigned>(buffer),
              static_cast<unsigned>(format), static_cast<unsigned>(data), static_cast<int>(size),
              static_cast<int>(freq));
}

// void alSourcei(ALuint source, ALenum param, ALint value): AL_BUFFER attaches a buffer (0
// detaches), not while the source plays (AL_INVALID_OPERATION); other parameters are accepted
// and have no effect (the game sets none).
void mt_hle_alSourcei(mt_cpu *cpu, std::uint32_t) noexcept {
    State &s = begin("alSourcei");
    mt_args a = mt_args_begin(cpu);
    const u32 source = mt_arg_u32(&a), param = mt_arg_u32(&a), value = mt_arg_u32(&a);
    if (!need_context(s, "alSourcei")) {
        return;
    }
    const auto src = s.sources.find(source);
    if (src == s.sources.end()) {
        set_error(s, k_al_invalid_name, "alSourcei");
        return;
    }
    if (param == k_al_buffer) {
        if (value != 0 && s.buffers.count(value) == 0) {
            set_error(s, k_al_invalid_value, "alSourcei");
            return;
        }
        if (mix::effect_playing(source)) {
            set_error(s, k_al_invalid_operation, "alSourcei");
            return;
        }
        src->second.buffer = value;
    }
}

// void alSourcePlay(ALuint source): (re)starts the source's voice at the first frame of its
// buffer's samples; a source without a buffer, or with an empty one, stops at once (the game's
// music ids have empty buffers).
void mt_hle_alSourcePlay(mt_cpu *cpu, std::uint32_t) noexcept {
    State &s = begin("alSourcePlay");
    const u32 source = cpu->r[0];
    if (!need_context(s, "alSourcePlay")) {
        return;
    }
    const auto src = s.sources.find(source);
    if (src == s.sources.end()) {
        set_error(s, k_al_invalid_name, "alSourcePlay");
        return;
    }
    src->second.plays++;
    s.st.source_plays++;
    const auto b = s.buffers.find(src->second.buffer);
    mix::effect_play(source, b != s.buffers.end() ? b->second.pcm : nullptr);
    mt_tracef("audio", "alSourcePlay(%u) (buffer %u)", static_cast<unsigned>(source),
              static_cast<unsigned>(src->second.buffer));
}

// void alListenerf(ALenum param, ALfloat value): AL_GAIN (the game's effects switch, 0 or 1)
// scales every effect; other parameters are accepted.
void mt_hle_alListenerf(mt_cpu *cpu, std::uint32_t) noexcept {
    State &s = begin("alListenerf");
    mt_args a = mt_args_begin(cpu);
    const u32 param = mt_arg_u32(&a);
    const float value = mt_arg_f32(&a);
    if (!need_context(s, "alListenerf")) {
        return;
    }
    if (param == k_al_gain) {
        if (!(value >= 0.0f)) {
            set_error(s, k_al_invalid_value, "alListenerf");
            return;
        }
        s.listener_gain = value;
        mix::set_listener_gain(value);
    }
    mt_tracef("audio", "alListenerf(0x%X, %g)", static_cast<unsigned>(param),
              static_cast<double>(value));
}

// --- AudioToolbox: the audio session and system sounds -------------------------------------------

// AudioSession's errors (AudioToolbox/AudioServices.h) and system sound ids.
constexpr std::int32_t k_session_bad_property_size = 0x2173697A; // '!siz'
constexpr u32 k_system_sound_vibrate = 0xFFF;                    // kSystemSoundID_Vibrate

// The host's vibrate hook (mt_audio_set_vibrate_hook): host state, kept across processes.
static mt_audio_vibrate_fn g_vibrate_hook = nullptr;
static void *g_vibrate_user = nullptr;

// OSStatus AudioSessionInitialize(CFRunLoopRef, CFStringRef mode,
//     AudioSessionInterruptionListener listener, void *userData): the listener is kept and
// never called. On the device it runs for a phone call, an alarm or another app taking the
// audio (state 1 begin, 0 end: docs/re/audio.md); a desktop has none of these, and the
// window losing focus is the platform's mute (#64), not an interruption: the game's own
// applicationWillResignActive (Clet_Pause) already stops its music and effects.
void mt_hle_AudioSessionInitialize(mt_cpu *cpu, std::uint32_t) noexcept {
    State &s = begin("AudioSessionInitialize");
    const u32 listener = cpu->r[2];
    if (s.session_initialized) {
        mt_ret_u32(cpu, static_cast<u32>(k_session_already_initialized));
        return;
    }
    s.session_initialized = true;
    s.session_listener = listener;
    mt_tracef("audio", "AudioSessionInitialize (listener 0x%08X)", static_cast<unsigned>(listener));
    mt_ret_u32(cpu, static_cast<u32>(k_no_err));
}

// OSStatus AudioSessionSetActive(Boolean active)
void mt_hle_AudioSessionSetActive(mt_cpu *cpu, std::uint32_t) noexcept {
    State &s = begin("AudioSessionSetActive");
    if (!s.session_initialized) {
        mt_ret_u32(cpu, static_cast<u32>(k_session_not_initialized));
        return;
    }
    s.st.session_active = (cpu->r[0] & 0xFFu) != 0 ? 1u : 0u;
    mt_ret_u32(cpu, static_cast<u32>(k_no_err));
}

// OSStatus AudioSessionSetProperty(AudioSessionPropertyID id, UInt32 size, const void *data):
// the category ('acat') is kept, other properties accepted.
void mt_hle_AudioSessionSetProperty(mt_cpu *cpu, std::uint32_t) noexcept {
    State &s = begin("AudioSessionSetProperty");
    const u32 id = cpu->r[0], size = cpu->r[1], data = cpu->r[2];
    if (!s.session_initialized) {
        mt_ret_u32(cpu, static_cast<u32>(k_session_not_initialized));
        return;
    }
    if (id == 0x61636174u /* 'acat' */) {
        if (size != 4) {
            mt_ret_u32(cpu, static_cast<u32>(k_session_bad_property_size));
            return;
        }
        if (data == 0) {
            mt_ret_u32(cpu, static_cast<u32>(k_param_err));
            return;
        }
        s.session_category = rd32(cpu, data);
    }
    mt_ret_u32(cpu, static_cast<u32>(k_no_err));
}

// void AudioServicesPlaySystemSound(SystemSoundID id): counted. kSystemSoundID_Vibrate (0xFFF,
// the game's only id) calls the host's vibrate hook; any other id (a sound the game never
// registers with AudioServicesCreateSystemSoundID) does nothing.
void mt_hle_AudioServicesPlaySystemSound(mt_cpu *cpu, std::uint32_t) noexcept {
    State &s = begin("AudioServicesPlaySystemSound");
    const u32 id = cpu->r[0];
    s.st.system_sounds++;
    mt_tracef("audio", "AudioServicesPlaySystemSound(0x%X)", static_cast<unsigned>(id));
    if (id == k_system_sound_vibrate && g_vibrate_hook != nullptr) {
        g_vibrate_hook(g_vibrate_user);
    }
}

void mt_audio_set_vibrate_hook(mt_audio_vibrate_fn hook, void *user) noexcept {
    g_vibrate_hook = hook;
    g_vibrate_user = user;
}

// --- AudioToolbox: ExtAudioFile ------------------------------------------------------------------

// OSStatus ExtAudioFileOpenURL(CFURLRef url, ExtAudioFileRef *outFile): a PCM WAV the guest may
// read (the bundle's sound/ effects).
void mt_hle_ExtAudioFileOpenURL(mt_cpu *cpu, std::uint32_t) noexcept {
    State &s = begin("ExtAudioFileOpenURL");
    const u32 url = cpu->r[0], out = cpu->r[1];
    if (out == 0) {
        mt_ret_u32(cpu, static_cast<u32>(k_param_err));
        return;
    }
    const std::string path = url_host_path(cpu, url);
    std::vector<std::uint8_t> bytes;
    File f;
    std::int32_t status = k_no_err;
    pack::Sound replacement; // an audio pack's file for this one (#188, ADR 0106)
    if (!path.empty() && pack::effect(path, &replacement)) {
        pcm_file(replacement, &f);
    } else if (path.empty() || !read_file(path, &bytes)) {
        status = k_fnf_err;
    } else if (!parse_wav(bytes, &f)) {
        status = k_unsupported_file;
    }
    const u32 h = status == k_no_err ? new_handle(cpu) : 0u;
    if (status == k_no_err && h == 0) {
        status = k_param_err;
    }
    if (status != k_no_err) {
        s.st.files_failed++;
        mt_tracef("audio", "ExtAudioFileOpenURL(%s): error %d", path.c_str(),
                  static_cast<int>(status));
        mt_ret_u32(cpu, static_cast<u32>(status));
        return;
    }
    const File &kept = s.files[h] = std::move(f);
    s.st.files_opened++;
    wr32(cpu, out, h);
    mt_tracef("audio", "ExtAudioFileOpenURL(%s) -> 0x%08X: %u Hz, %u ch, %u bit, %llu frames%s%s",
              path.c_str(), static_cast<unsigned>(h), static_cast<unsigned>(kept.file.rate),
              static_cast<unsigned>(kept.file.channels), static_cast<unsigned>(kept.file.bits),
              static_cast<unsigned long long>(kept.frames),
              replacement.file.empty() ? "" : ", from the audio pack: ", replacement.file.c_str());
    mt_ret_u32(cpu, static_cast<u32>(k_no_err));
}

// OSStatus ExtAudioFileGetProperty(ExtAudioFileRef, ExtAudioFilePropertyID,
//     UInt32 *ioSize, void *outData): the file format ('ffmt'), the client format ('cfmt') and
// the length in frames ('#frm').
void mt_hle_ExtAudioFileGetProperty(mt_cpu *cpu, std::uint32_t) noexcept {
    State &s = begin("ExtAudioFileGetProperty");
    const u32 h = cpu->r[0], id = cpu->r[1], io_size = cpu->r[2], out = cpu->r[3];
    File *f = file_of(s, h);
    if (f == nullptr || io_size == 0 || out == 0) {
        mt_ret_u32(cpu, static_cast<u32>(k_param_err));
        return;
    }
    const u32 room = rd32(cpu, io_size);
    if (id == k_prop_file_format || id == k_prop_client_format) {
        if (room < k_asbd_size) {
            mt_ret_u32(cpu, static_cast<u32>(k_ext_invalid_property_size));
            return;
        }
        write_asbd(cpu, out, id == k_prop_client_format && f->has_client ? f->client : f->file);
        wr32(cpu, io_size, k_asbd_size);
    } else if (id == k_prop_length_frames) {
        if (room < 8) {
            mt_ret_u32(cpu, static_cast<u32>(k_ext_invalid_property_size));
            return;
        }
        std::memcpy(mt_g2h(cpu, out, 8), &f->frames, 8);
        wr32(cpu, io_size, 8);
    } else {
        mt_ret_u32(cpu, static_cast<u32>(k_ext_invalid_property));
        return;
    }
    mt_ret_u32(cpu, static_cast<u32>(k_no_err));
}

// OSStatus ExtAudioFileSetProperty(ExtAudioFileRef, ExtAudioFilePropertyID, UInt32 size,
//     const void *data): the client format. Linear PCM at the file's rate and channel count,
//     in the file's own format or as 16-bit signed integer (client_mode); a conversion we
//     don't make is 'fmt?' (kAudioConverterErr_FormatNotSupported) and keeps the old format.
void mt_hle_ExtAudioFileSetProperty(mt_cpu *cpu, std::uint32_t) noexcept {
    State &s = begin("ExtAudioFileSetProperty");
    const u32 h = cpu->r[0], id = cpu->r[1], size = cpu->r[2], data = cpu->r[3];
    File *f = file_of(s, h);
    if (f == nullptr || data == 0) {
        mt_ret_u32(cpu, static_cast<u32>(k_param_err));
        return;
    }
    if (id != k_prop_client_format) {
        mt_ret_u32(cpu, static_cast<u32>(k_ext_invalid_property));
        return;
    }
    if (size < k_asbd_size) {
        mt_ret_u32(cpu, static_cast<u32>(k_ext_invalid_property_size));
        return;
    }
    const Asbd c = read_asbd(cpu, data);
    if (c.format_id != k_format_lpcm) {
        mt_ret_u32(cpu, static_cast<u32>(k_ext_non_pcm_client_format));
        return;
    }
    if (c.channels == 0 || c.channels > 8 || c.bytes_per_frame == 0 || c.bytes_per_frame > 64 ||
        !(c.rate > 0)) {
        mt_ret_u32(cpu, static_cast<u32>(k_param_err));
        return;
    }
    const int mode = client_mode(f->file, c);
    if (mode == 0) {
        mt_tracef("audio",
                  "ExtAudioFileSetProperty(0x%08X, 'cfmt'): %g Hz, %u ch, %u bit, flags 0x%X from "
                  "%g Hz, %u ch, %u bit: not supported ('fmt?')",
                  static_cast<unsigned>(h), c.rate, static_cast<unsigned>(c.channels),
                  static_cast<unsigned>(c.bits), static_cast<unsigned>(c.flags), f->file.rate,
                  static_cast<unsigned>(f->file.channels), static_cast<unsigned>(f->file.bits));
        mt_ret_u32(cpu, static_cast<u32>(k_converter_format_not_supported));
        return;
    }
    f->client = c;
    f->has_client = true;
    f->to_s16 = mode == 2 && f->file.bits != 16;
    mt_ret_u32(cpu, static_cast<u32>(k_no_err));
}

// OSStatus ExtAudioFileRead(ExtAudioFileRef, UInt32 *ioNumberFrames, AudioBufferList *ioData):
// up to *ioNumberFrames of the next frames in the client format (the file's format until one
// is set), as many as are left and as fit in the one buffer's mDataByteSize. Sets
// *ioNumberFrames and mDataByteSize to what was read: 0 at the end of the file.
void mt_hle_ExtAudioFileRead(mt_cpu *cpu, std::uint32_t) noexcept {
    State &s = begin("ExtAudioFileRead");
    const u32 h = cpu->r[0], io_frames = cpu->r[1], list = cpu->r[2];
    File *f = file_of(s, h);
    if (f == nullptr || io_frames == 0 || list == 0) {
        mt_ret_u32(cpu, static_cast<u32>(k_param_err));
        return;
    }
    const Asbd &c = f->has_client ? f->client : f->file;
    // AudioBufferList { UInt32 mNumberBuffers; AudioBuffer { UInt32 mNumberChannels,
    // mDataByteSize; void *mData; } mBuffers[]; }: one buffer, as the client is interleaved
    if (rd32(cpu, list) != 1) {
        mt_ret_u32(cpu, static_cast<u32>(k_param_err));
        return;
    }
    const u32 buf = list + 4u;
    std::uint64_t frames = rd32(cpu, io_frames);
    frames = std::min<std::uint64_t>(frames, f->frames - std::min(f->frames, f->position));
    frames = std::min<std::uint64_t>(frames, rd32(cpu, buf + 4) / c.bytes_per_frame);
    const u32 bytes = static_cast<u32>(frames) * c.bytes_per_frame, data = rd32(cpu, buf + 8);
    if (bytes != 0 && data == 0) {
        mt_ret_u32(cpu, static_cast<u32>(k_param_err));
        return;
    }
    if (bytes != 0) {
        std::uint8_t *out = mt_g2h(cpu, data, bytes);
        if (f->to_s16) {
            convert_to_s16(*f, f->position, frames, out);
        } else {
            std::memcpy(out, f->data.data() + f->position * f->file.bytes_per_frame, bytes);
        }
    }
    wr32(cpu, buf + 4, bytes);
    f->position += frames;
    wr32(cpu, io_frames, static_cast<u32>(frames));
    mt_ret_u32(cpu, static_cast<u32>(k_no_err));
}

// OSStatus ExtAudioFileDispose(ExtAudioFileRef)
void mt_hle_ExtAudioFileDispose(mt_cpu *cpu, std::uint32_t) noexcept {
    State &s = begin("ExtAudioFileDispose");
    const u32 h = cpu->r[0];
    if (s.files.erase(h) == 0) {
        mt_ret_u32(cpu, static_cast<u32>(k_param_err));
        return;
    }
    free_handle(cpu, h);
    mt_ret_u32(cpu, static_cast<u32>(k_no_err));
}

mt_audio_stats mt_audio_get_stats(void) noexcept {
    State &s = state();
    mt_audio_stats st = s.st;
    st.buffers = static_cast<u32>(s.buffers.size());
    st.buffers_filled = 0;
    for (const auto &b : s.buffers) {
        st.buffers_filled += b.second.filled ? 1u : 0u;
    }
    st.sources = static_cast<u32>(s.sources.size());
    return st;
}

} // extern "C"
