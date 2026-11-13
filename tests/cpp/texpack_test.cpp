// Texture packs and texture dumps (#153, ADR 0081, maplethief/texpack.h) on synthetic textures:
// the dump's files and manifest, a 1x pack built from the dump giving back every premultiplied
// texel byte for byte, a 2x pack premultiplied at load, the files a pack rejects, and the pack's
// place after hd_text in mt_gfx's chain. Nothing here comes from the game.
#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "cg/cg_objects.h"
#include "golden.h"
#include "maplethief/diag.h"
#include "maplethief/gfx.h"
#include "maplethief/texpack.h"
#include "texpack/texpack_internal.h"

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
namespace fs = std::filesystem;
namespace golden = maplethief::golden;

// A scratch directory, made empty, removed after.
struct Dir {
    fs::path path;
    explicit Dir(const char *name) {
        path = fs::temp_directory_path() / (std::string("maplethief-texpack-test-") + name);
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~Dir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    Dir(const Dir &) = delete;
    Dir &operator=(const Dir &) = delete;
    std::string str() const { return path.string(); }
};

// The runtime's log lines, while it lives; a fresh mt_gfx and no pack or dump around it.
struct Fresh {
    std::vector<std::string> lines;
    Fresh() {
        mt_gfx_reset();
        mt_diag_config cfg = mt_diag_config_default();
        cfg.log = [](void *user, const char *line) {
            static_cast<std::vector<std::string> *>(user)->emplace_back(line);
        };
        cfg.log_user = &lines;
        mt_diag_configure(&cfg);
    }
    ~Fresh() {
        mt_texpack_close();
        mt_texpack_dump_close();
        mt_gfx_set_replace_hook(nullptr, nullptr);
        mt_gfx_reset();
        mt_diag_reset();
    }
    int count(const std::string &text) const {
        int n = 0;
        for (const std::string &l : lines) {
            n += l.find(text) != std::string::npos ? 1 : 0;
        }
        return n;
    }
};

std::string name_of(const std::vector<u8> &texels, u32 w, u32 h, bool raw = false) {
    char name[MT_TEXPACK_NAME_MAX];
    mt_texpack_file_name(mt_gfx_upload_hash(w, h, MT_GFX_RGBA, MT_GFX_UNSIGNED_BYTE, texels.data()),
                         w, h, raw ? 1 : 0, name);
    return name;
}

u32 upload(const std::vector<u8> &texels, u32 w, u32 h) {
    const u32 tex = mt_gfx_texture_create();
    REQUIRE(tex != 0);
    REQUIRE(mt_gfx_texture_image(tex, w, h, MT_GFX_RGBA, MT_GFX_UNSIGNED_BYTE, texels.data()) == 0);
    return tex;
}

std::vector<u8> texels_of(u32 tex, u32 *w, u32 *h) {
    const u8 *p = mt_gfx_texture_texels(tex, w, h);
    REQUIRE(p != nullptr);
    return std::vector<u8>(p, p + static_cast<std::size_t>(*w) * *h * 4u);
}

void write_png(const fs::path &path, const std::vector<u8> &rgba, u32 w, u32 h) {
    golden::Image img(static_cast<int>(w), static_cast<int>(h));
    img.bytes() = rgba;
    std::string error;
    REQUIRE_MESSAGE(golden::write_png(path, img, &error), error);
}

void write_bytes(const fs::path &path, const std::vector<u8> &bytes) {
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

std::vector<std::string> read_lines(const fs::path &path) {
    std::ifstream f(path);
    std::vector<std::string> out;
    for (std::string line; std::getline(f, line);) {
        out.push_back(line);
    }
    return out;
}

// A symbolic link at `link` to `target`; false (and a message) where the system makes none (Windows
// without the privilege), so the caller skips what needs one.
bool make_symlink(const fs::path &target, const fs::path &link) {
    std::error_code ec;
    if (fs::is_directory(target)) {
        fs::create_directory_symlink(target, link, ec);
    } else {
        fs::create_symlink(target, link, ec);
    }
    if (ec) {
        MESSAGE("no symbolic link here (" << ec.message() << "): that part is skipped");
        return false;
    }
    return true;
}

// Premultiplied texels as the game's loader makes them (ADR 0041): 4 x 2 of varied alpha.
const std::vector<u8> k_premultiplied = {0,  0,   0,   0,  10,  20,  30,  40,  51,  51, 51,
                                         51, 200, 100, 50, 255, 255, 255, 255, 255, 1,  0,
                                         1,  2,   38,  38, 38,  38,  120, 60,  119, 128};
// Colours above alpha: not premultiplied.
const std::vector<u8> k_straight = {255, 0, 0, 128, 0, 255, 0, 64, 0, 0, 255, 0, 9, 9, 9, 9};

} // namespace

TEST_CASE("texpack: file names are <hash>_<W>x<H>.png and .raw.png") {
    char name[MT_TEXPACK_NAME_MAX];
    mt_texpack_file_name(0x00AB00CD00EF0012ull, 512, 32, 0, name);
    CHECK(std::string(name) == "00ab00cd00ef0012_512x32.png");
    mt_texpack_file_name(0xFFFFFFFFFFFFFFFFull, 4096, 4096, 1, name);
    CHECK(std::string(name) == "ffffffffffffffff_4096x4096.raw.png");
}

TEST_CASE("texpack: a dump writes every upload once, straight alpha, and a manifest line") {
    Fresh fresh;
    Dir dir("dump");
    REQUIRE(mt_texpack_dump_open(dir.str().c_str()) == 0);
    const u32 a = upload(k_premultiplied, 4, 2);
    const u32 b = upload(k_straight, 2, 2);
    upload(k_premultiplied, 4, 2); // the same texels again: not written again
    mt_texpack_stats s = mt_texpack_get_stats();
    CHECK(s.dumped == 2);
    CHECK(s.dump_errors == 0);
    // The dump replaces nothing.
    u32 w = 0, h = 0;
    CHECK(texels_of(a, &w, &h) == k_premultiplied);
    CHECK(texels_of(b, &w, &h) == k_straight);
    CHECK(mt_gfx_get_stats().replacements == 0);

    // The premultiplied upload: un-premultiplied, round(c * 255 / a).
    const auto pa = golden::load_png(dir.path / name_of(k_premultiplied, 4, 2));
    REQUIRE(pa.has_value());
    CHECK(pa->width() == 4);
    CHECK(pa->height() == 2);
    const std::vector<u8> straight = {0,   0,   0,   0,   64,  128, 191, 40,  255, 255, 255,
                                      51,  200, 100, 50,  255, 255, 255, 255, 255, 128, 0,
                                      128, 2,   255, 255, 255, 38,  239, 120, 237, 128};
    CHECK(pa->bytes() == straight);
    // The other: as stored, in .raw.png.
    const auto pb = golden::load_png(dir.path / name_of(k_straight, 2, 2, true));
    REQUIRE(pb.has_value());
    CHECK(pb->bytes() == k_straight);
    CHECK_FALSE(fs::exists(dir.path / name_of(k_straight, 2, 2)));

    const std::vector<std::string> manifest = read_lines(dir.path / "manifest.jsonl");
    REQUIRE(manifest.size() == 2);
    char hash[17];
    std::snprintf(hash, sizeof hash, "%016llx",
                  static_cast<unsigned long long>(mt_gfx_upload_hash(
                      4, 2, MT_GFX_RGBA, MT_GFX_UNSIGNED_BYTE, k_premultiplied.data())));
    CHECK(manifest[0] == std::string("{\"hash\":\"") + hash + "\",\"file\":\"" +
                             name_of(k_premultiplied, 4, 2) +
                             "\",\"width\":4,\"height\":2,\"format\":\"0x1908\",\"type\":"
                             "\"0x1401\",\"alpha\":\"straight\"}");
    CHECK(manifest[1].find("\"alpha\":\"raw\"") != std::string::npos);

    // A second dump into the same directory writes only what is new.
    mt_texpack_dump_close();
    CHECK(mt_gfx_replace_hook_count() == 0);
    REQUIRE(mt_texpack_dump_open(dir.str().c_str()) == 0);
    CHECK(fresh.count("(2 there already)") == 1);
    upload(k_premultiplied, 4, 2);
    CHECK(mt_texpack_get_stats().dumped == 0);
    const std::vector<u8> other(4 * 4, 7); // 2 x 2 of (7, 7, 7, 7)
    upload(other, 2, 2);
    CHECK(mt_texpack_get_stats().dumped == 1);
    CHECK(read_lines(dir.path / "manifest.jsonl").size() == 3);
}

TEST_CASE("texpack: a 1x pack made from the dump gives back every premultiplied texel exactly") {
    Fresh fresh;
    Dir dump("roundtrip-dump");
    Dir pack("roundtrip-pack");
    // Every colour c <= alpha a once (32,896 pairs) in R, with G = a - c and B = c / 2: a
    // 256 x 129 upload (the rest transparent black).
    const u32 w = 256, h = 129;
    std::vector<u8> texels(static_cast<std::size_t>(w) * h * 4, 0);
    std::size_t i = 0;
    for (u32 a = 0; a < 256; ++a) {
        for (u32 c = 0; c <= a; ++c, i += 4) {
            texels[i] = static_cast<u8>(c);
            texels[i + 1] = static_cast<u8>(a - c);
            texels[i + 2] = static_cast<u8>(c / 2);
            texels[i + 3] = static_cast<u8>(a);
        }
    }
    REQUIRE(mt_texpack_dump_open(dump.str().c_str()) == 0);
    upload(texels, w, h);
    mt_texpack_dump_close();
    const std::string name = name_of(texels, w, h);
    REQUIRE(fs::exists(dump.path / name));
    fs::copy_file(dump.path / name, pack.path / name);
    write_bytes(pack.path / "notes.txt", {'h', 'i'}); // not a pack file: ignored

    REQUIRE(mt_texpack_open(pack.str().c_str()) == 1);
    CHECK(fresh.count("1 textures, 0 UIKit images, 1 other files ignored") == 1);
    const u32 tex = upload(texels, w, h);
    u32 tw = 0, th = 0;
    CHECK(texels_of(tex, &tw, &th) == texels);
    CHECK(tw == w);
    const mt_texpack_stats s = mt_texpack_get_stats();
    CHECK(s.files == 1);
    CHECK(s.uploads == 1);
    CHECK(s.replaced == 1);
    CHECK(mt_gfx_get_stats().replacements == 1);
    // Another upload has no file: kept, counted.
    const u32 kept = upload(k_premultiplied, 4, 2);
    CHECK(texels_of(kept, &tw, &th) == k_premultiplied);
    CHECK(mt_texpack_get_stats().missing == 1);
}

TEST_CASE("texpack: a 2x pack is premultiplied at load, a .raw.png used as it is") {
    Fresh fresh;
    Dir pack("pack2x");
    // The 4 x 2 upload's 2x image, straight: each texel of `big` its own value.
    std::vector<u8> big;
    for (u32 n = 0; n < 8 * 4; ++n) {
        big.insert(big.end(), {static_cast<u8>(n * 8), 200, 255, static_cast<u8>(n * 8 + 3)});
    }
    write_png(pack.path / name_of(k_premultiplied, 4, 2), big, 8, 4);
    write_png(pack.path / name_of(k_straight, 2, 2, true), std::vector<u8>(4 * 4 * 4, 77), 4, 4);
    REQUIRE(mt_texpack_open(pack.str().c_str()) == 2);
    const u32 a = upload(k_premultiplied, 4, 2);
    u32 w = 0, h = 0;
    const std::vector<u8> got = texels_of(a, &w, &h);
    CHECK(w == 8);
    CHECK(h == 4);
    u32 size_w = 0, size_h = 0;
    CHECK(mt_gfx_texture_size(a, &size_w, &size_h) == 0);
    CHECK(size_w == 4); // GL's size stays the upload's
    for (std::size_t i = 0; i < big.size(); i += 4) {
        const u32 al = big[i + 3];
        CHECK(got[i] == (big[i] * al + 127) / 255);
        CHECK(got[i + 1] == (200 * al + 127) / 255);
        CHECK(got[i + 2] == al); // 255 * a / 255
        CHECK(got[i + 3] == al);
    }
    const u32 b = upload(k_straight, 2, 2);
    CHECK(texels_of(b, &w, &h) == std::vector<u8>(4 * 4 * 4, 77));
    CHECK(mt_texpack_get_stats().replaced == 2);
}

TEST_CASE("texpack: files of the wrong size, no PNG or a broken one are rejected and logged once") {
    Fresh fresh;
    Dir pack("bad");
    const std::vector<u8> t1(2 * 2 * 4, 1), t2(2 * 2 * 4, 2), t3(2 * 2 * 4, 3), t4(2 * 2 * 4, 4),
        t5(2 * 2 * 4, 5);
    // 4 x 6 for a 2 x 2 upload: not one k for both axes.
    write_png(pack.path / name_of(t1, 2, 2), std::vector<u8>(4 * 6 * 4, 9), 4, 6);
    // Text with a PNG's name.
    write_bytes(pack.path / name_of(t2, 2, 2), {'n', 'o', 't', ' ', 'a', ' ', 'p', 'n', 'g'});
    // A PNG cut inside its data: the header says 2 x 2, the pixels are gone.
    std::vector<u8> png;
    REQUIRE(mt_cg::png_encode(std::vector<u8>(16, 50).data(), 2, 2, 4, png));
    png.resize(45); // the signature, IHDR and the first bytes of IDAT
    write_bytes(pack.path / name_of(t3, 2, 2), png);
    // 18 x 18: k = 9, above MT_GFX_MAX_SCALE.
    write_png(pack.path / name_of(t4, 2, 2), std::vector<u8>(18 * 18 * 4, 9), 18, 18);
    // A file of the right name for another size of the same texels' hash: not this upload's.
    write_png(pack.path / name_of(t5, 1, 4), std::vector<u8>(16, 9), 1, 4);
    REQUIRE(mt_texpack_open(pack.str().c_str()) == 5);
    for (const auto *t : {&t1, &t2, &t3, &t4}) {
        for (int twice = 0; twice < 2; ++twice) {
            const u32 tex = upload(*t, 2, 2);
            u32 w = 0, h = 0;
            CHECK(texels_of(tex, &w, &h) == *t);
            CHECK(w == 2);
        }
    }
    const u32 t5tex = upload(t5, 2, 2);
    u32 w = 0, h = 0;
    CHECK(texels_of(t5tex, &w, &h) == t5);
    const mt_texpack_stats s = mt_texpack_get_stats();
    CHECK(s.uploads == 9);
    CHECK(s.replaced == 0);
    CHECK(s.rejected == 8);
    CHECK(s.missing == 1);
    CHECK(fresh.count("rejected, the game's texture kept") == 4); // once per file
    CHECK(fresh.count("4x6 is not k times 2x2 (k 1..8, at most 4096)") == 1);
    CHECK(fresh.count("18x18 is not k times 2x2") == 1);
    CHECK(fresh.count("is no PNG") == 1);
    CHECK(fresh.count("does not decode") == 1);
    CHECK(mt_gfx_get_stats().rejected_replacements == 0); // all caught before mt_gfx
}

namespace {

std::vector<u8> g_text_pixels(4 * 4 * 4, 222);
bool g_text_replaces = true;
int fake_text(void *, const mt_gfx_upload *, mt_gfx_replacement *out) {
    if (!g_text_replaces) {
        return 0;
    }
    out->pixels = g_text_pixels.data();
    out->width = out->height = 4;
    return 1;
}

} // namespace

TEST_CASE("texpack: the pack comes after hd_text in the chain, the dump before both") {
    Fresh fresh;
    Dir pack("chain-pack");
    Dir dump("chain-dump");
    const std::vector<u8> t(2 * 2 * 4, 40);
    write_png(pack.path / name_of(t, 2, 2), std::vector<u8>(6 * 6 * 4, 255), 6, 6);
    REQUIRE(mt_texpack_open(pack.str().c_str()) == 1);
    REQUIRE(mt_gfx_add_replace_hook(MT_GFX_REPLACE_ORDER_TEXT, fake_text, nullptr) == 0);
    REQUIRE(mt_texpack_dump_open(dump.str().c_str()) == 0);
    CHECK(mt_gfx_replace_hook_count() == 3);
    g_text_replaces = true;
    const u32 a = upload(t, 2, 2);
    u32 w = 0, h = 0;
    CHECK(texels_of(a, &w, &h) == g_text_pixels); // the text's
    CHECK(mt_texpack_get_stats().uploads == 0);   // the pack was not asked
    CHECK(mt_texpack_get_stats().dumped == 1);    // the dump saw it
    g_text_replaces = false;
    const u32 b = upload(t, 2, 2);
    texels_of(b, &w, &h);
    CHECK(w == 6); // the pack's
    CHECK(mt_texpack_get_stats().replaced == 1);
    mt_gfx_remove_replace_hook(fake_text, nullptr);
    mt_texpack_close();
    mt_texpack_dump_close();
    CHECK(mt_gfx_replace_hook_count() == 0);
}

TEST_CASE("texpack: a pack that is no directory opens nothing") {
    Fresh fresh;
    Dir dir("nodir");
    write_bytes(dir.path / "file", {1});
    CHECK(mt_texpack_open((dir.path / "file").string().c_str()) == -1);
    CHECK(mt_texpack_open((dir.path / "absent").string().c_str()) == -1);
    CHECK(mt_texpack_open("") == -1);
    CHECK(mt_texpack_open(nullptr) == -1);
    CHECK(fresh.count("is no directory: no pack") == 4);
    CHECK(mt_gfx_replace_hook_count() == 0);
}

TEST_CASE("texpack: a symbolic link in a pack is not followed, logged once (#413)") {
    Fresh fresh;
    Dir pack("links");
    Dir outside("links-outside");
    const std::vector<u8> linked(2 * 2 * 4, 61), real(2 * 2 * 4, 62);
    write_png(outside.path / "far.png", std::vector<u8>(2 * 2 * 4, 200), 2, 2);
    fs::create_directories(outside.path / "art");
    write_png(outside.path / "art" / "Menu.png", std::vector<u8>(2 * 2 * 4, 200), 2, 2);
    write_png(pack.path / name_of(real, 2, 2), std::vector<u8>(2 * 2 * 4, 201), 2, 2);
    fs::create_directories(pack.path / "ui");
    write_png(pack.path / "ui" / "Real.png", std::vector<u8>(2 * 2 * 4, 201), 2, 2);
    if (!make_symlink(outside.path / "far.png", pack.path / name_of(linked, 2, 2)) ||
        !make_symlink(outside.path / "far.png", pack.path / "ui" / "Linked.png") ||
        !make_symlink(outside.path / "art", pack.path / "ui" / "art")) {
        return;
    }
    // The real texture and the real ui/ image only; the links are skipped, in one line.
    REQUIRE(mt_texpack_open(pack.str().c_str()) == 2);
    CHECK(mt_texpack_get_stats().files == 1);
    CHECK(mt_texpack_get_stats().ui_files == 1);
    CHECK(fresh.count("symbolic link") == 1);
    const u32 tex = upload(linked, 2, 2);
    u32 w = 0, h = 0;
    CHECK(texels_of(tex, &w, &h) == linked); // the game's texture
    CHECK(mt_texpack_get_stats().missing == 1);
    // A ui/ folder that is itself a link: nothing below it is indexed.
    Dir pack2("links-ui");
    if (!make_symlink(outside.path / "art", pack2.path / "ui")) {
        return;
    }
    CHECK(mt_texpack_open(pack2.str().c_str()) == 0);
    CHECK(mt_texpack_get_stats().ui_files == 0);
    CHECK(fresh.count("symbolic link") == 2);
}

TEST_CASE("texpack: a pack indexes at most MT_TEXPACK_MAX_FILES files, the rest skipped and "
          "logged once (#413)") {
    CHECK(MT_TEXPACK_MAX_FILES == 65536u);
    Fresh fresh;
    Dir pack("count");
    for (u32 i = 0; i < 4; ++i) {
        char name[MT_TEXPACK_NAME_MAX];
        mt_texpack_file_name(0x1000u + i, 2, 2, 0, name);
        write_png(pack.path / name, std::vector<u8>(2 * 2 * 4, 9), 2, 2);
    }
    fs::create_directories(pack.path / "ui");
    write_png(pack.path / "ui" / "a.png", std::vector<u8>(2 * 2 * 4, 9), 2, 2);
    write_png(pack.path / "ui" / "b.png", std::vector<u8>(2 * 2 * 4, 9), 2, 2);
    const std::uint32_t old = mt_texpack::set_max_files(3);
    const int indexed = mt_texpack_open(pack.str().c_str());
    mt_texpack::set_max_files(old);
    CHECK(indexed == 3);
    CHECK(mt_texpack_get_stats().files + mt_texpack_get_stats().ui_files == 3);
    CHECK(fresh.count("3 files past the first 3 skipped") == 1);
    // Under the cap, all of them.
    CHECK(mt_texpack_open(pack.str().c_str()) == 6);
    CHECK(fresh.count("skipped") == 1);
}

TEST_CASE("texpack: a file over 80 MiB is rejected unread; one of 80 MiB is read (#413)") {
    Fresh fresh;
    Dir pack("large");
    const std::vector<u8> at(2 * 2 * 4, 71), over(2 * 2 * 4, 72);
    // Sparse files: a PNG's signature, then zeros.
    const auto sparse = [](const fs::path &path, std::uintmax_t size) {
        write_bytes(path, {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'});
        fs::resize_file(path, size);
    };
    sparse(pack.path / name_of(at, 2, 2), std::uintmax_t{80} << 20);
    sparse(pack.path / name_of(over, 2, 2), (std::uintmax_t{80} << 20) + 1);
    REQUIRE(mt_texpack_open(pack.str().c_str()) == 2);
    u32 w = 0, h = 0;
    CHECK(texels_of(upload(at, 2, 2), &w, &h) == at);
    CHECK(texels_of(upload(over, 2, 2), &w, &h) == over);
    CHECK(fresh.count(name_of(at, 2, 2) + " is no PNG") == 1); // read, then refused
    CHECK(fresh.count(name_of(over, 2, 2) + " is larger than 80 MiB") == 1);
    CHECK(mt_texpack_get_stats().rejected == 2);
}

TEST_CASE("texpack: a name's width and height are 1 to 5 digits, at most 4096 (#413)") {
    Fresh fresh;
    Dir pack("names");
    const char *const good[] = {"00000000000000aa_4096x1.png", "00000000000000ab_1x4096.raw.png",
                                "00000000000000AC_64x64.png"};
    const char *const bad[] = {
        "00000000000000ba_4097x1.png",   // above 4,096
        "00000000000000bb_99999x1.png",  // 5 digits, above 4,096
        "00000000000000bc_000064x1.png", // 6 digits
        "00000000000000bd_100000x1.png", // 6 digits
        "00000000000000be_064x64.png",   // a leading zero
        "00000000000000bf_0x64.png",     // 0
        "00000000000000c0_x64.png",      // no digits
        "00000000000000c1_64x64.jpg",    // not .png
        "00000000000000c2_64-64.png",    // no x
        "0000000000000c3_64x64.png",     // 15 hex digits
        "00000000000000g4_64x64.png",    // not hex
    };
    for (const char *n : good) {
        write_bytes(pack.path / n, {1});
    }
    for (const char *n : bad) {
        write_bytes(pack.path / n, {1});
    }
    CHECK(mt_texpack_open(pack.str().c_str()) == static_cast<int>(std::size(good)));
    CHECK(fresh.count(", " + std::to_string(std::size(bad)) + " other files ignored") == 1);
}

TEST_CASE("texpack: ui/ paths are 5 to 255 bytes of [A-Za-z0-9 _-+.@] parts ending .png (#413)") {
    Fresh fresh;
    Dir pack("ui-names");
    const fs::path ui = pack.path / "ui";
    const std::string part(200, 'a');
    const std::vector<std::string> good = {"a.png", "Menu BG@2x.png", "x_y-z+1.png",
                                           "sub/dir/Icon.png",
                                           part + "/" + std::string(50, 'b') + ".png"}; // 255
    const std::vector<std::string> bad = {
        "Menu.PNG",                                 // .png in lower case only
        "Menu.png.txt",                             // not .png
        "caf\xC3\xA9.png",                          // not ASCII
        "a$b.png",                                  // a character outside the set
        "sub/(x).png",                              // in a folder too
        part + "/" + std::string(51, 'b') + ".png", // 256 bytes
    };
    // Each file made; a path too long for the system (Windows' 260 characters) is left out.
    const auto make = [&](const std::vector<std::string> &list) {
        std::size_t made = 0;
        for (const std::string &rel : list) {
            const fs::path p = ui / fs::path(std::u8string(rel.begin(), rel.end()));
            std::error_code ec;
            fs::create_directories(p.parent_path(), ec);
            write_bytes(p, {1});
            made += fs::exists(p, ec) ? 1u : 0u;
        }
        return made;
    };
    const std::size_t good_made = make(good), bad_made = make(bad);
    CHECK(good_made >= good.size() - 1);
    CHECK(mt_texpack_open(pack.str().c_str()) == static_cast<int>(good_made));
    CHECK(mt_texpack_get_stats().ui_files == good_made);
    CHECK(fresh.count(", " + std::to_string(bad_made) + " other files ignored") == 1);
}
