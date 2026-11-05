// The WAV chunk walk (wav.h, #62, #188).
#include "audio/wav.h"

#include <cstring>

namespace mt_audio::wav {
namespace {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

u32 le32(const std::vector<std::uint8_t> &b, std::size_t at) {
    return static_cast<u32>(b[at]) | static_cast<u32>(b[at + 1]) << 8 |
           static_cast<u32>(b[at + 2]) << 16 | static_cast<u32>(b[at + 3]) << 24;
}
u32 le16(const std::vector<std::uint8_t> &b, std::size_t at) {
    return static_cast<u32>(b[at]) | static_cast<u32>(b[at + 1]) << 8;
}

constexpr u32 k_smpl_header = 36; // the sampler chunk's fields before its loops
constexpr u32 k_smpl_loop = 24;   // one loop: id, type, start, end, fraction, play count

} // namespace

bool parse(const std::vector<std::uint8_t> &b, Info *out) {
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) != 0 ||
        std::memcmp(b.data() + 8, "WAVE", 4) != 0) {
        return false;
    }
    bool have_fmt = false, have_data = false, have_smpl = false;
    u32 channels = 0, rate = 0, block_align = 0, bits = 0, data_size = 0;
    u64 smpl_start = 0, smpl_end = 0;
    std::size_t data_at = 0;
    std::size_t at = 12;
    while (at + 8 <= b.size()) {
        const u32 size = le32(b, at + 4);
        const std::size_t body = at + 8;
        if (size > b.size() - body) {
            // a truncated last chunk: the data chunk may still be usable up to the end
            if (!have_data && std::memcmp(b.data() + at, "data", 4) == 0) {
                data_size = static_cast<u32>(b.size() - body);
                data_at = body;
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
        } else if (!have_data && std::memcmp(b.data() + at, "data", 4) == 0) {
            data_size = size; // the first data chunk is the sound; later ones are ignored
            data_at = body;
            have_data = true;
        } else if (!have_smpl && std::memcmp(b.data() + at, "smpl", 4) == 0 &&
                   size >= k_smpl_header + k_smpl_loop && le32(b, body + 28) >= 1) {
            smpl_start = le32(b, body + k_smpl_header + 8);
            smpl_end = u64{le32(b, body + k_smpl_header + 12)} + 1; // the end sample is played
            have_smpl = true;
        }
        at = body + size + (size & 1u); // chunks are word-aligned
    }
    if (!have_fmt || !have_data || channels == 0 || channels > 8 || rate == 0 || rate > 384000 ||
        (bits != 8 && bits != 16 && bits != 24 && bits != 32) ||
        block_align != channels * (bits / 8)) {
        return false;
    }
    out->channels = channels;
    out->rate = rate;
    out->bits = bits;
    out->block_align = block_align;
    out->frames = data_size / block_align;
    out->data_at = data_at;
    out->has_loop = have_smpl && smpl_start < smpl_end && smpl_end <= out->frames;
    out->loop_start = out->has_loop ? smpl_start : 0;
    out->loop_end = out->has_loop ? smpl_end : 0;
    return true;
}

std::vector<std::int16_t> to_s16(const std::vector<std::uint8_t> &b, const Info &w) {
    const u32 bytes = w.bits / 8;
    const u64 samples = w.frames * w.channels;
    std::vector<std::int16_t> out(static_cast<std::size_t>(samples));
    const std::uint8_t *in = b.data() + w.data_at;
    for (u64 i = 0; i < samples; i++, in += bytes) {
        u32 v;
        if (bytes == 1) {
            v = static_cast<u32>(in[0] ^ 0x80u) << 8;
        } else {
            v = static_cast<u32>(in[bytes - 2]) | static_cast<u32>(in[bytes - 1]) << 8;
        }
        out[static_cast<std::size_t>(i)] = static_cast<std::int16_t>(static_cast<std::uint16_t>(v));
    }
    return out;
}

} // namespace mt_audio::wav
