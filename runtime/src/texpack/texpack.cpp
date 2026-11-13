// Texture packs and texture dumps (#153, ADR 0081, maplethief/texpack.h).
//
// The pack's files are untrusted data: their names are matched against a fixed pattern when the
// pack is indexed (at most MT_TEXPACK_MAX_FILES files; a symbolic link is skipped, #413), each
// file is read (at most k_max_file bytes) when its upload comes, its PNG signature and header
// size are checked against the upload before stb_image (PNG only) decodes it, and the decoded
// size is checked again. A rejected file is logged once and never read
// again; the game's texture is kept.
#include "maplethief/texpack.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#if defined(_WIN32)
#include <share.h>
#endif
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <new>
#include <set>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cg/cg_objects.h"
#include "diag/diag_internal.h"
#include "maplethief/saves.h"
#include "texpack/texpack_internal.h"

namespace {

namespace fs = std::filesystem;
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// A pack file larger than this is rejected unread (a 4096 x 4096 RGBA PNG stored without
// compression is 64 MiB and a little more).
constexpr std::uintmax_t k_max_file = 80u << 20;

fs::path host_path(const std::string &utf8) {
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string utf8_of(const fs::path &p) {
    const std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

void logf(const char *format, ...) {
    char line[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof line, format, args);
    va_end(args);
    mt_diag_log(line);
}

// A GL file name: "<16 hex>_<W>x<H>.png" or ".raw.png" (hex in either case).
struct Key {
    u64 hash = 0;
    u32 width = 0, height = 0;
    bool operator==(const Key &) const = default;
};
struct KeyHash {
    std::size_t operator()(const Key &k) const noexcept {
        return static_cast<std::size_t>(k.hash ^ (static_cast<u64>(k.width) << 32 | k.height));
    }
};

bool parse_dimension(const std::string &s, std::size_t *at, char end, u32 *out) {
    u64 v = 0;
    std::size_t i = *at;
    const std::size_t start = i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9' && i - start < 5) {
        v = v * 10 + static_cast<u64>(s[i] - '0');
        ++i;
    }
    if (i == start || i >= s.size() || s[i] != end || v == 0 || v > MT_GFX_MAX_SIZE ||
        (s[start] == '0')) {
        return false;
    }
    *out = static_cast<u32>(v);
    *at = i + 1;
    return true;
}

bool parse_name(const std::string &name, Key *key, bool *raw) {
    if (name.size() < 16 + 1 + 3 + 4 || name[16] != '_') {
        return false;
    }
    u64 h = 0;
    for (std::size_t i = 0; i < 16; ++i) {
        const char c = name[i];
        const int d = c >= '0' && c <= '9'   ? c - '0'
                      : c >= 'a' && c <= 'f' ? c - 'a' + 10
                      : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                             : -1;
        if (d < 0) {
            return false;
        }
        h = h << 4 | static_cast<u64>(d);
    }
    std::size_t at = 17;
    Key k;
    k.hash = h;
    if (!parse_dimension(name, &at, 'x', &k.width) || !parse_dimension(name, &at, '.', &k.height)) {
        return false;
    }
    const std::string rest = name.substr(at);
    if (rest == "png") {
        *raw = false;
    } else if (rest == "raw.png") {
        *raw = true;
    } else {
        return false;
    }
    *key = k;
    return true;
}

// A bundle path the dump may write below ui/ and a pack may name: '/'-separated components of
// letters, digits and " _-+.@", none empty, "." or "..", ending in .png.
bool safe_rel(const std::string &rel) {
    if (rel.empty() || rel.size() > 255 || rel.size() < 5 ||
        rel.compare(rel.size() - 4, 4, ".png") != 0) {
        return false;
    }
    std::size_t start = 0;
    while (start <= rel.size()) {
        std::size_t end = rel.find('/', start);
        if (end == std::string::npos) {
            end = rel.size();
        }
        const std::string part = rel.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") {
            return false;
        }
        for (const char c : part) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == ' ' || c == '_' || c == '-' ||
                            c == '+' || c == '.' || c == '@';
            if (!ok) {
                return false;
            }
        }
        start = end + 1;
    }
    return true;
}

