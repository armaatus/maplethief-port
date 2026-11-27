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
#include "maplethief/packcheck.h"
#include "maplethief/saves.h"
#include "stubs/json.h"
#include "texpack/texpack_internal.h"
#include "textpack/packcheck_internal.h"

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
// image's size): a PNG k times that size. Decoded into *pixels (straight or as stored), its
// factor into *k_out; false with *why. The check (mt_texpack_check) asks the same.
bool read_checked(const std::string &path, u32 want_w, u32 want_h, std::vector<u8> *pixels,
                  u32 *k_out, std::string *why) {
    std::vector<u8> bytes;
    u32 w = 0, h = 0;
    bool ok = read_file(host_path(path), bytes, why);
    if (ok && !mt_cg::png_info(bytes.data(), bytes.size(), &w, &h)) {
        *why = "is no PNG (or is cut before its image data)";
        ok = false;
    }
    const u32 k = ok ? w / want_w : 0u;
    if (ok && (k < 1 || k > MT_GFX_MAX_SCALE || w != k * want_w || h != k * want_h ||
               w > MT_GFX_MAX_SIZE || h > MT_GFX_MAX_SIZE)) {
        *why = std::to_string(w) + "x" + std::to_string(h) + " is not k times " +
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
        *why = "does not decode (" + error + ")";
        ok = false;
    }
    if (!ok) {
        return false;
    }
    pixels->assign(d.pixels->begin(), d.pixels->end());
    *k_out = k;
    return true;
}

