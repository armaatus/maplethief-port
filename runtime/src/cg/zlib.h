// zlib streams (RFC 1950: deflate with a zlib header) through the runtime's private copy of the
// pinned stb_image and stb_image_write (ADR 0003; implemented in codec.cpp, which holds that
// copy): the save states' sections (#202, ADR 0105). No new dependency.
#ifndef MAPLETHIEF_RUNTIME_CG_ZLIB_H
#define MAPLETHIEF_RUNTIME_CG_ZLIB_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mt_cg {

// `data` deflated into `out`; false if it is too large (2 GiB) or out of memory.
bool zlib_deflate(const std::uint8_t *data, std::size_t size, std::vector<std::uint8_t> &out);
// A zlib stream inflated into `out`, which must come to exactly `expected` bytes; false for a
// malformed stream, a longer or shorter result (the input is untrusted).
bool zlib_inflate(const std::uint8_t *data, std::size_t size, std::size_t expected,
                  std::vector<std::uint8_t> &out);

} // namespace mt_cg

#endif // MAPLETHIEF_RUNTIME_CG_ZLIB_H