bool read_file(const fs::path &path, std::vector<u8> &out, std::string *why) {
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec) {
        *why = "cannot be read";
        return false;
    }
    if (size > k_max_file) {
        *why = "is larger than " + std::to_string(k_max_file >> 20) + " MiB";
        return false;
    }
    std::ifstream f(path, std::ios::binary);
    out.resize(static_cast<std::size_t>(size));
    if (!f || (size != 0 &&
               !f.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(size)))) {
        *why = "cannot be read";
        return false;
    }
    return true;
}

// round(c * a / 255) for each colour: the game's loader (ADR 0041).
void premultiply(std::vector<u8> &rgba) {
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        const u32 a = rgba[i + 3];
        for (std::size_t c = 0; c < 3; ++c) {
            rgba[i + c] = static_cast<u8>((rgba[i + c] * a + 127u) / 255u);
        }
    }
}

// Whether every texel's colours are at most its alpha (what premultiplied texels are).
bool is_premultiplied(const u8 *rgba, std::size_t n) {
    for (std::size_t i = 0; i < n; i += 4) {
        const u8 a = rgba[i + 3];
        if (rgba[i] > a || rgba[i + 1] > a || rgba[i + 2] > a) {
            return false;
        }
    }
    return true;
}

// round(c * 255 / a) for each colour (0 where a is 0): the inverse of premultiply for
// premultiplied texels.
void unpremultiply(u8 *rgba, std::size_t n) {
    for (std::size_t i = 0; i < n; i += 4) {
        const u32 a = rgba[i + 3];
        for (std::size_t c = 0; c < 3; ++c) {
            rgba[i + c] = a == 0 ? 0 : static_cast<u8>((rgba[i + c] * 510u + a) / (2u * a));
        }
    }
}

struct File {
    std::string path; // UTF-8
    bool raw = false;
    bool bad = false; // rejected once: never read again
};

struct Pack {
    bool open = false;
    std::string dir;
    std::unordered_map<Key, File, KeyHash> files;
    std::map<std::string, File> ui; // by bundle path
    std::vector<u8> pixels;         // the replacement handed out last
};

struct Dump {
    bool open = false;
    fs::path dir;
    std::unordered_set<Key, KeyHash> seen;
    std::set<std::string> seen_ui;
    std::FILE *manifest = nullptr;
    std::vector<u8> rgba;
};

Pack g_pack;
Dump g_dump;
mt_texpack_stats g_stats{};
u32 g_max_files = MT_TEXPACK_MAX_FILES; // the files a pack indexes (lower in a test)

// Reads and checks a pack file for an image of want_w x want_h (the upload's or the UIKit
// image's size): a PNG k times that size. Decoded into g_pack.pixels (straight or as stored),
// its factor into *k_out.
bool load(File &file, u32 want_w, u32 want_h, const char *what, u32 *k_out) {
    if (file.bad) {
        return false;
    }
    std::vector<u8> bytes;
    std::string why;
    u32 w = 0, h = 0;
    bool ok = read_file(host_path(file.path), bytes, &why);
    if (ok && !mt_cg::png_info(bytes.data(), bytes.size(), &w, &h)) {
        why = "is no PNG (or is cut before its image data)";
        ok = false;
    }
    const u32 k = ok ? w / want_w : 0u;
    if (ok && (k < 1 || k > MT_GFX_MAX_SCALE || w != k * want_w || h != k * want_h ||
               w > MT_GFX_MAX_SIZE || h > MT_GFX_MAX_SIZE)) {
        why = std::to_string(w) + "x" + std::to_string(h) + " is not k times " +
              std::to_string(want_w) + "x" + std::to_string(want_h) + " (k 1.." +
              std::to_string(MT_GFX_MAX_SCALE) + ", at most " + std::to_string(MT_GFX_MAX_SIZE) +
              ")";
        ok = false;
    }
    mt_cg::Decoded d;
    std::string error;
    if (ok &&
        (!mt_cg::png_decode(bytes.data(), bytes.size(), &d, &error) || d.width != w ||
         d.height != h || !d.pixels || d.pixels->size() != static_cast<std::size_t>(w) * h * 4u)) {
        why = "does not decode (" + error + ")";
        ok = false;
    }
    if (!ok) {
        file.bad = true;
        logf("texpack: %s %s %s: rejected, the game's %s kept", what, file.path.c_str(),
             why.c_str(), what);
        return false;
    }
    g_pack.pixels.assign(d.pixels->begin(), d.pixels->end());
    *k_out = k;
    return true;
}

