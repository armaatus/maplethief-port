// PNG through the pinned stb_image (runtime/src/cg/codec.cpp, #51, #324): a texture pack's or
// a border's image, and CgBI (Apple's PNG variant). png_info reads the header, png_decode the
// whole image within its size limits; what it decodes is width x height RGBA.
#include <cstddef>
#include <cstdint>
#include <string>

#include "cg/cg_objects.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    std::uint32_t w = 0, h = 0;
    const bool info = mt_cg::png_info(data, size, &w, &h);
    mt_cg::Decoded d;
    std::string error;
    if (mt_cg::png_decode(data, size, &d, &error)) {
        if (!d.pixels || d.pixels->size() != static_cast<std::size_t>(d.width) * d.height * 4u ||
            d.width > mt_cg::k_png_max_side || d.height > mt_cg::k_png_max_side ||
            (info && (d.width != w || d.height != h))) {
            __builtin_trap();
        }
    }
    return 0;
}
