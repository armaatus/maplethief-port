// mt_gfx textures (#55, ADR 0039, maplethief/gfx.h): handles, level-0 images stored as RGBA8,
// filters and wrap modes.
//
// Every uploaded format/type pair has one conversion function that turns a row of source texels
// into RGBA8, listed in k_conversions: all seven pairs of GL ES 1.1 (#57, ADR 0041). The game
// uploads only GL_RGBA/GL_UNSIGNED_BYTE (docs/ANALYSIS.md -> Rendering); the others are there so
// that no valid upload is ever left empty. A conversion expands the texels and nothing else: the
// bytes the game uploads (CoreGraphics' premultiplied RGBA) are kept exactly, and the blend
// treats them as GL does (ADR 0041). The RGBA8 that results is what GL_MODULATE, the only
// texture environment the game uses, reads from each base format (GL ES 1.1 table 3.15):
//   GL_ALPHA           (1, 1, 1, A): C = Cf, A = Af * At
//   GL_LUMINANCE       (L, L, L, 1): C = Cf * Lt, A = Af
//   GL_LUMINANCE_ALPHA (L, L, L, A)
//   GL_RGB             (R, G, B, 1)
// An n-bit component c of a packed type becomes round(c * 255 / (2^n - 1)) (GL's conversion to
// [0, 1], section 2.1.2, then to 8 bits). Packed texels are 16-bit words in the guest's
// (little-endian) byte order.
//
// The replacement hook (the seam for HD textures and mods, M10) sees each upload with pixels and
// its FNV-1a 64 content hash, and may hand back an RGBA8 image k times the logical size; the
// texture then samples that image (its size is the texel size the rasterizer filters at) while
// keeping the upload's logical size.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <unordered_map>

#include "diag/diag_internal.h"
#include "gfx/gfx_internal.h"

