// The border's image (platform/src/border.cpp, #157): a PNG the player names in the settings.
// decode_border_image within its limits; what it decodes is width x height RGBA.
#include <cstddef>
#include <cstdint>
#include <string>

#include "border.h"

namespace mf = maplethief::frontend;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    mf::BorderImage image;
    std::string why;
    if (mf::decode_border_image(data, size, &image, &why) &&
        image.rgba.size() != static_cast<std::size_t>(image.width) * image.height * 4u) {
        __builtin_trap();
    }
    return 0;
}
