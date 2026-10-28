// PNG decoding and encoding for UIImage, CGImage and UIImagePNGRepresentation (#51, ADR 0036),
// with the pinned stb_image and stb_image_write (ADR 0003). stb_image converts Apple's CgBI
// PNGs (raw deflate, BGRA, premultiplied) itself; its output for all 113 shipped CgBI files is
// bit-identical to ImageIO's (ADR 0003).
//
// The decoders are private copies (static linkage), so the runtime and the golden-image test
// helper (tests/golden/) can be linked into one program. Inputs are the files of a hash-checked
// IPA and images the game makes itself; they are parsed, never executed.
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "cg/cg_objects.h"

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace mt_cg {

bool png_decode(const u8 *data, std::size_t size, Decoded *out, std::string *error) {
    if (size == 0 || size > 0x7FFFFFFFu) {
        if (error != nullptr) {
            *error = "empty or oversized data";
        }
        return false;
    }
    // Per call and per thread: CgBI files become straight RGBA (GCD workers may decode too).
    stbi_convert_iphone_png_to_rgb_thread(1);
    stbi_set_unpremultiply_on_load_thread(1);
    int w = 0, h = 0, comp = 0;
    stbi_uc *pixels = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &comp, 4);
    if (pixels == nullptr) {
        if (error != nullptr) {
            *error = stbi_failure_reason();
        }
        return false;
    }
    const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u;
    auto bytes = std::make_shared<std::vector<u8>>(pixels, pixels + n);
    stbi_image_free(pixels);
    out->width = static_cast<u32>(w);
    out->height = static_cast<u32>(h);
    // comp: channels in the file (palette with tRNS and CgBI count 4).
    out->has_alpha = comp == 2 || comp == 4;
    out->pixels = std::move(bytes);
    return true;
}

namespace {

void append(void *context, void *data, int size) {
    auto *out = static_cast<std::vector<u8> *>(context);
    const auto *p = static_cast<const u8 *>(data);
    out->insert(out->end(), p, p + size);
}

} // namespace

bool png_encode(const u8 *pixels, u32 width, u32 height, u32 channels, std::vector<u8> &out) {
    out.clear();
    if (width == 0 || height == 0 || (channels != 3 && channels != 4)) {
        return false;
    }
    return stbi_write_png_to_func(append, &out, static_cast<int>(width), static_cast<int>(height),
                                  static_cast<int>(channels), pixels,
                                  static_cast<int>(width * channels)) != 0;
}

u32 image_from_png(mt_cpu *cpu, const u8 *data, std::size_t size, std::string *error) {
    Decoded d;
    if (!png_decode(data, size, &d, error)) {
        return 0;
    }
    return image_create_host(cpu, d.width, d.height, 32,
                             d.has_alpha ? k_alpha_last : k_alpha_none_skip_last, d.pixels);
}

} // namespace mt_cg