namespace mt_gfx {
namespace {

// Converts `width` source texels at `src` into RGBA8 at `dst`.
using ConvertRow = void (*)(const u8 *src, u8 *dst, u32 width) noexcept;

struct Conversion {
    u32 format, type;
    u32 bytes_per_texel; // of the source
    ConvertRow row;
};

void rgba_unsigned_byte(const u8 *src, u8 *dst, u32 width) noexcept {
    std::memcpy(dst, src, static_cast<std::size_t>(width) * 4u);
}

void rgb_unsigned_byte(const u8 *src, u8 *dst, u32 width) noexcept {
    for (u32 i = 0; i < width; ++i, src += 3, dst += 4) {
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
        dst[3] = 255;
    }
}

void alpha_unsigned_byte(const u8 *src, u8 *dst, u32 width) noexcept {
    for (u32 i = 0; i < width; ++i, dst += 4) {
        dst[0] = dst[1] = dst[2] = 255;
        dst[3] = src[i];
    }
}

void luminance_unsigned_byte(const u8 *src, u8 *dst, u32 width) noexcept {
    for (u32 i = 0; i < width; ++i, dst += 4) {
        dst[0] = dst[1] = dst[2] = src[i];
        dst[3] = 255;
    }
}

void luminance_alpha_unsigned_byte(const u8 *src, u8 *dst, u32 width) noexcept {
    for (u32 i = 0; i < width; ++i, src += 2, dst += 4) {
        dst[0] = dst[1] = dst[2] = src[0];
        dst[3] = src[1];
    }
}

// An n-bit component (max = 2^n - 1) to 8 bits, rounded: round(c * 255 / max).
constexpr u8 widen(u32 c, u32 max) noexcept { return static_cast<u8>((c * 255u + max / 2u) / max); }

inline u32 word(const u8 *p) noexcept {
    return static_cast<u32>(p[0]) | static_cast<u32>(p[1]) << 8;
}

void rgb_unsigned_short_5_6_5(const u8 *src, u8 *dst, u32 width) noexcept {
    for (u32 i = 0; i < width; ++i, src += 2, dst += 4) {
        const u32 v = word(src);
        dst[0] = widen(v >> 11, 31);
        dst[1] = widen((v >> 5) & 63u, 63);
        dst[2] = widen(v & 31u, 31);
        dst[3] = 255;
    }
}

void rgba_unsigned_short_4_4_4_4(const u8 *src, u8 *dst, u32 width) noexcept {
    for (u32 i = 0; i < width; ++i, src += 2, dst += 4) {
        const u32 v = word(src);
        dst[0] = widen(v >> 12, 15);
        dst[1] = widen((v >> 8) & 15u, 15);
        dst[2] = widen((v >> 4) & 15u, 15);
        dst[3] = widen(v & 15u, 15);
    }
}

void rgba_unsigned_short_5_5_5_1(const u8 *src, u8 *dst, u32 width) noexcept {
    for (u32 i = 0; i < width; ++i, src += 2, dst += 4) {
        const u32 v = word(src);
        dst[0] = widen(v >> 11, 31);
        dst[1] = widen((v >> 6) & 31u, 31);
        dst[2] = widen((v >> 1) & 31u, 31);
        dst[3] = (v & 1u) != 0 ? 255 : 0;
    }
}

// GL ES 1.1 table 3.4: every format/type pair glTexImage2D accepts.
constexpr Conversion k_conversions[] = {
    {MT_GFX_RGBA, MT_GFX_UNSIGNED_BYTE, 4, rgba_unsigned_byte},
    {MT_GFX_RGB, MT_GFX_UNSIGNED_BYTE, 3, rgb_unsigned_byte},
    {MT_GFX_ALPHA, MT_GFX_UNSIGNED_BYTE, 1, alpha_unsigned_byte},
    {MT_GFX_LUMINANCE, MT_GFX_UNSIGNED_BYTE, 1, luminance_unsigned_byte},
    {MT_GFX_LUMINANCE_ALPHA, MT_GFX_UNSIGNED_BYTE, 2, luminance_alpha_unsigned_byte},
    {MT_GFX_RGB, MT_GFX_UNSIGNED_SHORT_5_6_5, 2, rgb_unsigned_short_5_6_5},
    {MT_GFX_RGBA, MT_GFX_UNSIGNED_SHORT_4_4_4_4, 2, rgba_unsigned_short_4_4_4_4},
    {MT_GFX_RGBA, MT_GFX_UNSIGNED_SHORT_5_5_5_1, 2, rgba_unsigned_short_5_5_5_1},
};

const Conversion *find_conversion(u32 format, u32 type) noexcept {
    for (const Conversion &c : k_conversions) {
        if (c.format == format && c.type == type) {
            return &c;
        }
    }
    return nullptr;
}

// GL ES's default GL_UNPACK_ALIGNMENT.
constexpr u32 k_unpack_alignment = 4;

// Source bytes per row, without and with GL_UNPACK_ALIGNMENT's padding.
std::size_t row_bytes(const Conversion &c, u32 width) {
    return static_cast<std::size_t>(width) * c.bytes_per_texel;
}
std::size_t row_stride(const Conversion &c, u32 width) {
    return (row_bytes(c, width) + k_unpack_alignment - 1) / k_unpack_alignment * k_unpack_alignment;
}

struct Textures {
    std::unordered_map<u32, Texture> map;
    u32 next = 1;
    u64 replacements = 0, rejected = 0;
};

// The replacement hooks (#153, ADR 0081): at most MT_GFX_MAX_REPLACE_HOOKS, by `order` (equal
// orders in the order they were added).
struct Hook {
    int order = 0;
    mt_gfx_replace_fn fn = nullptr;
    void *user = nullptr;
};

struct Hooks {
    Hook list[MT_GFX_MAX_REPLACE_HOOKS];
    u32 count = 0;
};

Hooks &hooks() {
    static Hooks h;
    return h;
}

// The texture filter override (mt_gfx_set_texture_filter): 0, or MT_GFX_NEAREST.
u32 g_filter_override = 0;

constexpr u64 k_fnv_offset = 0xCBF29CE484222325ull;
constexpr u64 k_fnv_prime = 0x00000100000001B3ull;

// mt_gfx_upload_hash.
u64 hash_upload(const Conversion &c, u32 width, u32 height, const u8 *src) {
    u64 h = k_fnv_offset;
    const std::size_t bytes = row_bytes(c, width), stride = row_stride(c, width);
    for (u32 y = 0; y < height; ++y) {
        const u8 *row = src + y * stride;
        for (std::size_t i = 0; i < bytes; ++i) {
            h = (h ^ row[i]) * k_fnv_prime;
        }
    }
    return h;
}

Textures &textures() {
    static Textures t;
    return t;
}

Texture *find_mutable(u32 handle) noexcept {
    if (handle == 0) {
        return nullptr;
    }
    auto &map = textures().map;
    const auto it = map.find(handle);
    return it == map.end() ? nullptr : &it->second;
}

bool valid_filter(u32 value, bool min) noexcept {
    switch (value) {
    case MT_GFX_NEAREST:
    case MT_GFX_LINEAR:
        return true;
    case MT_GFX_NEAREST_MIPMAP_NEAREST:
    case MT_GFX_LINEAR_MIPMAP_NEAREST:
    case MT_GFX_NEAREST_MIPMAP_LINEAR:
    case MT_GFX_LINEAR_MIPMAP_LINEAR:
        return min;
    default:
        return false;
    }
}

} // namespace

const Texture *find_texture(u32 handle) noexcept { return find_mutable(handle); }

void texture_stats(mt_gfx_stats *stats) noexcept {
    u64 total = 0;
    u32 replaced = 0;
    for (const auto &[handle, texture] : textures().map) {
        total += texture.texels.size();
        replaced += texture.replaced ? 1u : 0u;
    }
    stats->textures = static_cast<u32>(textures().map.size());
    stats->texture_bytes = total;
    stats->replaced_textures = replaced;
    stats->replacements = textures().replacements;
    stats->rejected_replacements = textures().rejected;
}

void texture_reset() noexcept {
    for (const auto &[handle, texture] : textures().map) {
        notify_texture_changing(handle, true);
    }
    textures() = Textures{};
}

u32 filter_override() noexcept { return g_filter_override; }

u32 base_filter(u32 filter) noexcept {
    switch (filter) {
    case MT_GFX_LINEAR:
    case MT_GFX_LINEAR_MIPMAP_NEAREST:
    case MT_GFX_LINEAR_MIPMAP_LINEAR:
        return MT_GFX_LINEAR;
    default:
        return MT_GFX_NEAREST;
    }
}

} // namespace mt_gfx