// The pack's mt_gfx hook.
int pack_replace(void *, const mt_gfx_upload *up, mt_gfx_replacement *out) noexcept {
    try {
        ++g_stats.uploads;
        const auto it = g_pack.files.find(Key{up->hash, up->width, up->height});
        if (it == g_pack.files.end()) {
            ++g_stats.missing;
            return 0;
        }
        u32 k = 0;
        if (!load(it->second, up->width, up->height, "texture", &k)) {
            ++g_stats.rejected;
            return 0;
        }
        if (!it->second.raw) {
            premultiply(g_pack.pixels);
        }
        out->pixels = g_pack.pixels.data();
        out->width = k * up->width;
        out->height = k * up->height;
        ++g_stats.replaced;
        return 1;
    } catch (const std::bad_alloc &) {
        ++g_stats.rejected;
        return 0;
    }
}

std::string json_hex(u32 v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%04X", static_cast<unsigned>(v));
    return buf;
}

bool write_dump_file(const fs::path &path, const u8 *rgba, u32 w, u32 h) {
    std::vector<u8> png;
    if (!mt_cg::png_encode(rgba, w, h, 4, png)) {
        return false;
    }
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    return mt_saves_replace_file(utf8_of(path).c_str(), png.data(), png.size()) == 0;
}

void manifest_line(const std::string &line) {
    if (g_dump.manifest != nullptr) {
        std::fputs(line.c_str(), g_dump.manifest);
        std::fputc('\n', g_dump.manifest);
        std::fflush(g_dump.manifest);
    }
}

// The dump's mt_gfx hook: writes the upload once, never replaces it.
int dump_observe(void *, const mt_gfx_upload *up, mt_gfx_replacement *) noexcept {
    try {
        const Key key{up->hash, up->width, up->height};
        if (!g_dump.seen.insert(key).second) {
            return 0;
        }
        const std::size_t n = static_cast<std::size_t>(up->width) * up->height * 4u;
        g_dump.rgba.resize(n);
        if (mt_gfx_upload_rgba8(up->width, up->height, up->format, up->type, up->pixels,
                                g_dump.rgba.data()) != 0) {
            return 0;
        }
        const bool raw = !is_premultiplied(g_dump.rgba.data(), n);
        if (!raw) {
            unpremultiply(g_dump.rgba.data(), n);
        }
        char name[MT_TEXPACK_NAME_MAX];
        mt_texpack_file_name(up->hash, up->width, up->height, raw ? 1 : 0, name);
        if (!write_dump_file(g_dump.dir / name, g_dump.rgba.data(), up->width, up->height)) {
            ++g_stats.dump_errors;
            logf("texpack: the dump cannot write %s", name);
            return 0;
        }
        char hash[17];
        std::snprintf(hash, sizeof hash, "%016llx", static_cast<unsigned long long>(up->hash));
        manifest_line(std::string("{\"hash\":\"") + hash + "\",\"file\":\"" + name +
                      "\",\"width\":" + std::to_string(up->width) +
                      ",\"height\":" + std::to_string(up->height) + ",\"format\":\"" +
                      json_hex(up->format) + "\",\"type\":\"" + json_hex(up->type) +
                      "\",\"alpha\":\"" + (raw ? "raw" : "straight") + "\"}");
        ++g_stats.dumped;
    } catch (const std::bad_alloc &) {
        ++g_stats.dump_errors;
    }
    return 0;
}

// The UIKit image hook: the dump (once per file), then the pack's ui/ file.
int image_hook(void *, const mt_cg_image_file *image, mt_cg_image_replacement *out) noexcept {
    try {
        const std::string rel = image->file != nullptr ? image->file : "";
        if (!safe_rel(rel)) {
            return 0;
        }
        if (g_dump.open && g_dump.seen_ui.insert(rel).second) {
            const fs::path path = g_dump.dir / "ui" / host_path(rel);
            if (write_dump_file(path, image->rgba, image->width, image->height)) {
                manifest_line("{\"ui\":\"" + rel + "\",\"file\":\"ui/" + rel +
                              "\",\"width\":" + std::to_string(image->width) + ",\"height\":" +
                              std::to_string(image->height) + ",\"alpha\":\"straight\"}");
                ++g_stats.dumped_ui;
            } else {
                ++g_stats.dump_errors;
                logf("texpack: the dump cannot write ui/%s", rel.c_str());
            }
        }
        if (!g_pack.open) {
            return 0;
        }
        ++g_stats.ui_images;
        const auto it = g_pack.ui.find(rel);
        if (it == g_pack.ui.end()) {
            return 0;
        }
        u32 k = 0;
        if (!load(it->second, image->width, image->height, "image", &k)) {
            ++g_stats.ui_rejected;
            return 0;
        }
        out->rgba = g_pack.pixels.data();
        out->width = k * image->width;
        out->height = k * image->height;
        ++g_stats.ui_replaced;
        return 1;
    } catch (const std::bad_alloc &) {
        return 0;
    }
}

