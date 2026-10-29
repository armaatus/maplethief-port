// OpenAL (#61, ADR 0045), the audio session, system sounds and ExtAudioFile (silent: #117, ADR
// 0038), maplethief/audio.h. The game's use (docs/re/audio.md): CMSoundPlayer opens the default
// device, makes a context current, generates 44 buffers and 44 sources, decodes the 26 WAV
// effects through ExtAudioFile (Apple's MyGetOpenALAudioData) into the buffers, attaches each
// buffer to its source, and later plays an effect with alSourcePlay. Any AL error at start-up
// ends the app with exit(1), so the error state is kept as OpenAL keeps it.
//
// OpenAL plays through the mixer (mixer.h): alBufferData copies the guest's PCM into a host
// Pcm, alSourcePlay (re)starts the source's effect voice, alListenerf(AL_GAIN) is the
// listener gain, and the effects pause while the context is suspended or not current. No mixer
// call runs guest code or reads guest memory under the mixer's lock. Still to come: WAV
// decoding (#62), the audio session (#64).
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "audio/audio_internal.h"
#include "audio/mixer.h"
#include "cf/cf_objects.h"
#include "foundation/foundation.h"
#include "maplethief/diag.h"
#include "maplethief/hle/abi.h"
#include "maplethief/memory.h"

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
constexpr u32 k_prop_file_format = 0x66666D74;   // 'ffmt'
constexpr u32 k_prop_client_format = 0x63666D74; // 'cfmt'
constexpr u32 k_prop_length_frames = 0x2366726D; // '#frm'
constexpr u32 k_format_lpcm = 0x6C70636D;        // 'lpcm'

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
    std::uint64_t frames = 0, position = 0;
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

std::uint32_t le32(const std::vector<std::uint8_t> &b, std::size_t at) {
    return static_cast<u32>(b[at]) | static_cast<u32>(b[at + 1]) << 8 |
           static_cast<u32>(b[at + 2]) << 16 | static_cast<u32>(b[at + 3]) << 24;
}
std::uint32_t le16(const std::vector<std::uint8_t> &b, std::size_t at) {
    return static_cast<u32>(b[at]) | static_cast<u32>(b[at + 1]) << 8;
}

// A PCM WAV file's format and length (RIFF chunk walk; an untrusted file: every read is
// bounds-checked). False if it is no PCM WAV.
bool parse_wav(const std::vector<std::uint8_t> &b, File *out) {
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) != 0 ||
        std::memcmp(b.data() + 8, "WAVE", 4) != 0) {
        return false;
    }
    bool have_fmt = false, have_data = false;
    u32 channels = 0, rate = 0, block_align = 0, bits = 0, data_size = 0;
    std::size_t at = 12;
    while (at + 8 <= b.size()) {
        const u32 size = le32(b, at + 4);
        const std::size_t body = at + 8;
        if (size > b.size() - body) {
            // a truncated last chunk: the data chunk may still be usable up to the end
            if (std::memcmp(b.data() + at, "data", 4) == 0) {
                data_size = static_cast<u32>(b.size() - body);
                have_data = true;
            }
            break;
        }
        if (std::memcmp(b.data() + at, "fmt ", 4) == 0 && size >= 16) {
            const u32 tag = le16(b, body);
            if (tag != 1) { // WAVE_FORMAT_PCM only (every shipped WAV)
                return false;
            }
            channels = le16(b, body + 2);
            rate = le32(b, body + 4);
            block_align = le16(b, body + 12);
            bits = le16(b, body + 14);
            have_fmt = true;
        } else if (std::memcmp(b.data() + at, "data", 4) == 0) {
            data_size = size;
            have_data = true;
        }
        at = body + size + (size & 1u); // chunks are word-aligned
    }
    if (!have_fmt || !have_data || channels == 0 || channels > 8 || rate == 0 || rate > 384000 ||
        (bits != 8 && bits != 16 && bits != 24 && bits != 32) ||
        block_align != channels * (bits / 8)) {
        return false;
    }
    Asbd &f = out->file;
    f.rate = rate;
    f.format_id = k_format_lpcm;
    // kAudioFormatFlagIsSignedInteger (not for 8-bit WAV, which is unsigned) | IsPacked
    f.flags = (bits > 8 ? 0x4u : 0u) | 0x8u;
    f.bytes_per_packet = block_align;
    f.frames_per_packet = 1;
    f.bytes_per_frame = block_align;
    f.channels = channels;
    f.bits = bits;
    out->frames = data_size / block_align;
    return true;
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

