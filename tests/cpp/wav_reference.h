// A strict reference reader for the shipped WAVs (#62), independent of the runtime's
// ExtAudioFile: the RIFF chunk walk of mtport.audio.read_wav (tools/mtport/audio_headers.py),
// with the same refusals (a chunk running past the RIFF size, a second fmt or data chunk, data
// before fmt, an inconsistent PCM fmt), giving the fmt fields and the data chunk's bytes.
// Shared by audio_real_test.cpp and boot_real_test.cpp, which compare the runtime's decoding
// with it.
#ifndef MAPLETHIEF_TESTS_CPP_WAV_REFERENCE_H
#define MAPLETHIEF_TESTS_CPP_WAV_REFERENCE_H

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace wav_reference {

struct Wav {
    std::string error; // empty if the file reads
    std::uint32_t tag = 0, channels = 0, rate = 0, byte_rate = 0, align = 0, bits = 0;
    std::vector<std::uint8_t> data; // the data chunk
    std::vector<std::string> chunks;
    std::uint64_t file_size = 0;
    std::uint64_t frames() const { return align == 0 ? 0 : data.size() / align; }
};

inline std::uint32_t le(const std::vector<std::uint8_t> &b, std::size_t at, int bytes) {
    std::uint32_t v = 0;
    for (int k = bytes - 1; k >= 0; k--) {
        v = v << 8 | b[at + static_cast<std::size_t>(k)];
    }
    return v;
}

inline Wav read(const std::filesystem::path &path) {
    Wav w;
    std::ifstream in(path, std::ios::binary);
    const std::vector<std::uint8_t> b((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
    w.file_size = b.size();
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) != 0 ||
        std::memcmp(b.data() + 8, "WAVE", 4) != 0) {
        w.error = "not a RIFF/WAVE file";
        return w;
    }
    const std::uint64_t end = 8ull + le(b, 4, 4);
    if (end > b.size()) {
        w.error = "RIFF size runs past the end";
        return w;
    }
    bool fmt = false, data = false;
    std::uint64_t pos = 12;
    while (pos < end) {
        if (pos + 8 > end) {
            w.error = "truncated chunk header";
            return w;
        }
        const std::string id(reinterpret_cast<const char *>(b.data() + pos), 4);
        const std::uint64_t size = le(b, pos + 4, 4), body = pos + 8;
        if (body + size > end) {
            w.error = "chunk " + id + " runs past the end";
            return w;
        }
        w.chunks.push_back(id);
        if (id == "fmt ") {
            if (fmt || size < 16) {
                w.error = "a second or short fmt chunk";
                return w;
            }
            fmt = true;
            w.tag = le(b, body, 2);
            w.channels = le(b, body + 2, 2);
            w.rate = le(b, body + 4, 4);
            w.byte_rate = le(b, body + 8, 4);
            w.align = le(b, body + 12, 2);
            w.bits = le(b, body + 14, 2);
        } else if (id == "data") {
            if (data || !fmt) {
                w.error = "a second data chunk, or data before fmt";
                return w;
            }
            data = true;
            w.data.assign(b.begin() + static_cast<std::ptrdiff_t>(body),
                          b.begin() + static_cast<std::ptrdiff_t>(body + size));
        }
        pos = std::min<std::uint64_t>(end, body + size + (size & 1)); // padded to even sizes
    }
    if (!fmt || !data) {
        w.error = "no fmt or no data chunk";
    } else if (w.channels == 0 || w.rate == 0 || w.align == 0 ||
               (w.tag == 1 &&
                (w.align != w.channels * ((w.bits + 7) / 8) || w.byte_rate != w.rate * w.align))) {
        w.error = "inconsistent fmt";
    }
    return w;
}

} // namespace wav_reference

#endif // MAPLETHIEF_TESTS_CPP_WAV_REFERENCE_H
