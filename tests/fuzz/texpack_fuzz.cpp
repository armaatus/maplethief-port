// A texture pack's PNG (runtime/src/texpack/texpack.cpp, #153, ADR 0081): a file from a
// downloaded pack, as the game's upload reaches it. The first byte picks the upload (1 to 4
// texels each way, the .png or .raw.png name); the rest is the pack's file for it, written to a
// scratch pack, indexed by mt_texpack_open and loaded by the upload. A replacement is k times
// the upload's size.
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "maplethief/diag.h"
#include "maplethief/gfx.h"
#include "maplethief/texpack.h"

namespace {

namespace fs = std::filesystem;

const fs::path &scratch() {
    static const fs::path dir = [] {
        mt_diag_config cfg = mt_diag_config_default();
        cfg.log = [](void *, const char *) {}; // a rejected file is logged: not per input
        mt_diag_configure(&cfg);
        fs::path d =
            fs::temp_directory_path() / ("maplethief-fuzz-texpack-" + std::to_string(::getpid()));
        std::error_code ec;
        fs::remove_all(d, ec);
        fs::create_directories(d);
        return d;
    }();
    return dir;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    const std::uint32_t w = 1 + (data[0] & 3u), h = 1 + ((data[0] >> 2) & 3u);
    const bool raw = (data[0] & 16u) != 0;
    std::vector<std::uint8_t> texels(static_cast<std::size_t>(w) * h * 4u);
    for (std::size_t i = 0; i < texels.size(); ++i) {
        texels[i] = static_cast<std::uint8_t>(i * 37u);
    }
    char name[MT_TEXPACK_NAME_MAX];
    mt_texpack_file_name(mt_gfx_upload_hash(w, h, MT_GFX_RGBA, MT_GFX_UNSIGNED_BYTE, texels.data()),
                         w, h, raw ? 1 : 0, name);

    const fs::path &dir = scratch();
    std::error_code ec;
    for (const fs::directory_entry &e : fs::directory_iterator(dir, ec)) {
        fs::remove_all(e.path(), ec);
    }
    std::ofstream(dir / name, std::ios::binary)
        .write(reinterpret_cast<const char *>(data + 1), static_cast<std::streamsize>(size - 1));

    mt_gfx_reset();
    if (mt_texpack_open(dir.string().c_str()) != 1) {
        __builtin_trap();
    }
    const std::uint32_t tex = mt_gfx_texture_create();
    if (mt_gfx_texture_image(tex, w, h, MT_GFX_RGBA, MT_GFX_UNSIGNED_BYTE, texels.data()) != 0) {
        __builtin_trap();
    }
    std::uint32_t tw = 0, th = 0;
    const std::uint8_t *p = mt_gfx_texture_texels(tex, &tw, &th);
    const mt_texpack_stats stats = mt_texpack_get_stats();
    if (p == nullptr || tw % w != 0 || th % h != 0 || tw / w != th / h ||
        (stats.replaced == 0 && (tw != w || th != h))) {
        __builtin_trap();
    }
    mt_gfx_texture_destroy(tex);
    mt_texpack_close();
    return 0;
}