// The symbolic links an index skipped (#413): a pack's files are its own, never a link's target.
struct Links {
    u64 count = 0;
    std::string first; // UTF-8, for the log
    void add(const fs::path &p) {
        if (count++ == 0) {
            first = utf8_of(p);
        }
    }
};

// Whether a pack's directory entry is a regular file itself, not a link to one (a link is
// counted in *links).
bool regular_file(const fs::directory_entry &entry, Links *links) {
    std::error_code ec;
    const fs::file_status st = entry.symlink_status(ec);
    if (fs::is_symlink(st)) {
        links->add(entry.path());
        return false;
    }
    return !ec && fs::is_regular_file(st);
}

void update_image_hook() {
    mt_cg_set_image_replace_hook(g_pack.open || g_dump.open ? image_hook : nullptr, nullptr);
}

void reset_pack_stats() {
    const mt_texpack_stats d = g_stats;
    g_stats = mt_texpack_stats{};
    g_stats.dumped = d.dumped;
    g_stats.dumped_ui = d.dumped_ui;
    g_stats.dump_errors = d.dump_errors;
}

} // namespace

std::uint32_t mt_texpack::set_max_files(std::uint32_t n) {
    const u32 old = g_max_files;
    g_max_files = n;
    return old;
}

extern "C" void mt_texpack_file_name(uint64_t hash, uint32_t width, uint32_t height, int raw,
                                     char *out) noexcept {
    std::snprintf(out, MT_TEXPACK_NAME_MAX, "%016llx_%ux%u.%s",
                  static_cast<unsigned long long>(hash), static_cast<unsigned>(width),
                  static_cast<unsigned>(height), raw != 0 ? "raw.png" : "png");
}

extern "C" int mt_texpack_open(const char *dir) noexcept {
    mt_texpack_close();
    try {
        const fs::path root = host_path(dir != nullptr ? dir : "");
        std::error_code ec;
        if (dir == nullptr || *dir == '\0' || !fs::is_directory(root, ec)) {
            logf("texpack: the pack %s is no directory: no pack", dir != nullptr ? dir : "");
            return -1;
        }
        reset_pack_stats();
        Pack pack;
        pack.dir = dir;
        u64 ignored = 0, skipped = 0;
        Links links;
        // Whether one more file may be indexed (at most g_max_files; the rest counted).
        const auto room = [&pack, &skipped]() {
            const bool ok = pack.files.size() + pack.ui.size() < g_max_files;
            skipped += ok ? 0u : 1u;
            return ok;
        };
        for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
            if (!regular_file(*it, &links)) {
                continue;
            }
            const std::string name = utf8_of(it->path().filename());
            Key key;
            bool raw = false;
            if (!parse_name(name, &key, &raw)) {
                ignored += name != "manifest.jsonl" ? 1u : 0u;
                continue;
            }
            const auto have = pack.files.find(key);
            if (have != pack.files.end()) {
                if (raw) { // <hash>.png and .raw.png: the .raw.png counts
                    have->second = File{utf8_of(it->path()), raw, false};
                }
            } else if (room()) {
                pack.files[key] = File{utf8_of(it->path()), raw, false};
            }
        }
        const fs::path ui = root / "ui";
        std::error_code ue;
        const fs::file_status ui_status = fs::symlink_status(ui, ue);
        if (fs::is_symlink(ui_status)) {
            links.add(ui);
        } else if (fs::is_directory(ui_status)) {
            // A link to a folder below ui/ is an entry like any other: never followed into.
            for (fs::recursive_directory_iterator it(ui, ec), end; !ec && it != end;
                 it.increment(ec)) {
                if (!regular_file(*it, &links)) {
                    continue;
                }
                const std::string rel = utf8_of(it->path().lexically_relative(ui));
                if (!safe_rel(rel)) {
                    ++ignored;
                    continue;
                }
                if (pack.ui.count(rel) != 0 || room()) {
                    pack.ui[rel] = File{utf8_of(it->path()), false, false};
                }
            }
        }
        if (links.count != 0) {
            logf("texpack: pack %s: %llu symbolic links not followed (the first: %s)", dir,
                 static_cast<unsigned long long>(links.count), links.first.c_str());
        }
        if (skipped != 0) {
            logf("texpack: pack %s: %llu files past the first %u skipped", dir,
                 static_cast<unsigned long long>(skipped), static_cast<unsigned>(g_max_files));
        }
        pack.open = true;
        g_pack = std::move(pack);
        g_stats.files = static_cast<u32>(g_pack.files.size());
        g_stats.ui_files = static_cast<u32>(g_pack.ui.size());
        mt_gfx_add_replace_hook(MT_GFX_REPLACE_ORDER_PACK, pack_replace, nullptr);
        update_image_hook();
        logf("texpack: pack %s: %u textures, %u UIKit images%s", dir,
             static_cast<unsigned>(g_stats.files), static_cast<unsigned>(g_stats.ui_files),
             ignored != 0 ? (", " + std::to_string(ignored) + " other files ignored").c_str() : "");
        return static_cast<int>(g_stats.files + g_stats.ui_files);
    } catch (const std::exception &) {
        g_pack = Pack{};
        logf("texpack: the pack %s cannot be indexed: no pack", dir);
        return -1;
    }
}