using namespace mt_gfx;

extern "C" uint32_t mt_gfx_texture_create(void) noexcept {
    Textures &t = textures();
    try {
        while (t.next == 0 || t.map.count(t.next) != 0) {
            ++t.next; // wrapped around after 2^32 creates: skip 0 and live handles
        }
        const u32 handle = t.next++;
        t.map.emplace(handle, Texture{});
        return handle;
    } catch (const std::bad_alloc &) {
        return 0;
    }
}

extern "C" void mt_gfx_texture_destroy(uint32_t texture) noexcept {
    if (texture != 0 && textures().map.count(texture) != 0) {
        notify_texture_changing(texture, true);
        textures().map.erase(texture);
    }
}

extern "C" int mt_gfx_texture_image(uint32_t texture, uint32_t width, uint32_t height,
                                    uint32_t format, uint32_t type, const void *pixels) noexcept {
    Texture *tex = find_mutable(texture);
    const Conversion *conv = find_conversion(format, type);
    if (tex == nullptr || conv == nullptr || width > MT_GFX_MAX_SIZE || height > MT_GFX_MAX_SIZE) {
        return -1;
    }
    if (width == 0 || height == 0) {
        notify_texture_changing(texture, false);
        ++tex->generation;
        tex->logical_width = tex->logical_height = tex->width = tex->height = 0;
        tex->replaced = false;
        tex->texels.clear();
        tex->texels.shrink_to_fit();
        return 0;
    }
    const u8 *src = static_cast<const u8 *>(pixels);

    // The hooks may replace an upload with pixels: the first valid replacement counts.
    mt_gfx_replacement repl{};
    bool replace = false;
    if (src != nullptr && hooks().count != 0) {
        mt_gfx_upload up{};
        up.texture = texture;
        up.width = width;
        up.height = height;
        up.format = format;
        up.type = type;
        up.hash = hash_upload(*conv, width, height, src);
        up.pixels = pixels;
        // A copy: a hook must not change the chain, but a copy keeps this loop safe if it does.
        const Hooks chain = hooks();
        for (u32 i = 0; i < chain.count && !replace; ++i) {
            repl = mt_gfx_replacement{};
            if (chain.list[i].fn(chain.list[i].user, &up, &repl) == 0) {
                continue;
            }
            const u32 k = repl.width / width;
            replace = repl.pixels != nullptr && k >= 1 && k <= MT_GFX_MAX_SCALE &&
                      repl.width == k * width && repl.height == k * height &&
                      repl.width <= MT_GFX_MAX_SIZE && repl.height <= MT_GFX_MAX_SIZE;
            if (!replace) {
                ++textures().rejected;
                char line[200];
                std::snprintf(line, sizeof line,
                              "gfx: texture %u: replacement %ux%u for %ux%u (hash %016llx) "
                              "rejected: not k times the size, k 1..%u",
                              texture, repl.width, repl.height, width, height,
                              static_cast<unsigned long long>(up.hash), MT_GFX_MAX_SCALE);
                mt_diag_log(line);
            }
        }
    }

    const u32 tw = replace ? repl.width : width, th = replace ? repl.height : height;
    std::vector<u8> texels;
    try {
        texels.assign(static_cast<std::size_t>(tw) * th * 4u, 0);
    } catch (const std::bad_alloc &) {
        return -1;
    }
    if (replace) {
        std::memcpy(texels.data(), repl.pixels, texels.size());
        ++textures().replacements;
    } else if (src != nullptr) {
        const std::size_t stride = row_stride(*conv, width);
        for (u32 y = 0; y < height; ++y) {
            conv->row(src + y * stride, texels.data() + static_cast<std::size_t>(y) * width * 4u,
                      width);
        }
    }
    notify_texture_changing(texture, false);
    ++tex->generation;
    tex->logical_width = width;
    tex->logical_height = height;
    tex->width = tw;
    tex->height = th;
    tex->replaced = replace;
    tex->texels = std::move(texels);
    return 0;
}

