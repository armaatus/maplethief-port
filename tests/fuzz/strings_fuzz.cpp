// A text pack's .strings file (runtime/src/textpack/strings_file.cpp, #189): parse_strings,
// and what it accepts is strict UTF-8 within the limits.
#include <cstddef>
#include <cstdint>
#include <string>

#include "maplethief/textpack.h"
#include "textpack/strings_file.h"

namespace {

bool utf8(const std::string &s) {
    return mt_textpack::valid_utf8(reinterpret_cast<const std::uint8_t *>(s.data()), s.size());
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    mt_textpack::StringsFile file;
    std::string why;
    if (!mt_textpack::parse_strings(data, size, &file, &why)) {
        return 0;
    }
    if (file.entries.size() > MT_TEXTPACK_MAX_ENTRIES) {
        __builtin_trap();
    }
    for (const auto &[key, value] : file.entries) {
        if (key.empty() || key.size() > MT_TEXTPACK_MAX_KEY ||
            value.size() > MT_TEXTPACK_MAX_VALUE || !utf8(key) || !utf8(value) ||
            key.find('\0') != std::string::npos || value.find('\0') != std::string::npos) {
            __builtin_trap();
        }
    }
    return 0;
}