extern "C" void mt_texpack_close(void) noexcept {
    if (!g_pack.open) {
        return;
    }
    mt_gfx_remove_replace_hook(pack_replace, nullptr);
    g_pack = Pack{};
    update_image_hook();
}

extern "C" int mt_texpack_dump_open(const char *dir) noexcept {
    mt_texpack_dump_close();
    try {
        if (dir == nullptr || *dir == '\0') {
            return -1;
        }
        const fs::path root = host_path(dir);
        std::error_code ec;
        fs::create_directories(root, ec);
        if (!fs::is_directory(root, ec)) {
            logf("texpack: the dump directory %s cannot be made: no dump", dir);
            return -1;
        }
        Dump d;
        d.dir = root;
        for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
            Key key;
            bool raw = false;
            if (parse_name(utf8_of(it->path().filename()), &key, &raw)) {
                d.seen.insert(key);
            }
        }
        if (fs::is_directory(root / "ui", ec)) {
            for (fs::recursive_directory_iterator it(root / "ui", ec), end; !ec && it != end;
                 it.increment(ec)) {
                d.seen_ui.insert(utf8_of(it->path().lexically_relative(root / "ui")));
            }
        }
#if defined(_WIN32)
        // Shared, so the manifest can be read while the dump is open (_wfopen_s denies that).
        d.manifest = _wfsopen((root / "manifest.jsonl").c_str(), L"ab", _SH_DENYNO);
#else
        d.manifest = std::fopen((root / "manifest.jsonl").c_str(), "ab");
#endif
        if (d.manifest == nullptr) {
            logf("texpack: the dump cannot write %s/manifest.jsonl: no dump", dir);
            return -1;
        }
        const std::size_t before = d.seen.size();
        d.open = true;
        g_dump = std::move(d);
        g_stats.dumped = g_stats.dumped_ui = g_stats.dump_errors = 0;
        mt_gfx_add_replace_hook(MT_GFX_REPLACE_ORDER_DUMP, dump_observe, nullptr);
        update_image_hook();
        logf("texpack: dumping every texture to %s (%zu there already)", dir, before);
        return 0;
    } catch (const std::exception &) {
        g_dump = Dump{};
        return -1;
    }
}

extern "C" void mt_texpack_dump_close(void) noexcept {
    if (!g_dump.open) {
        return;
    }
    mt_gfx_remove_replace_hook(dump_observe, nullptr);
    if (g_dump.manifest != nullptr) {
        std::fclose(g_dump.manifest);
    }
    g_dump = Dump{};
    update_image_hook();
}

extern "C" mt_texpack_stats mt_texpack_get_stats(void) noexcept { return g_stats; }