extern "C" int mt_gfx_texture_parameter(uint32_t texture, uint32_t pname, uint32_t value) noexcept {
    Texture *tex = find_mutable(texture);
    if (tex == nullptr) {
        return -1;
    }
    switch (pname) {
    case MT_GFX_TEXTURE_MIN_FILTER:
    case MT_GFX_TEXTURE_MAG_FILTER: {
        const bool min = pname == MT_GFX_TEXTURE_MIN_FILTER;
        if (!valid_filter(value, min)) {
            return -1;
        }
        (min ? tex->min_filter : tex->mag_filter) = value;
        return 0;
    }
    case MT_GFX_TEXTURE_WRAP_S:
    case MT_GFX_TEXTURE_WRAP_T:
        if (value != MT_GFX_REPEAT && value != MT_GFX_CLAMP_TO_EDGE) {
            return -1;
        }
        (pname == MT_GFX_TEXTURE_WRAP_S ? tex->wrap_s : tex->wrap_t) = value;
        return 0;
    default:
        return -1;
    }
}

extern "C" int mt_gfx_texture_size(uint32_t texture, uint32_t *width, uint32_t *height) noexcept {
    const Texture *tex = find_texture(texture);
    if (width != nullptr) {
        *width = tex != nullptr ? tex->logical_width : 0;
    }
    if (height != nullptr) {
        *height = tex != nullptr ? tex->logical_height : 0;
    }
    return tex != nullptr ? 0 : -1;
}

