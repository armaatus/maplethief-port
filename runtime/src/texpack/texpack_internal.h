// Texture packs (maplethief/texpack.h): what the tests reach besides the C API.
#ifndef MAPLETHIEF_RUNTIME_TEXPACK_TEXPACK_INTERNAL_H
#define MAPLETHIEF_RUNTIME_TEXPACK_TEXPACK_INTERNAL_H

#include <cstdint>

namespace mt_texpack {

// The files the next mt_texpack_open indexes at most: MT_TEXPACK_MAX_FILES, unless a test sets
// fewer (so it need not make 65,537 files). Returns the cap it replaces.
std::uint32_t set_max_files(std::uint32_t n);

} // namespace mt_texpack

#endif // MAPLETHIEF_RUNTIME_TEXPACK_TEXPACK_INTERNAL_H