// read_checked for the pack open now, into g_pack.pixels; a rejected file is logged once and
// never read again.
bool load(File &file, u32 want_w, u32 want_h, const char *what, u32 *k_out) {
    if (file.bad) {
        return false;
    }
    std::string why;
    if (!read_checked(file.path, want_w, want_h, &g_pack.pixels, k_out, &why)) {
        file.bad = true;
        logf("texpack: %s %s %s: rejected, the game's %s kept", what, file.path.c_str(),
             why.c_str(), what);
        return false;
    }
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

// What indexing a pack passed over: names that are no pack file, files past the cap, links.
struct Index {
    u64 skipped = 0;
    Links links;
    std::vector<std::string> ignored; // UTF-8 names relative to the pack
};

// Indexes the pack in `root` into `pack` by the names it may have (at most g_max_files).
void index_pack(const fs::path &root, Pack &pack, Index &ix) {
    std::error_code ec;
    // Whether one more file may be indexed (at most g_max_files; the rest counted).
    const auto room = [&pack, &ix]() {
        const bool ok = pack.files.size() + pack.ui.size() < g_max_files;
        ix.skipped += ok ? 0u : 1u;
        return ok;
    };
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!regular_file(*it, &ix.links)) {
            continue;
        }
        const std::string name = utf8_of(it->path().filename());
        Key key;
        bool raw = false;
        if (!parse_name(name, &key, &raw)) {
            if (name != "manifest.jsonl" && name != "pack.strings") {
                ix.ignored.push_back(name);
            }
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
        ix.links.add(ui);
    } else if (fs::is_directory(ui_status)) {
        // A link to a folder below ui/ is an entry like any other: never followed into.
        for (fs::recursive_directory_iterator it(ui, ec), end; !ec && it != end; it.increment(ec)) {
            if (!regular_file(*it, &ix.links)) {
                continue;
            }
            const std::string rel = utf8_of(it->path().lexically_relative(ui));
            if (!safe_rel(rel)) {
                ix.ignored.push_back("ui/" + rel);
                continue;
            }
            if (pack.ui.count(rel) != 0 || room()) {
                pack.ui[rel] = File{utf8_of(it->path()), false, false};
            }
        }
    }
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
        Index ix;
        index_pack(root, pack, ix);
        const Links &links = ix.links;
        const u64 skipped = ix.skipped, ignored = ix.ignored.size();
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

extern "C" int mt_texpack_dump_is_open(void) noexcept { return g_dump.open ? 1 : 0; }

extern "C" mt_texpack_stats mt_texpack_get_stats(void) noexcept { return g_stats; }

extern "C" mt_packcheck_report mt_texpack_check(const char *dir, const char *bundle,
                                                mt_packcheck_fn fn, void *user) noexcept {
    mt_packcheck::Reporter r(fn, user, "texture");
    try {
        const std::string d = dir != nullptr ? dir : "";
        const fs::path root = host_path(d);
        std::error_code ec;
        if (d.empty() || !fs::is_directory(root, ec)) {
            r.refused(d + ": is no folder");
            return r.report;
        }
        std::map<std::string, std::string> manifest;
        mt_packcheck::check_manifest(root, "texture", r, &manifest);
        Pack pack;
        Index ix;
        index_pack(root, pack, ix);
        if (ix.links.count != 0) {
            r.ignored(std::to_string(ix.links.count) +
                      " symbolic links not followed (the first: " + ix.links.first + ")");
        }
        if (ix.skipped != 0) {
            r.ignored(std::to_string(ix.skipped) + " files past the first " +
                      std::to_string(g_max_files) + " skipped");
        }
        for (const std::string &name : ix.ignored) {
            r.ignored(name + ": not a texture's name (<hash>_<W>x<H>.png, .raw.png, or ui/ "
                             "with letters, digits and \" _-+.@\" ending .png): ignored");
        }
        std::vector<u8> pixels;
        std::string why;
        u32 k = 0;
        std::map<std::string, u32> factors; // k: how many files have it
        for (const auto &[key, file] : pack.files) {
            const std::string name = utf8_of(host_path(file.path).filename());
            if (read_checked(file.path, key.width, key.height, &pixels, &k, &why)) {
                ++r.report.good;
                ++factors[std::to_string(k) + "x"];
            } else {
                r.rejected(name + " " + why + ": rejected, the game's texture kept");
            }
        }
        const std::string b = bundle != nullptr ? bundle : "";
        u64 unsized = 0;
        for (const auto &[rel, file] : pack.ui) {
            // The game's image of that bundle path gives the size; without it any size passes.
            u32 w = 0, h = 0;
            bool sized = false;
            if (!b.empty()) {
                std::vector<u8> game;
                std::string ignore;
                const fs::path original = host_path(b) / host_path(rel);
                if (fs::is_regular_file(original, ec) && read_file(original, game, &ignore)) {
                    sized = mt_cg::png_info(game.data(), game.size(), &w, &h);
                    mt_cg::Decoded dd; // an Apple CgBI file, if stb_image's header read failed
                    if (!sized && mt_cg::png_decode(game.data(), game.size(), &dd, &ignore)) {
                        w = dd.width;
                        h = dd.height;
                        sized = true;
                    }
                }
                if (!sized) {
                    r.ignored("ui/" + rel + ": the game has no image of that name: ignored");
                    continue;
                }
            }
            if (!sized) {
                std::vector<u8> bytes;
                if (read_file(host_path(file.path), bytes, &why) &&
                    mt_cg::png_info(bytes.data(), bytes.size(), &w, &h)) {
                    ++unsized; // checked as its own size (k = 1): only the file itself
                } else {
                    w = h = 1; // read_checked says why
                }
            }
            if (read_checked(file.path, w, h, &pixels, &k, &why)) {
                ++r.report.good;
                ++factors[std::to_string(k) + "x"];
            } else {
                r.rejected("ui/" + rel + " " + why + ": rejected, the game's image kept");
            }
        }
        if (unsized != 0) {
            r.note(std::to_string(unsized) +
                   " ui/ images not compared with the game's (its folder not given): each must be "
                   "a whole multiple of the game's image");
        }
        std::string sizes;
        for (const auto &[f, n] : factors) {
            sizes += (sizes.empty() ? "" : ", ") + std::to_string(n) + " at " + f;
        }
        r.note(std::to_string(r.report.good) + " of " +
               std::to_string(pack.files.size() + pack.ui.size()) + " pictures usable" +
               (sizes.empty() ? "" : " (" + sizes + ")"));
        if (pack.files.empty() && pack.ui.empty()) {
            r.ignored("no texture files: the pack replaces nothing");
        }
    } catch (const std::exception &) {
        r.refused(std::string(dir != nullptr ? dir : "") + ": cannot be read");
    }
    return r.report;
}

// --- The pack builder (#371, ADR 0205): mtport.texpack build, in the game -------------------

namespace {

using Rows = std::vector<std::vector<u32>>; // pixels as 32-bit words: compared and copied only

Rows nearest_rows(const Rows &rows, u32 k) {
    Rows out;
    out.reserve(rows.size() * k);
    for (const auto &row : rows) {
        std::vector<u32> wide;
        wide.reserve(row.size() * k);
        for (const u32 p : row) {
            wide.insert(wide.end(), k, p);
        }
        for (u32 i = 0; i < k; ++i) {
            out.push_back(wide);
        }
    }
    return out;
}

// Scale2x (AdvMAME2x): E's four corners take D, F (left, right) where an edge meets; the
// border repeated.
Rows scale2x(const Rows &rows) {
    Rows out;
    out.reserve(rows.size() * 2);
    const std::size_t n = rows.size();
    for (std::size_t y = 0; y < n; ++y) {
        const auto &row = rows[y];
        const auto &up = y > 0 ? rows[y - 1] : row;
        const auto &down = y + 1 < n ? rows[y + 1] : row;
        const std::size_t w = row.size();
        std::vector<u32> top(w * 2), bottom(w * 2);
        for (std::size_t x = 0; x < w; ++x) {
            const u32 b = up[x], h = down[x], e = row[x];
            const u32 d = row[x > 0 ? x - 1 : 0], f = row[x + 1 < w ? x + 1 : x];
            const bool edge = b != h && d != f;
            top[2 * x] = edge && d == b ? d : e;
            top[2 * x + 1] = edge && b == f ? f : e;
            bottom[2 * x] = edge && d == h ? d : e;
            bottom[2 * x + 1] = edge && h == f ? f : e;
        }
        out.push_back(std::move(top));
        out.push_back(std::move(bottom));
    }
    return out;
}

// Scale3x (AdvMAME3x), with the diagonal neighbours A, C, G, I for the edge pixels.
Rows scale3x(const Rows &rows) {
    Rows out;
    out.reserve(rows.size() * 3);
    const std::size_t n = rows.size();
    for (std::size_t y = 0; y < n; ++y) {
        const auto &row = rows[y];
        const auto &up = y > 0 ? rows[y - 1] : row;
        const auto &down = y + 1 < n ? rows[y + 1] : row;
        const std::size_t w = row.size();
        std::vector<u32> r0(w * 3), r1(w * 3), r2(w * 3);
        for (std::size_t x = 0; x < w; ++x) {
            const std::size_t l = x > 0 ? x - 1 : 0, r = x + 1 < w ? x + 1 : x;
            const u32 a = up[l], b = up[x], c = up[r];
            const u32 d = row[l], e = row[x], f = row[r];
            const u32 g = down[l], h = down[x], i = down[r];
            u32 *o0 = &r0[3 * x], *o1 = &r1[3 * x], *o2 = &r2[3 * x];
            if (b != h && d != f) {
                o0[0] = d == b ? d : e;
                o0[1] = (d == b && e != c) || (b == f && e != a) ? b : e;
                o0[2] = b == f ? f : e;
                o1[0] = (d == b && e != g) || (d == h && e != a) ? d : e;
                o1[1] = e;
                o1[2] = (b == f && e != i) || (h == f && e != c) ? f : e;
                o2[0] = d == h ? d : e;
                o2[1] = (d == h && e != i) || (h == f && e != g) ? h : e;
                o2[2] = h == f ? f : e;
            } else {
                o0[0] = o0[1] = o0[2] = o1[0] = o1[1] = o1[2] = o2[0] = o2[1] = o2[2] = e;
            }
        }
        out.push_back(std::move(r0));
        out.push_back(std::move(r1));
        out.push_back(std::move(r2));
    }
    return out;
}

// The passes for factor k: 2 and 3 for the scaler (none for k = 1); -k means nearest.
std::vector<int> scale_steps(u32 k, int scaler) {
    if (k <= 1) {
        return {};
    }
    if (scaler == MT_TEXPACK_SCALER_SCALE) {
        switch (k) {
        case 2:
            return {2};
        case 3:
            return {3};
        case 4:
            return {2, 2};
        case 6:
            return {2, 3};
        case 8:
            return {2, 2, 2};
        default:
            break;
        }
    }
    return {-static_cast<int>(k)};
}

// One file of a dump: its path relative to the dump and its size there.
struct DumpEntry {
    std::string file; // "<hash>_<W>x<H>.png" or "ui/<bundle path>"
    u32 width = 0, height = 0;
};

// A manifest object the builder can trust: a file name of the dump's own shapes whose size
// agrees with the line's.
bool dump_entry_of(const std::string &line, DumpEntry *out) {
    using namespace mt_stubs_impl;
    JsonValue v{};
    const char *error = nullptr;
    if (!json_parse(line.data(), line.size(), &v, &error)) {
        return false;
    }
    const JsonValue *file = json_get(&v, "file"), *w = json_get(&v, "width"),
                    *h = json_get(&v, "height");
    bool ok = file != nullptr && file->type == JsonType::string && w != nullptr &&
              w->type == JsonType::number && w->is_integer && h != nullptr &&
              h->type == JsonType::number && h->is_integer && w->integer >= 1 &&
              w->integer <= MT_GFX_MAX_SIZE && h->integer >= 1 && h->integer <= MT_GFX_MAX_SIZE;
    if (ok) {
        out->file = file->string;
        out->width = static_cast<u32>(w->integer);
        out->height = static_cast<u32>(h->integer);
        Key key;
        bool raw = false;
        ok = out->file.rfind("ui/", 0) == 0
                 ? safe_rel(out->file.substr(3))
                 : parse_name(out->file, &key, &raw) && key.width == out->width &&
                       key.height == out->height;
    }
    json_free(&v);
    return ok;
}

// Upscales one file of the dump into the pack: "" or what went wrong.
std::string build_one(const fs::path &dump, const fs::path &pack, const DumpEntry &e, u32 k,
                      int scaler) {
    std::vector<u8> bytes;
    std::string why;
    if (!read_file(dump / host_path(e.file), bytes, &why)) {
        return e.file + ": " + why;
    }
    mt_cg::Decoded d;
    std::string error;
    if (!mt_cg::png_decode(bytes.data(), bytes.size(), &d, &error) || !d.pixels) {
        return e.file + ": does not decode (" + error + ")";
    }
    if (d.width != e.width || d.height != e.height) {
        return e.file + ": " + std::to_string(d.width) + "x" + std::to_string(d.height) +
               ", the manifest says " + std::to_string(e.width) + "x" + std::to_string(e.height);
    }
    if (e.width * k > MT_GFX_MAX_SIZE || e.height * k > MT_GFX_MAX_SIZE) {
        return e.file + ": " + std::to_string(e.width) + "x" + std::to_string(e.height) + " at " +
               std::to_string(k) + "x is above " + std::to_string(MT_GFX_MAX_SIZE) +
               " pixels a side";
    }
    Rows rows(e.height, std::vector<u32>(e.width));
    for (u32 y = 0; y < e.height; ++y) {
        std::memcpy(rows[y].data(), d.pixels->data() + static_cast<std::size_t>(y) * e.width * 4,
                    static_cast<std::size_t>(e.width) * 4);
    }
    for (const int step : scale_steps(k, scaler)) {
        rows = step < 0    ? nearest_rows(rows, static_cast<u32>(-step))
               : step == 2 ? scale2x(rows)
                           : scale3x(rows);
    }
    const u32 ow = e.width * k, oh = e.height * k;
    std::vector<u8> rgba(static_cast<std::size_t>(ow) * oh * 4);
    for (u32 y = 0; y < oh; ++y) {
        std::memcpy(rgba.data() + static_cast<std::size_t>(y) * ow * 4, rows[y].data(),
                    static_cast<std::size_t>(ow) * 4);
    }
    if (!write_dump_file(pack / host_path(e.file), rgba.data(), ow, oh)) {
        return e.file + ": cannot be written";
    }
    return "";
}

} // namespace

extern "C" int mt_texpack_build(const char *dump, const char *pack, unsigned k, int scaler, int ui,
                                mt_packcheck_fn fn, void *user) noexcept {
    mt_packcheck::Reporter r(fn, user, "texture");
    try {
        if (k < 1 || k > MT_GFX_MAX_SCALE) {
            r.refused("scale " + std::to_string(k) + ": must be 1 to " +
                      std::to_string(MT_GFX_MAX_SCALE));
            return -1;
        }
        if (scaler != MT_TEXPACK_SCALER_SCALE && scaler != MT_TEXPACK_SCALER_NEAREST) {
            r.refused("the scaler must be scale or nearest");
            return -1;
        }
        const fs::path from = host_path(dump != nullptr ? dump : "");
        const fs::path to = host_path(pack != nullptr ? pack : "");
        std::error_code ec;
        const fs::path manifest = from / "manifest.jsonl";
        if (dump == nullptr || *dump == '\0' || !fs::is_regular_file(manifest, ec)) {
            r.refused(std::string(dump != nullptr ? dump : "") +
                      ": no manifest.jsonl (not a texture dump)");
            return -1;
        }
        if (pack == nullptr || *pack == '\0' ||
            fs::weakly_canonical(from, ec) == fs::weakly_canonical(to, ec)) {
            r.refused("the pack must be another folder than the dump");
            return -1;
        }
        // The manifest is the dump's own file, but a dump is a folder anyone can edit: read
        // within a bound, line by line.
        const std::uintmax_t size = fs::file_size(manifest, ec);
        if (ec || size > (64u << 20)) {
            r.refused("manifest.jsonl is larger than 64 MiB");
            return -1;
        }
        std::ifstream in(manifest, std::ios::binary);
        std::vector<DumpEntry> entries;
        std::set<std::string> seen;
        std::string line;
        for (u64 number = 1; std::getline(in, line); ++number) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.find_first_not_of(" \t") == std::string::npos) {
                continue;
            }
            DumpEntry e;
            if (line.size() > 4096 || !dump_entry_of(line, &e)) {
                r.ignored("manifest line " + std::to_string(number) + ": " + line.substr(0, 80) +
                          ": skipped");
                continue;
            }
            if ((ui == 0 && e.file.rfind("ui/", 0) == 0) || !seen.insert(e.file).second) {
                continue;
            }
            entries.push_back(std::move(e));
        }
        fs::create_directories(to, ec);
        if (!fs::is_directory(to, ec)) {
            r.refused(std::string(pack) + ": cannot be made");
            return -1;
        }
        int written = 0;
        for (const DumpEntry &e : entries) {
            const std::string why = build_one(from, to, e, k, scaler);
            if (why.empty()) {
                ++written;
                ++r.report.good;
            } else {
                r.rejected(why);
            }
        }
        r.note(std::to_string(written) + " of " + std::to_string(entries.size()) +
               " pictures written at " + std::to_string(k) + "x");
        return written;
    } catch (const std::exception &) {
        r.refused("the build failed");
        return -1;
    }
}