std::string url_host_path(mt_cpu *cpu, u32 url) {
    std::string text;
    bool is_file = false;
    if (url == 0 || !mt_ns::url_string(cpu, url, &text, &is_file) || !is_file) {
        return "";
    }
    return mt_ns::host_path(text, false);
}

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

// OSStatus AudioSessionInitialize(CFRunLoopRef, CFStringRef mode,
//     AudioSessionInterruptionListener listener, void *userData): the listener is kept and
// never called (no interruptions).
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
        if (size != 4 || data == 0) {
            mt_ret_u32(cpu, static_cast<u32>(k_param_err));
            return;
        }
        s.session_category = rd32(cpu, data);
    }
    mt_ret_u32(cpu, static_cast<u32>(k_no_err));
}

// void AudioServicesPlaySystemSound(SystemSoundID id): counted (0xFFF, vibrate: no rumble yet).
void mt_hle_AudioServicesPlaySystemSound(mt_cpu *cpu, std::uint32_t) noexcept {
    State &s = begin("AudioServicesPlaySystemSound");
    s.st.system_sounds++;
    mt_tracef("audio", "AudioServicesPlaySystemSound(0x%X)", static_cast<unsigned>(cpu->r[0]));
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
    if (path.empty() || !read_file(path, &bytes)) {
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
    s.files[h] = f;
    s.st.files_opened++;
    wr32(cpu, out, h);
    mt_tracef("audio", "ExtAudioFileOpenURL(%s) -> 0x%08X: %u Hz, %u ch, %u bit, %llu frames",
              path.c_str(), static_cast<unsigned>(h), static_cast<unsigned>(f.file.rate),
              static_cast<unsigned>(f.file.channels), static_cast<unsigned>(f.file.bits),
              static_cast<unsigned long long>(f.frames));
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
//     const void *data): the client format (linear PCM at any rate and channel count).
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
    if (c.format_id != k_format_lpcm || c.channels == 0 || c.channels > 8 ||
        c.bytes_per_frame == 0 || c.bytes_per_frame > 64 || !(c.rate > 0)) {
        mt_ret_u32(cpu, static_cast<u32>(k_param_err)); // only linear PCM output is supported
        return;
    }
    f->client = c;
    f->has_client = true;
    mt_ret_u32(cpu, static_cast<u32>(k_no_err));
}

// OSStatus ExtAudioFileRead(ExtAudioFileRef, UInt32 *ioFrames, AudioBufferList *ioData): the
// next frames as silence in the client format (the length is the file's, converted to the
// client's rate); #62 decodes the samples.
void mt_hle_ExtAudioFileRead(mt_cpu *cpu, std::uint32_t) noexcept {
    State &s = begin("ExtAudioFileRead");
    const u32 h = cpu->r[0], io_frames = cpu->r[1], list = cpu->r[2];
    File *f = file_of(s, h);
    if (f == nullptr || io_frames == 0 || list == 0) {
        mt_ret_u32(cpu, static_cast<u32>(k_param_err));
        return;
    }
    const Asbd &c = f->has_client ? f->client : f->file;
    const std::uint64_t total = c.rate == f->file.rate
                                    ? f->frames
                                    : static_cast<std::uint64_t>(std::ceil(
                                          static_cast<double>(f->frames) * c.rate / f->file.rate));
    std::uint64_t frames = rd32(cpu, io_frames);
    frames = std::min<std::uint64_t>(frames, total - std::min(total, f->position));
    // AudioBufferList { UInt32 mNumberBuffers; AudioBuffer { UInt32 mNumberChannels,
    // mDataByteSize; void *mData; } mBuffers[]; }
    const u32 count = rd32(cpu, list);
    if (count == 0 || count > 8) {
        mt_ret_u32(cpu, static_cast<u32>(k_param_err));
        return;
    }
    const u32 per_buffer = count == 1 ? c.bytes_per_frame : c.bytes_per_frame / count;
    for (u32 i = 0; i < count; i++) {
        const u32 buf = list + 4u + 12u * i;
        if (per_buffer != 0) {
            frames = std::min<std::uint64_t>(frames, rd32(cpu, buf + 4) / per_buffer);
        }
    }
    for (u32 i = 0; i < count; i++) {
        const u32 buf = list + 4u + 12u * i;
        const u32 bytes = static_cast<u32>(frames) * per_buffer, data = rd32(cpu, buf + 8);
        if (bytes != 0) {
            std::memset(mt_g2h(cpu, data, bytes), 0, bytes);
        }
        wr32(cpu, buf + 4, bytes);
    }
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
