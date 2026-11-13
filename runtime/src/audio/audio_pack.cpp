// Audio packs (#188, ADR 0106, maplethief/audio.h, audio_pack.h).
//
// The pack's files are untrusted data: their names are matched when the pack is indexed (at most
// MT_AUDIO_PACK_MAX_FILES files; a symbolic link is skipped, #413), each file is read (at most
// MT_AUDIO_PACK_MAX_FILE bytes, checked before reading) when the game opens the sound it
// replaces, its format is checked (WAV: wav.h's chunk walk; MP3: dr_mp3 finds frames; FLAC: its
// STREAMINFO) and then its rate, channels and length against the limits before anything is
// decoded. A rejected file is logged once and never read again; the game's own file is used.
#include "audio/audio_pack.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <new>
#include <string>
#include <system_error>

#include "audio/mp3_stream.h"
#include "audio/wav.h"
#include "diag/diag_internal.h"
#include "maplethief/audio.h"

namespace mt_audio::pack {
namespace {

namespace fs = std::filesystem;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr std::uintmax_t k_max_loop_file = 256; // a .loop file: two numbers

fs::path host_path(const std::string &utf8) {
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string utf8_of(const fs::path &p) {
    const std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

void logf(const char *format, ...) {
    char line[768];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof line, format, args);
    va_end(args);
    mt_diag_log(line);
}

std::string lower(std::string s) {
    for (char &c : s) {
        c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return s;
}

// The extension of a lower-case name ("" if none), and its stem.
std::string extension(const std::string &name) {
    const std::size_t dot = name.rfind('.');
    return dot == std::string::npos || dot == 0 ? std::string() : name.substr(dot + 1);
}
std::string stem(const std::string &name) {
    const std::size_t dot = name.rfind('.');
    return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

struct File {
    std::string path; // UTF-8
    bool bad = false; // rejected once: never read again
};

struct Pack {
    bool open = false;
    std::string dir;
    std::map<std::string, File> sounds; // by lower-case file name (.wav, .mp3)
    std::map<std::string, File> loops;  // by lower-case stem (.loop)
};

Pack g_pack;
mt_audio_pack_stats g_stats{};
u32 g_max_files = MT_AUDIO_PACK_MAX_FILES; // the files a pack indexes (lower in a test)

bool read_file(const std::string &path, std::vector<std::uint8_t> *out, std::string *why) {
    std::error_code ec;
    const fs::path p = host_path(path);
    const std::uintmax_t size = fs::file_size(p, ec);
    if (ec) {
        *why = "cannot be read";
        return false;
    }
    if (size > MT_AUDIO_PACK_MAX_FILE) {
        *why = "is larger than " + std::to_string(MT_AUDIO_PACK_MAX_FILE >> 20) + " MiB";
        return false;
    }
    if (size == 0) {
        *why = "is empty";
        return false;
    }
    std::ifstream in(p, std::ios::binary);
    out->resize(static_cast<std::size_t>(size));
    if (!in ||
        !in.read(reinterpret_cast<char *>(out->data()), static_cast<std::streamsize>(size))) {
        *why = "cannot be read";
        return false;
    }
    return true;
}

// The pack file for the bundle file at host path `path`: the same name first, then the stem
// with .wav, .mp3, .flac. Null without a pack or a file.
File *find(const std::string &path) {
    if (!g_pack.open) {
        return nullptr;
    }
    std::string name = path;
    const std::size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) {
        name = name.substr(slash + 1);
    }
    name = lower(name);
    const std::string s = stem(name);
    for (const std::string &candidate : {name, s + ".wav", s + ".mp3", s + ".flac"}) {
        const auto it = g_pack.sounds.find(candidate);
        if (it != g_pack.sounds.end()) {
            return &it->second;
        }
    }
    return nullptr;
}

void reject(File &f, const char *role, const std::string &original, const std::string &why) {
    f.bad = true;
    ++g_stats.rejected;
    logf("audiopack: %s %s %s: rejected, the game's %s kept", role, f.path.c_str(), why.c_str(),
         original.c_str());
}

std::string base_name(const std::string &path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// The rate and channels check every pack file passes; "" if they are within the limits.
std::string check_format(u32 rate, u32 channels) {
    if (channels < 1 || channels > 2) {
        return "has " + std::to_string(channels) + " channels (1 or 2)";
    }
    if (rate < MT_AUDIO_PACK_MIN_RATE || rate > MT_AUDIO_PACK_MAX_RATE) {
        return "is at " + std::to_string(rate) + " Hz (" + std::to_string(MT_AUDIO_PACK_MIN_RATE) +
               " to " + std::to_string(MT_AUDIO_PACK_MAX_RATE) + ")";
    }
    return "";
}

std::string too_long(u32 seconds) { return "is longer than " + std::to_string(seconds) + " s"; }

Mp3Stream::Codec codec_of(const std::string &path) {
    return lower(extension(path)) == "flac" ? Mp3Stream::Codec::kFlac : Mp3Stream::Codec::kMp3;
}

// Why a compressed file opened no stream.
std::string no_audio(const std::string &path, u32 max_seconds) {
    const bool flac = codec_of(path) == Mp3Stream::Codec::kFlac;
    return std::string("holds no ") + (flac ? "FLAC" : "MP3") + " audio of 1 or 2 channels" +
           (flac ? " with its length" : "") + ", or " + too_long(max_seconds);
}

// A WAV's samples and format, checked; "" if it is good.
std::string wav_pcm(const std::vector<std::uint8_t> &bytes, u32 max_seconds, wav::Info *w,
                    std::vector<std::int16_t> *samples) {
    if (!wav::parse(bytes, w)) {
        return "is no PCM WAV";
    }
    std::string why = check_format(w->rate, w->channels);
    if (!why.empty()) {
        return why;
    }
    if (w->frames == 0) {
        return "holds no samples";
    }
    if (w->frames > u64{w->rate} * max_seconds) {
        return too_long(max_seconds);
    }
    *samples = wav::to_s16(bytes, *w);
    return "";
}

// <stem>.loop: "start end" (frames, end excluded), lines starting with '#' ignored.
bool read_loop(const std::string &stem_name, Mp3Stream::Loop *out) {
    const auto it = g_pack.loops.find(stem_name);
    if (it == g_pack.loops.end() || it->second.bad) {
        return false;
    }
    File &f = it->second;
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(host_path(f.path), ec);
    std::string text;
    bool ok = !ec && size <= k_max_loop_file;
    if (ok) {
        std::ifstream in(host_path(f.path), std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(in), {});
        ok = in.good() || in.eof();
    }
    u64 v[2] = {0, 0};
    int n = 0;
    bool comment = false, in_number = false;
    for (std::size_t i = 0; ok && i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\n' || c == '\r') {
            comment = false;
            in_number = false;
            continue;
        }
        if (comment) {
            continue;
        }
        if (c == '#' && !in_number) {
            comment = true;
        } else if (c >= '0' && c <= '9') {
            if (!in_number) {
                ok = n < 2;
                in_number = ok;
                n += ok ? 1 : 0;
            }
            if (ok) {
                u64 &x = v[n - 1];
                ok = x <= (UINT64_MAX - 9) / 10;
                x = x * 10 + static_cast<u64>(c - '0');
            }
        } else if (c == ' ' || c == '\t' || c == ',') {
            in_number = false;
        } else {
            ok = false;
        }
    }
    if (!ok || n != 2 || v[0] >= v[1]) {
        f.bad = true;
        logf("audiopack: %s is not \"start end\" (frames, start below end): no loop",
             f.path.c_str());
        return false;
    }
    out->start = v[0];
    out->end = v[1];
    return true;
}

} // namespace

std::uint32_t set_max_files(std::uint32_t n) {
    const u32 old = g_max_files;
    g_max_files = n;
    return old;
}

bool effect(const std::string &path, Sound *out) {
    if (!g_pack.open) {
        return false;
    }
    ++g_stats.effects;
    File *f = find(path);
    if (f == nullptr || f->bad) {
        return false;
    }
    const std::string original = base_name(path);
    try {
        std::vector<std::uint8_t> bytes;
        std::string why;
        if (read_file(f->path, &bytes, &why)) {
            if (lower(extension(f->path)) == "wav") {
                wav::Info w;
                why = wav_pcm(bytes, MT_AUDIO_PACK_MAX_EFFECT_SECONDS, &w, &out->samples);
                out->rate = w.rate;
                out->channels = w.channels;
            } else {
                // The header's rate, channels and length are checked before any frame is
                // decoded (#413): open_pack() reads only the header and walks the MP3's frames.
                const auto stream =
                    Mp3Stream::open_pack(std::move(bytes), codec_of(f->path), nullptr, false,
                                         MT_AUDIO_PACK_MAX_EFFECT_SECONDS);
                if (!stream) {
                    why = no_audio(f->path, MT_AUDIO_PACK_MAX_EFFECT_SECONDS);
                } else if (why = check_format(stream->rate(), stream->channels()); why.empty()) {
                    out->rate = stream->rate();
                    out->channels = stream->channels();
                    why = decode_whole(*stream, &out->samples) ? "" : "holds no samples";
                }
            }
        }
        if (!why.empty()) {
            reject(*f, "effect", original, why);
            return false;
        }
    } catch (const std::bad_alloc &) {
        reject(*f, "effect", original, "does not fit in memory");
        return false;
    }
    out->file = f->path;
    ++g_stats.effects_replaced;
    return true;
}

std::shared_ptr<Mp3Stream> music(const std::string &path, bool gapless, std::string *file) {
    if (!g_pack.open) {
        return nullptr;
    }
    ++g_stats.music;
    File *f = find(path);
    if (f == nullptr || f->bad) {
        return nullptr;
    }
    const std::string original = base_name(path);
    std::shared_ptr<Mp3Stream> stream;
    try {
        Mp3Stream::Loop loop;
        bool has_loop = read_loop(stem(lower(original)), &loop);
        std::vector<std::uint8_t> bytes;
        std::string why;
        if (read_file(f->path, &bytes, &why)) {
            if (lower(extension(f->path)) == "wav") {
                wav::Info w;
                std::vector<std::int16_t> samples;
                why = wav_pcm(bytes, MT_AUDIO_PACK_MAX_MUSIC_SECONDS, &w, &samples);
                if (why.empty()) {
                    bytes = {}; // the file's bytes are no longer needed
                    if (!has_loop && w.has_loop) {
                        loop = Mp3Stream::Loop{w.loop_start, w.loop_end};
                        has_loop = true;
                    }
                    stream = Mp3Stream::open_pcm(std::move(samples), w.rate, w.channels,
                                                 has_loop ? &loop : nullptr, gapless);
                }
            } else {
                stream = Mp3Stream::open_pack(std::move(bytes), codec_of(f->path),
                                              has_loop ? &loop : nullptr, gapless,
                                              MT_AUDIO_PACK_MAX_MUSIC_SECONDS);
                if (!stream) {
                    why = no_audio(f->path, MT_AUDIO_PACK_MAX_MUSIC_SECONDS);
                }
            }
            if (why.empty() && stream) {
                why = check_format(stream->rate(), stream->channels());
            }
            if (why.empty() && !stream) {
                why = "holds no samples";
            }
        }
        if (!why.empty()) {
            reject(*f, "music", original, why);
            return nullptr;
        }
        if (has_loop && stream->loop().end == 0) {
            logf("audiopack: the loop %llu to %llu of %s is outside its %llu frames: no loop",
                 static_cast<unsigned long long>(loop.start),
                 static_cast<unsigned long long>(loop.end), f->path.c_str(),
                 static_cast<unsigned long long>(stream->full_length()));
        }
    } catch (const std::bad_alloc &) {
        reject(*f, "music", original, "does not fit in memory");
        return nullptr;
    }
    *file = f->path;
    ++g_stats.music_replaced;
    return stream;
}

} // namespace mt_audio::pack

using mt_audio::pack::g_pack;
using mt_audio::pack::g_stats;

extern "C" int mt_audio_pack_open(const char *dir) noexcept {
    namespace fs = std::filesystem;
    mt_audio_pack_close();
    try {
        const std::string d = dir != nullptr ? dir : "";
        const fs::path root = mt_audio::pack::host_path(d);
        std::error_code ec;
        if (d.empty() || !fs::is_directory(root, ec)) {
            mt_audio::pack::logf("audiopack: the pack %s is no directory: no pack", d.c_str());
            return -1;
        }
        mt_audio::pack::Pack pack;
        pack.dir = d;
        mt_audio_pack_stats st{};
        std::uint64_t links = 0, skipped = 0;
        std::string first_link;
        for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
            // A pack's files are its own: a symbolic link is skipped, never followed (#413).
            std::error_code fe;
            const fs::file_status status = it->symlink_status(fe);
            if (fs::is_symlink(status)) {
                first_link = links++ == 0 ? mt_audio::pack::utf8_of(it->path()) : first_link;
                continue;
            }
            if (fe || !fs::is_regular_file(status)) {
                continue;
            }
            const std::string name =
                mt_audio::pack::lower(mt_audio::pack::utf8_of(it->path().filename()));
            const std::string ext = mt_audio::pack::extension(name);
            const bool sound = ext == "wav" || ext == "mp3" || ext == "flac";
            if (!sound && ext != "loop") {
                continue;
            }
            if (st.files + st.loops >= mt_audio::pack::g_max_files) {
                ++skipped;
                continue;
            }
            const mt_audio::pack::File file{mt_audio::pack::utf8_of(it->path()), false};
            if (sound) {
                pack.sounds[name] = file;
                ++st.files;
            } else {
                pack.loops[mt_audio::pack::stem(name)] = file;
                ++st.loops;
            }
        }
        if (links != 0) {
            mt_audio::pack::logf("audiopack: pack %s: %llu symbolic links not followed (the "
                                 "first: %s)",
                                 d.c_str(), static_cast<unsigned long long>(links),
                                 first_link.c_str());
        }
        if (skipped != 0) {
            mt_audio::pack::logf("audiopack: pack %s: %llu files past the first %u skipped",
                                 d.c_str(), static_cast<unsigned long long>(skipped),
                                 static_cast<unsigned>(mt_audio::pack::g_max_files));
        }
        pack.open = true;
        g_pack = std::move(pack);
        g_stats = st;
        mt_audio::pack::logf("audiopack: pack %s: %u sound files, %u loops", d.c_str(),
                             static_cast<unsigned>(st.files), static_cast<unsigned>(st.loops));
        return static_cast<int>(st.files);
    } catch (const std::exception &) {
        mt_audio::pack::logf("audiopack: the pack %s cannot be indexed: no pack",
                             dir != nullptr ? dir : "");
        return -1;
    }
}

extern "C" void mt_audio_pack_close(void) noexcept {
    g_pack = mt_audio::pack::Pack{};
    g_stats = mt_audio_pack_stats{};
}

extern "C" mt_audio_pack_stats mt_audio_pack_get_stats(void) noexcept { return g_stats; }
