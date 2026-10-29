// CommonCrypto's one-shot digests (libSystem.B.dylib, #117): CC_MD5 and CC_SHA1, implemented
// for real (RFC 1321, FIPS 180-4) so that the values the game derives from them are the
// device's for the same input. -[UIDevice(IdentifierAddition) uniqueGlobalDeviceIdentifier]
// hashes the MAC address with CC_MD5 (the device identifier of the hello packet,
// docs/re/network.md); CC_SHA1 is imported by Chartboost.
#include <cstdint>
#include <cstring>

#include "hle/hle.h"
#include "maplethief/hle/abi.h"
#include "maplethief/memory.h"

namespace {

using u32 = std::uint32_t;
using u8 = std::uint8_t;

u32 rotl(u32 x, u32 n) { return (x << n) | (x >> (32u - n)); }

// --- MD5 (RFC 1321) ---------------------------------------------------------------------------

constexpr u32 k_md5_t[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};
constexpr u32 k_md5_s[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                             5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                             4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                             6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

void md5_block(u32 h[4], const u8 *p) {
    u32 m[16];
    for (u32 i = 0; i < 16; i++) {
        m[i] = static_cast<u32>(p[4 * i]) | static_cast<u32>(p[4 * i + 1]) << 8 |
               static_cast<u32>(p[4 * i + 2]) << 16 | static_cast<u32>(p[4 * i + 3]) << 24;
    }
    u32 a = h[0], b = h[1], c = h[2], d = h[3];
    for (u32 i = 0; i < 64; i++) {
        u32 f, g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        const u32 t = d;
        d = c;
        c = b;
        b = b + rotl(a + f + k_md5_t[i] + m[g], k_md5_s[i]);
        a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
}

// --- SHA-1 (FIPS 180-4) -------------------------------------------------------------------------

void sha1_block(u32 h[5], const u8 *p) {
    u32 w[80];
    for (u32 i = 0; i < 16; i++) {
        w[i] = static_cast<u32>(p[4 * i]) << 24 | static_cast<u32>(p[4 * i + 1]) << 16 |
               static_cast<u32>(p[4 * i + 2]) << 8 | static_cast<u32>(p[4 * i + 3]);
    }
    for (u32 i = 16; i < 80; i++) {
        w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (u32 i = 0; i < 80; i++) {
        u32 f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        const u32 t = rotl(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rotl(b, 30);
        b = a;
        a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

// The Merkle-Damgard padding both use: 0x80, zeros, the bit length (little-endian for MD5,
// big-endian for SHA-1) in the last 8 bytes of a 64-byte block.
template <typename Block, typename State>
void digest(const u8 *data, u32 len, State *h, Block block, bool big_endian_length) {
    u32 at = 0;
    for (; len - at >= 64; at += 64) {
        block(h, data + at);
    }
    u8 tail[128] = {};
    const u32 rest = len - at;
    std::memcpy(tail, data + at, rest);
    tail[rest] = 0x80;
    const u32 blocks = rest + 1 + 8 <= 64 ? 1u : 2u;
    const std::uint64_t bits = static_cast<std::uint64_t>(len) * 8u;
    u8 *end = tail + 64 * blocks - 8;
    for (u32 i = 0; i < 8; i++) {
        end[big_endian_length ? 7 - i : i] = static_cast<u8>(bits >> (8 * i));
    }
    for (u32 i = 0; i < blocks; i++) {
        block(h, tail + 64 * i);
    }
}

// unsigned char *CC_xxx(const void *data, CC_LONG len, unsigned char *md): md must be non-NULL
// (CommonCrypto writes through it); returns md.
const u8 k_empty = 0;
const u8 *input(mt_cpu *cpu, u32 data, u32 len) {
    return len != 0 ? mt_g2h(cpu, data, len) : &k_empty;
}

} // namespace

extern "C" void mt_hle_CC_MD5(mt_cpu *cpu, std::uint32_t) noexcept {
    const u32 data = cpu->r[0], len = cpu->r[1], md = cpu->r[2];
    u32 h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    digest(input(cpu, data, len), len, h, md5_block, false);
    u8 *out = mt_g2h(cpu, md, 16);
    for (u32 i = 0; i < 16; i++) {
        out[i] = static_cast<u8>(h[i / 4] >> (8 * (i % 4)));
    }
    mt_ret_u32(cpu, md);
}

extern "C" void mt_hle_CC_SHA1(mt_cpu *cpu, std::uint32_t) noexcept {
    const u32 data = cpu->r[0], len = cpu->r[1], md = cpu->r[2];
    u32 h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    digest(input(cpu, data, len), len, h, sha1_block, true);
    u8 *out = mt_g2h(cpu, md, 20);
    for (u32 i = 0; i < 20; i++) {
        out[i] = static_cast<u8>(h[i / 4] >> (24 - 8 * (i % 4)));
    }
    mt_ret_u32(cpu, md);
}