extern "C" uint64_t mt_gfx_upload_hash(uint32_t width, uint32_t height, uint32_t format,
                                       uint32_t type, const void *pixels) noexcept {
    const Conversion *conv = find_conversion(format, type);
    if (conv == nullptr || pixels == nullptr) {
        return 0;
    }
    return hash_upload(*conv, width, height, static_cast<const u8 *>(pixels));
}

extern "C" void mt_gfx_set_replace_hook(mt_gfx_replace_fn fn, void *user) noexcept {
    hooks() = Hooks{};
    if (fn != nullptr) {
        mt_gfx_add_replace_hook(0, fn, user);
    }
}

extern "C" int mt_gfx_add_replace_hook(int order, mt_gfx_replace_fn fn, void *user) noexcept {
    if (fn == nullptr) {
        return -1;
    }
    mt_gfx_remove_replace_hook(fn, user);
    Hooks &h = hooks();
    if (h.count == MT_GFX_MAX_REPLACE_HOOKS) {
        return -1;
    }
    u32 at = h.count;
    while (at > 0 && h.list[at - 1].order > order) {
        h.list[at] = h.list[at - 1];
        --at;
    }
    h.list[at] = Hook{order, fn, user};
    ++h.count;
    return 0;
}

extern "C" void mt_gfx_remove_replace_hook(mt_gfx_replace_fn fn, void *user) noexcept {
    Hooks &h = hooks();
    u32 out = 0;
    for (u32 i = 0; i < h.count; ++i) {
        if (h.list[i].fn != fn || h.list[i].user != user) {
            h.list[out++] = h.list[i];
        }
    }
    for (u32 i = out; i < h.count; ++i) {
        h.list[i] = Hook{};
    }
    h.count = out;
}

extern "C" uint32_t mt_gfx_replace_hook_count(void) noexcept { return hooks().count; }

extern "C" int mt_gfx_upload_rgba8(uint32_t width, uint32_t height, uint32_t format, uint32_t type,
                                   const void *pixels, uint8_t *out) noexcept {
    const Conversion *conv = find_conversion(format, type);
    if (conv == nullptr || pixels == nullptr || out == nullptr || width > MT_GFX_MAX_SIZE ||
        height > MT_GFX_MAX_SIZE) {
        return -1;
    }
    const auto *src = static_cast<const u8 *>(pixels);
    const std::size_t stride = row_stride(*conv, width);
    for (u32 y = 0; y < height; ++y) {
        conv->row(src + y * stride, out + static_cast<std::size_t>(y) * width * 4u, width);
    }
    return 0;
}

extern "C" int mt_gfx_set_texture_filter(uint32_t filter) noexcept {
    if (filter != 0 && filter != MT_GFX_NEAREST) {
        return -1;
    }
    g_filter_override = filter;
    return 0;
}

extern "C" uint32_t mt_gfx_texture_filter(void) noexcept { return g_filter_override; }

extern "C" const uint8_t *mt_gfx_texture_texels(uint32_t texture, uint32_t *width,
                                                uint32_t *height) noexcept {
    const Texture *tex = find_texture(texture);
    const bool has = tex != nullptr && !tex->texels.empty();
    if (width != nullptr) {
        *width = has ? tex->width : 0;
    }
    if (height != nullptr) {
        *height = has ? tex->height : 0;
    }
    return has ? tex->texels.data() : nullptr;
}
