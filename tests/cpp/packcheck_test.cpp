// The pack tools in the runtime (#371, ADR 0205, maplethief/packcheck.h, texpack.h) on synthetic
// packs written at run time: each kind's check against its loader's rules (a good file, each
// rejection with its reason, the names passed over), pack.strings' rules (ADR 0149), the kind
// told from a folder, a text pack opened by its pack.strings, the texture pack builder's Scale2x
// and Scale3x worked by hand (tools/mtport/texpack.py's tests check the same) and its manifest
// rules, and the text export from a synthetic bundle. Nothing here comes from the game.
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "cg/cg_objects.h"
#include "maplethief/audio.h"
#include "maplethief/packcheck.h"
#include "maplethief/texpack.h"
#include "maplethief/textpack.h"
#include "textpack/textpack_internal.h"

namespace {

namespace fs = std::filesystem;
using u8 = std::uint8_t;
using u32 = std::uint32_t;

// A scratch directory, made empty, removed after.
struct Dir {
    fs::path path;
    explicit Dir(const char *name) {
        path = fs::temp_directory_path() / (std::string("maplethief-packcheck-test-") + name);
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

void write(const fs::path &p, const std::string &bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}
void write(const fs::path &p, const std::vector<u8> &bytes) {
    write(p, std::string(bytes.begin(), bytes.end()));
}
std::string read(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

// A w x h PNG of straight RGBA, each pixel `rgba` unless `pixels` is given.
std::vector<u8> png(u32 w, u32 h, u32 rgba = 0xFF2040C0u,
                    const std::vector<u32> *pixels = nullptr) {
    std::vector<u8> px(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i) {
        const u32 v = pixels != nullptr ? (*pixels)[i] : rgba;
        std::memcpy(px.data() + i * 4, &v, 4);
    }
    std::vector<u8> out;
    REQUIRE(mt_cg::png_encode(px.data(), w, h, 4, out));
    return out;
}

std::vector<u32> pixels_of(const fs::path &p, u32 *w, u32 *h) {
    const std::string bytes = read(p);
    mt_cg::Decoded d;
    std::string error;
    REQUIRE(
        mt_cg::png_decode(reinterpret_cast<const u8 *>(bytes.data()), bytes.size(), &d, &error));
    *w = d.width;
    *h = d.height;
    std::vector<u32> out(static_cast<std::size_t>(d.width) * d.height);
    std::memcpy(out.data(), d.pixels->data(), out.size() * 4);
    return out;
}

// The check's lines by level.
struct Lines {
    std::vector<std::string> note, ignored, rejected;
    static void add(void *user, int level, const char *line) {
        auto *l = static_cast<Lines *>(user);
        (level == MT_PACKCHECK_REJECTED  ? l->rejected
         : level == MT_PACKCHECK_IGNORED ? l->ignored
                                         : l->note)
            .emplace_back(line);
    }
    static bool has(const std::vector<std::string> &v, const std::string &text) {
        for (const std::string &l : v) {
            if (l.find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
};

std::string name(const char *hash, u32 w, u32 h, const char *ext = "png") {
    return std::string(hash) + "_" + std::to_string(w) + "x" + std::to_string(h) + "." + ext;
}

// 16-bit PCM WAV of `frames` frames of silence-but-one (a click), `rate` Hz, mono.
std::vector<u8> wav(u32 frames, u32 rate) {
    std::vector<u8> b;
    const auto le = [&b](u32 v, int n) {
        for (int i = 0; i < n; ++i) {
            b.push_back(static_cast<u8>(v >> (8 * i)));
        }
    };
    const u32 data = frames * 2;
    b.insert(b.end(), {'R', 'I', 'F', 'F'});
    le(36 + data, 4);
    b.insert(b.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    le(16, 4);
    le(1, 2);
    le(1, 2);
    le(rate, 4);
    le(rate * 2, 4);
    le(2, 2);
    le(16, 2);
    b.insert(b.end(), {'d', 'a', 't', 'a'});
    le(data, 4);
    for (u32 i = 0; i < frames; ++i) {
        le(i == 0 ? 1000u : 0u, 2);
    }
    return b;
}

const std::string kManifest =
    "\"format\" = \"1\";\n\"kind\" = \"texture\";\n\"id\" = \"hd.sprites\";\n"
    "\"name\" = \"HD sprites\";\n";

} // namespace

TEST_CASE("packcheck: a texture pack checked by the loader's rules: good, rejected, ignored "
          "(#371)") {
    Dir d("texture");
    const fs::path p = d.path;
    write(p / "pack.strings", kManifest);
    write(p / name("00000000000000a1", 4, 2), png(8, 4));            // 2x: good
    write(p / name("00000000000000a2", 4, 2, "raw.png"), png(4, 2)); // 1x raw: good
    write(p / name("00000000000000a3", 4, 2), png(9, 4));            // not k times
    write(p / name("00000000000000a4", 2, 2), png(18, 18));          // k = 9
    write(p / name("00000000000000a5", 2, 2), std::string("not a picture"));
    std::vector<u8> cut = png(4, 4);
    cut.resize(cut.size() - 20);
    write(p / name("00000000000000a6", 2, 2), cut); // cut in its data
    write(p / "notes.txt", std::string("hello"));
    write(p / "manifest.jsonl", std::string("{}"));
    write(p / "ui" / "Button.png", png(24, 20)); // 2x of the game's 12 x 10
    write(p / "ui" / "Bad.png", png(25, 20));
    write(p / "ui" / "Gone.png", png(10, 10)); // the game has no such image
    write(p / "ui" / "a b" / ".." / "x.txt", std::string("x"));
    Dir app("texture-app");
    write(app.path / "Button.png", png(12, 10));
    write(app.path / "Bad.png", png(12, 10));

    Lines l;
    const mt_packcheck_report r =
        mt_texpack_check(d.str().c_str(), app.str().c_str(), Lines::add, &l);
    CHECK(r.opens == 1);
    CHECK(std::strcmp(r.kind, "texture") == 0);
    CHECK(r.good == 3);     // a1, a2, ui/Button.png
    CHECK(r.rejected == 5); // a3, a4, a5, a6, ui/Bad.png
    CHECK(Lines::has(l.rejected, name("00000000000000a3", 4, 2) +
                                     " 9x4 is not k times 4x2 (k 1..8, at most 4096)"));
    CHECK(Lines::has(l.rejected, "18x18 is not k times 2x2"));
    CHECK(Lines::has(l.rejected, name("00000000000000a5", 2, 2) + " is no PNG"));
    CHECK(Lines::has(l.rejected, name("00000000000000a6", 2, 2) + " does not decode"));
    CHECK(Lines::has(l.rejected, "ui/Bad.png 25x20 is not k times 12x10"));
    CHECK(Lines::has(l.ignored, "notes.txt: not a texture's name"));
    CHECK(Lines::has(l.ignored, "ui/Gone.png: the game has no image of that name"));
    CHECK(Lines::has(l.ignored, "ui/x.txt: not a texture's name"));
    CHECK_FALSE(Lines::has(l.ignored, "manifest.jsonl"));
    CHECK_FALSE(Lines::has(l.ignored, "pack.strings"));
    CHECK(Lines::has(l.note, "3 of 9 pictures usable (1 at 1x, 2 at 2x)"));

    // Without the game's folder a ui/ image is checked as itself (a note says so).
    Lines bare;
    const mt_packcheck_report b = mt_texpack_check(d.str().c_str(), nullptr, Lines::add, &bare);
    CHECK(b.good == 5); // a1, a2 and the three ui/ PNGs as their own size
    CHECK(b.rejected == 4);
    CHECK(Lines::has(bare.note, "3 ui/ images not compared with the game's"));

    // What the check says is what the game does: the same pack opened indexes the same files.
    CHECK(mt_texpack_open(d.str().c_str()) == 9);
    mt_texpack_close();
}

TEST_CASE("packcheck: a texture pack's symbolic link is ignored, never followed (#371)") {
    Dir d("texture-link");
    Dir outside("texture-link-target");
    write(outside.path / "secret.png", png(4, 2));
    std::error_code ec;
    fs::create_symlink(outside.path / "secret.png", d.path / name("00000000000000b1", 4, 2), ec);
    if (ec) {
        MESSAGE("symbolic links not available here: " << ec.message());
        return;
    }
    Lines l;
    const mt_packcheck_report r = mt_texpack_check(d.str().c_str(), nullptr, Lines::add, &l);
    CHECK(r.good == 0);
    CHECK(Lines::has(l.ignored, "1 symbolic links not followed"));
    CHECK(Lines::has(l.ignored, "no texture files: the pack replaces nothing"));
}

TEST_CASE("packcheck: pack.strings by the manifest reader's rules (#370), each refusal with its "
          "reason (#371)") {
    struct Case {
        std::string manifest;
        const char *reason; // nullptr: the pack opens
    };
    const std::string kind_id = "\"kind\" = \"texture\";\n\"id\" = \"a\";\n\"name\" = \"A\";\n";
    const Case cases[] = {
        {"\"format\" = \"1\";\n" + kind_id, nullptr},
        {kind_id, "it has no \"format\""},
        {"\"format\" = \"2\";\n" + kind_id, "pack format 2 needs a newer port"},
        {"\"format\" = \"one\";\n" + kind_id, "its \"format\" is not a number"},
        {"\"format\" = \"1\";\n\"kind\" = \"audio\";\n\"id\" = \"a\";\n\"name\" = \"A\";\n",
         "it is an audio pack, not a texture pack"},
        {"\"format\" = \"1\";\n\"kind\" = \"music\";\n\"id\" = \"a\";\n\"name\" = \"A\";\n",
         "its \"kind\" is not texture, audio or text"},
        {"\"format\" = \"1\";\n\"kind\" = \"texture\";\n\"id\" = \"-a\";\n\"name\" = \"A\";\n",
         "its \"id\" is not 1 to 64"},
        {"\"format\" = \"1\";\n\"kind\" = \"texture\";\n\"id\" = \"Big\";\n\"name\" = \"A\";\n",
         "its \"id\" is not 1 to 64"},
        {"\"format\" = \"1\";\n\"kind\" = \"texture\";\n\"id\" = \"a\";\n", "it has no \"name\""},
        {"\"format\" = \"1\";\n\"kind\" = \"texture\";\n\"id\" = \"a\";\n\"name\" = \"" +
             std::string(65, 'n') + "\";\n",
         "its \"name\" is longer than 64 characters"},
        // 64 characters of two bytes each: characters count, not bytes.
        {"\"format\" = \"1\";\n" + kind_id.substr(0, kind_id.find("\"name\"")) + "\"name\" = \"" +
             [] {
                 std::string s;
                 for (int i = 0; i < 64; ++i) {
                     s += "\xC3\xB6";
                 }
                 return s;
             }() +
             "\";\n",
         nullptr},
        {"\"format\" = \"1\";\n" + kind_id + "\"version\" = \"" + std::string(33, 'v') + "\";\n",
         "its \"version\" is longer than 32 characters"},
        {"\"format\" = \"1\";\n" + kind_id + "\"game_version\" = \"1.6\";\n",
         "made for the game's version 1.6; this port runs 1.5"},
        {"\"format\" = \"1\";\n" + kind_id + "\"game_version\" = \"1.5\";\n", nullptr},
        {"\"format\" = \"1\";\n" + kind_id + "\"port_min\" = \"999.0.0\";\n",
         "needs port 999.0.0 or later; this is " MT_VERSION},
        {"\"format\" = \"1\";\n" + kind_id + "\"port_min\" = \"0.0.0\";\n", nullptr},
        {"\"format\" = \"1\";\n" + kind_id + "\"port_min\" = \"soon\";\n",
         "its \"port_min\" is not a version (X.Y.Z)"},
        // Only a text pack's language is read.
        {"\"format\" = \"1\";\n" + kind_id + "\"language\" = \"de de\";\n", nullptr},
        {"\"format\" = \"1\";\n" + kind_id + "\"broken\n", "line "},
    };
    for (const Case &c : cases) {
        CAPTURE(c.manifest);
        Dir d("manifest");
        write(d.path / "pack.strings", c.manifest);
        write(d.path / name("00000000000000c1", 2, 2), png(2, 2));
        Lines l;
        const mt_packcheck_report r = mt_texpack_check(d.str().c_str(), nullptr, Lines::add, &l);
        if (c.reason == nullptr) {
            CHECK(r.opens == 1);
            CHECK(l.rejected.empty());
        } else {
            CHECK(r.opens == 0);
            CHECK(Lines::has(l.rejected, std::string("pack.strings: ") + c.reason));
            CHECK(Lines::has(l.rejected, "the pack is refused"));
        }
    }
    // An unknown key is passed over, a strange "content" too; no pack.strings is a note.
    Dir d("manifest-extra");
    write(d.path / "pack.strings",
          "\"format\" = \"1\";\n" + kind_id + "\"colour\" = \"red\";\n\"content\" = \"mine\";\n");
    Lines l;
    CHECK(mt_texpack_check(d.str().c_str(), nullptr, Lines::add, &l).opens == 1);
    CHECK(Lines::has(l.note, "\"colour\" is not a key of format 1: ignored"));
    CHECK(Lines::has(l.ignored, "its \"content\" is neither own nor derived"));
    fs::remove(d.path / "pack.strings");
    Lines none;
    CHECK(mt_texpack_check(d.str().c_str(), nullptr, Lines::add, &none).opens == 1);
    CHECK(Lines::has(none.note, "pack.strings: none: the game shows the folder's name"));
}

TEST_CASE("packcheck: an audio pack checked as effects and tracks by the game's own sounds "
          "(#371)") {
    Dir app("audio-app");
    write(app.path / "sound" / "12.wav", std::string("x")); // an effect
    write(app.path / "sound" / "31.mp3", std::string("x")); // a track
    write(app.path / "sound" / "40.wav", std::string("x"));
    Dir d("audio");
    const fs::path p = d.path;
    write(p / "12.wav", wav(800, 8000));                 // 0.1 s effect: good
    write(p / "31.wav", wav(8000 * 61, 8000));           // a 61 s track: good
    write(p / "31.flac", std::string("fLaC"));           // hidden by 31.wav
    write(p / "31.loop", std::string("100 200\n"));      // in range
    write(p / "40.wav", wav(8000 * 61, 8000));           // a 61 s effect: rejected
    write(p / "41.wav", wav(800, 8000));                 // no game sound 41
    write(p / "50.mp3", std::string("ID3 nothing"));     // ignored: no sound 50
    write(p / "audiopack.txt", std::string("the list")); // the init's list
    write(p / "cover.jpg", std::string("x"));            // not a sound
    Lines l;
    const mt_packcheck_report r =
        mt_audio_pack_check(d.str().c_str(), app.str().c_str(), Lines::add, &l);
    CHECK(r.opens == 1);
    CHECK(std::strcmp(r.kind, "audio") == 0);
    CHECK(r.good == 2);
    CHECK(r.rejected == 1);
    CHECK(Lines::has(l.rejected, "40.wav (replaces 40.wav) is longer than 60 s: rejected"));
    CHECK(Lines::has(l.ignored, "31.flac: 31.wav replaces the same sound: ignored"));
    CHECK(Lines::has(l.ignored, "41.wav: the game has no sound 41: ignored"));
    CHECK(Lines::has(l.ignored, "50.mp3: the game has no sound 50: ignored"));
    CHECK(Lines::has(l.ignored, "cover.jpg: not a sound"));
    CHECK_FALSE(Lines::has(l.ignored, "audiopack.txt"));

    // A broken loop, one past the track's end, a loop for an effect.
    write(p / "31.loop", std::string("200 100\n"));
    write(p / "12.loop", std::string("1 2\n"));
    Lines l2;
    mt_audio_pack_check(d.str().c_str(), app.str().c_str(), Lines::add, &l2);
    CHECK(Lines::has(l2.rejected, "31.loop is not \"start end\""));
    CHECK(Lines::has(l2.ignored, "12.loop: 12.wav is an effect, which never loops"));
    // An end past the file is cut to it (as the game does); a start past it is no loop.
    write(p / "31.loop", std::string("9999999 99999999\n"));
    Lines l3;
    mt_audio_pack_check(d.str().c_str(), app.str().c_str(), Lines::add, &l3);
    CHECK(Lines::has(l3.rejected, "31.loop 9999999 to 99999999 is outside 31.wav's frames"));

    // Without the game's sounds every file is a track: the 61 s effect passes, with a note.
    Lines bare;
    const mt_packcheck_report b = mt_audio_pack_check(d.str().c_str(), nullptr, Lines::add, &bare);
    CHECK(Lines::has(bare.note, "the game's sounds not at hand"));
    CHECK_FALSE(Lines::has(bare.rejected, "40.wav"));
    CHECK(Lines::has(bare.rejected, "50.mp3")); // no MP3 audio in it
    CHECK(b.good == 4);                         // 12, 31, 40, 41
}

TEST_CASE("packcheck: a text pack as the game reads it, pack.strings or textpack.strings "
          "(#371)") {
    Dir d("text");
    const fs::path p = d.path;
    write(p / "textpack.strings",
          std::string("\"format\" = \"1\";\n\"name\" = \"Deutsch\";\n\"language\" = \"de\";\n"));
    write(p / "Scripts.strings", std::string("\"A.b.1\" = \"Hallo\";\n\"A.b.2\" = \"Welt\";\n"));
    write(p / "Tables.strings",
          "\"item_etc.0.name\" = \"" + std::string(70, 'x') + "\";\n\"mob.x.name\" = \"y\";\n");
    write(p / "Port.strings",
          std::string("\"hint_move\" = \"bewegen\";\n\"nope\" = \"x\";\n\"saves_slot\" = "
                      "\"Platz\";\n"));
    write(p / "ScriptExtra.strings", std::string("\"a\" = \"\xC3\x28\";\n")); // bad UTF-8
    write(p / "Other.strings", std::string(""));
    mt_packcheck_set_port_text(
        [](void *, const char *key) -> const char * {
            return std::strcmp(key, "hint_move") == 0    ? "move"
                   : std::strcmp(key, "saves_slot") == 0 ? "Slot {0}"
                                                         : nullptr;
        },
        nullptr);
    Lines l;
    const mt_packcheck_report r = mt_textpack_check(d.str().c_str(), Lines::add, &l);
    mt_packcheck_set_port_text(nullptr, nullptr);
    CHECK(r.opens == 1);
    CHECK(std::strcmp(r.kind, "text") == 0);
    CHECK(r.good == 4); // the 2 Scripts, item_etc.0.name, hint_move
    CHECK(r.rejected == 1);
    CHECK(Lines::has(l.rejected, "ScriptExtra.strings: line 1: invalid UTF-8"));
    CHECK(Lines::has(l.note, "Tables.strings \"item_etc.0.name\": 70 bytes, its field holds 64"));
    CHECK(Lines::has(l.ignored, "Tables.strings \"mob.x.name\": the row is not a number"));
    CHECK(Lines::has(l.ignored, "Port.strings \"nope\": the port has no such text"));
    CHECK(Lines::has(l.ignored, "Port.strings \"saves_slot\": its places"));
    CHECK(Lines::has(l.ignored, "Other.strings: not a text pack file"));
    CHECK(Lines::has(l.note, "textpack.strings: the older manifest"));

    // pack.strings counts when it is there: then textpack.strings is passed over, and a manifest
    // without a language refuses the pack.
    write(p / "pack.strings", std::string("\"format\" = \"1\";\n\"kind\" = \"text\";\n\"id\" = "
                                          "\"de\";\n\"name\" = \"Deutsch\";\n"));
    Lines l2;
    CHECK(mt_textpack_check(d.str().c_str(), Lines::add, &l2).opens == 0);
    CHECK(Lines::has(l2.rejected, "pack.strings: its \"language\" is not a language tag"));
    CHECK(Lines::has(l2.ignored, "textpack.strings: pack.strings is the manifest: ignored"));
    CHECK(mt_textpack_open(d.str().c_str()) == -1); // the game refuses it the same way

    write(p / "pack.strings", std::string("\"format\" = \"1\";\n\"kind\" = \"text\";\n\"id\" = "
                                          "\"de\";\n\"name\" = \"Deutsch\";\n\"language\" = "
                                          "\"de\";\n"));
    fs::remove(p / "textpack.strings");
    Lines l3;
    CHECK(mt_textpack_check(d.str().c_str(), Lines::add, &l3).opens == 1);
    // The game opens a pack whose manifest is pack.strings alone (ADR 0149).
    CHECK(mt_textpack_open(d.str().c_str()) >= 0);
    CHECK(std::strcmp(mt_textpack_manifest("name"), "Deutsch") == 0);
    CHECK(std::strcmp(mt_textpack_lookup("Scripts", "A.b.2"), "Welt") == 0);
    mt_textpack_close();

    // No manifest at all: refused.
    fs::remove(p / "pack.strings");
    Lines l4;
    CHECK(mt_textpack_check(d.str().c_str(), Lines::add, &l4).opens == 0);
    CHECK(Lines::has(l4.rejected, "no pack.strings or textpack.strings"));
}

TEST_CASE("packcheck: the kind told from a folder, and mt_pack_check by it (#371)") {
    Dir d("kind");
    CHECK(std::string(mt_pack_kind(d.str().c_str())).empty());
    Lines l;
    const mt_packcheck_report none = mt_pack_check(d.str().c_str(), "", nullptr, Lines::add, &l);
    CHECK(none.opens == 0);
    CHECK(std::string(none.kind).empty());
    write(d.path / "31.mp3", std::string("x"));
    CHECK(std::string(mt_pack_kind(d.str().c_str())) == "audio");
    write(d.path / name("00000000000000d1", 2, 2), png(2, 2));
    CHECK(std::string(mt_pack_kind(d.str().c_str())) == "texture");
    write(d.path / "textpack.strings", std::string(""));
    CHECK(std::string(mt_pack_kind(d.str().c_str())) == "text");
    write(d.path / "pack.strings", std::string("\"kind\" = \"audio\";\n"));
    CHECK(std::string(mt_pack_kind(d.str().c_str())) == "audio");
    CHECK(std::string(mt_pack_check(d.str().c_str(), "", nullptr, nullptr, nullptr).kind) ==
          "audio");
    CHECK(std::string(mt_pack_check(d.str().c_str(), "texture", nullptr, nullptr, nullptr).kind) ==
          "texture");
    CHECK(std::string(mt_pack_kind((d.path / "missing").string().c_str())).empty());
}

TEST_CASE("packcheck: Scale2x and Scale3x by hand, as mtport.texpack's (#371)") {
    Dir d("scale");
    const u32 A = 0xFF0000FFu, B = 0xFFFF0000u;
    // A diagonal: A on and above it, B below.
    const std::vector<u32> diag = {A, A, B, A};
    write(d.path / "dump" / name("00000000000000e1", 2, 2), png(2, 2, 0, &diag));
    // Flat: blocks.
    const std::vector<u32> flat(4, A);
    write(d.path / "dump" / name("00000000000000e2", 2, 2), png(2, 2, 0, &flat));
    write(d.path / "dump" / "manifest.jsonl",
          "{\"hash\":\"00000000000000e1\",\"file\":\"" + name("00000000000000e1", 2, 2) +
              "\",\"width\":2,\"height\":2}\n{\"file\":\"" + name("00000000000000e2", 2, 2) +
              "\",\"width\":2,\"height\":2}\n");
    for (const u32 k : {2u, 3u}) {
        CAPTURE(k);
        const fs::path out = d.path / ("pack" + std::to_string(k));
        Lines l;
        CHECK(mt_texpack_build((d.path / "dump").string().c_str(), out.string().c_str(), k,
                               MT_TEXPACK_SCALER_SCALE, 1, Lines::add, &l) == 2);
        u32 w = 0, h = 0;
        const std::vector<u32> got = pixels_of(out / name("00000000000000e1", 2, 2), &w, &h);
        REQUIRE(w == 2 * k);
        REQUIRE(h == 2 * k);
        if (k == 2) {
            // E = B at (0, 1): B above is A... Scale2x of [[A, A], [B, A]]:
            const std::vector<u32> want = {A, A, A, A, //
                                           A, A, A, A, //
                                           B, A, A, A, //
                                           B, B, A, A};
            CHECK(got == want);
        } else {
            const std::vector<u32> want = {A, A, A, A, A, A, //
                                           A, A, A, A, A, A, //
                                           A, A, A, A, A, A, //
                                           B, A, A, A, A, A, //
                                           B, B, A, A, A, A, //
                                           B, B, B, A, A, A};
            CHECK(got == want);
        }
        const std::vector<u32> blocks = pixels_of(out / name("00000000000000e2", 2, 2), &w, &h);
        CHECK(blocks == std::vector<u32>(static_cast<std::size_t>(4 * k * k), A));
        // The built pack checks clean and opens.
        CHECK(mt_texpack_check(out.string().c_str(), nullptr, nullptr, nullptr).rejected == 0);
    }
    // Nearest: blocks of the diagonal, 5x (no Scale composition for 5).
    const fs::path n5 = d.path / "pack5";
    CHECK(mt_texpack_build((d.path / "dump").string().c_str(), n5.string().c_str(), 5,
                           MT_TEXPACK_SCALER_SCALE, 1, nullptr, nullptr) == 2);
    u32 w = 0, h = 0;
    const std::vector<u32> got = pixels_of(n5 / name("00000000000000e1", 2, 2), &w, &h);
    CHECK(w == 10);
    CHECK(got[5 * 10 + 4] == B);
    CHECK(got[5 * 10 + 5] == A);
    CHECK(got[9 * 10 + 9] == A);
}

TEST_CASE("packcheck: the builder skips manifest lines it cannot trust and refuses bad requests "
          "(#371)") {
    Dir d("build");
    const fs::path dump = d.path / "dump";
    write(dump / name("00000000000000f1", 4, 2), png(4, 2));
    write(dump / name("00000000000000f2", 4, 2), png(4, 4)); // the file disagrees
    write(dump / "ui" / "Menu" / "Back.png", png(3, 3));
    write(dump / "manifest.jsonl",
          "{\"file\":\"" + name("00000000000000f1", 4, 2) +
              "\",\"width\":4,\"height\":2}\n"
              "{\"file\":\"" +
              name("00000000000000f1", 4, 2) +
              "\",\"width\":4,\"height\":2}\n" // once only
              "{\"file\":\"" +
              name("00000000000000f2", 4, 2) +
              "\",\"width\":4,\"height\":2}\n"
              "{\"file\":\"" +
              name("00000000000000f3", 4, 2) +
              "\",\"width\":8,\"height\":2}\n" // size vs name
              "{\"file\":\"ui/../escape.png\",\"width\":3,\"height\":3}\n"
              "{\"ui\":\"Menu/Back.png\",\"file\":\"ui/Menu/Back.png\",\"width\":3,\"height\":3}\n"
              "not json\n\n");
    const fs::path pack = d.path / "pack";
    Lines l;
    CHECK(mt_texpack_build(dump.string().c_str(), pack.string().c_str(), 2,
                           MT_TEXPACK_SCALER_NEAREST, 1, Lines::add, &l) == 2);
    CHECK(fs::exists(pack / name("00000000000000f1", 4, 2)));
    CHECK(fs::exists(pack / "ui" / "Menu" / "Back.png"));
    CHECK_FALSE(fs::exists(d.path / "escape.png"));
    CHECK(l.ignored.size() == 3); // f3, ui/../, not json
    CHECK(Lines::has(l.ignored, "manifest line 4:"));
    CHECK(Lines::has(l.ignored, "manifest line 7: not json: skipped"));
    CHECK(Lines::has(l.rejected, name("00000000000000f2", 4, 2) + ": 4x4, the manifest says 4x2"));
    CHECK(Lines::has(l.note, "2 of 3 pictures written at 2x"));
    // --pack-no-ui
    const fs::path no_ui = d.path / "no-ui";
    CHECK(mt_texpack_build(dump.string().c_str(), no_ui.string().c_str(), 1,
                           MT_TEXPACK_SCALER_SCALE, 0, nullptr, nullptr) == 1);
    CHECK_FALSE(fs::exists(no_ui / "ui"));
    // Refused before anything is read.
    Lines bad;
    CHECK(mt_texpack_build(dump.string().c_str(), pack.string().c_str(), 9, MT_TEXPACK_SCALER_SCALE,
                           1, Lines::add, &bad) == -1);
    CHECK(mt_texpack_build(dump.string().c_str(), dump.string().c_str(), 2, MT_TEXPACK_SCALER_SCALE,
                           1, Lines::add, &bad) == -1);
    CHECK(mt_texpack_build(pack.string().c_str(), no_ui.string().c_str(), 2,
                           MT_TEXPACK_SCALER_SCALE, 1, Lines::add, &bad) == -1);
    CHECK(Lines::has(bad.rejected, "scale 9: must be 1 to 8"));
    CHECK(Lines::has(bad.rejected, "the pack must be another folder than the dump"));
    CHECK(Lines::has(bad.rejected, "no manifest.jsonl (not a texture dump)"));
}

namespace {

// A binary plist of one dictionary of ASCII strings (1-byte offsets and references: small).
std::string bplist_dict(const std::vector<std::pair<std::string, std::string>> &entries) {
    std::vector<std::string> objects;
    const auto ascii = [](const std::string &s) {
        REQUIRE(s.size() < 15);
        return std::string(1, static_cast<char>(0x50 | s.size())) + s;
    };
    std::string dict(1, static_cast<char>(0xD0 | entries.size()));
    REQUIRE(entries.size() < 15);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        dict += static_cast<char>(1 + i);
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
        dict += static_cast<char>(1 + entries.size() + i);
    }
    objects.push_back(dict);
    for (const auto &e : entries) {
        objects.push_back(ascii(e.first));
    }
    for (const auto &e : entries) {
        objects.push_back(ascii(e.second));
    }
    std::string out = "bplist00";
    std::vector<std::size_t> offsets;
    for (const std::string &o : objects) {
        offsets.push_back(out.size());
        out += o;
    }
    const std::size_t table = out.size();
    for (const std::size_t off : offsets) {
        REQUIRE(off < 256);
        out += static_cast<char>(off);
    }
    std::string trailer(32, '\0');
    trailer[6] = 1; // offset size
    trailer[7] = 1; // object ref size
    trailer[15] = static_cast<char>(objects.size());
    trailer[23] = 0; // the root
    trailer[31] = static_cast<char>(table);
    return out + trailer;
}

// A table of the layout textpack.cpp's text columns name, with `rows` rows; row r's text
// columns hold "<column> <r>" (and row 1's item_etc name a CP949 Hangul syllable).
std::vector<u8> table(const char *stem, u32 rows) {
    std::size_t n = 0;
    const mt_textpack::TextColumn *cols = mt_textpack::text_columns(&n);
    const mt_textpack::TextColumn *any = nullptr;
    for (std::size_t i = 0; i < n; ++i) {
        any = std::strcmp(cols[i].table, stem) == 0 ? &cols[i] : any;
    }
    REQUIRE(any != nullptr);
    std::vector<u8> b(6 + static_cast<std::size_t>(rows) * any->stride, 0);
    const std::uint16_t header[3] = {static_cast<std::uint16_t>(any->columns),
                                     static_cast<std::uint16_t>(rows),
                                     static_cast<std::uint16_t>(any->stride)};
    std::memcpy(b.data(), header, 6);
    for (std::size_t i = 0; i < n; ++i) {
        if (std::strcmp(cols[i].table, stem) != 0) {
            continue;
        }
        for (u32 r = 0; r < rows; ++r) {
            std::string text = std::string(cols[i].column) + " " + std::to_string(r);
            if (r == 1 && std::strcmp(stem, "item_etc") == 0 &&
                std::strcmp(cols[i].column, "name") == 0) {
                text = "\xB0\xA1"; // U+AC00 in CP949
            }
            std::memcpy(b.data() + 6 + r * cols[i].stride + cols[i].offset, text.data(),
                        text.size());
        }
    }
    return b;
}

} // namespace

TEST_CASE("packcheck: the text export writes the game's text as a pack that checks clean "
          "(#371)") {
    Dir app("export-app");
    write(app.path / "en.lproj" / "Scripts.strings",
          bplist_dict({{"Q.talk.2", "Bye"}, {"Q.talk.1", "Hi \"you\""}}));
    write(app.path / "en.lproj" / "ScriptExtra.strings", bplist_dict({{"X.y.1", "Extra"}}));
    for (const char *t : {"mob", "npc", "item_equip", "item_etc", "skill"}) {
        write(app.path / "English" / "data" / (std::string(t) + ".dat"), table(t, 2));
    }
    Dir d("export");
    Lines l;
    // 3 dialogue lines, and 2 rows of each of the 8 text columns.
    CHECK(mt_textpack_export(app.str().c_str(), "en", d.str().c_str(), Lines::add, &l) == 3 + 16);
    CHECK(l.rejected.empty());
    const std::string scripts = read(d.path / "Scripts.strings");
    CHECK(scripts.find("\"Q.talk.1\" = \"Hi \\\"you\\\"\";\n\"Q.talk.2\" = \"Bye\";") !=
          std::string::npos); // sorted, quoted
    const std::string tables = read(d.path / "Tables.strings");
    CHECK(tables.find("/* at most 64 bytes */\n\"item_etc.1.name\" = \"\xEA\xB0\x80\";") !=
          std::string::npos);
    CHECK(tables.find("\"skill.0.detail\" = \"detail 0\";") != std::string::npos);
    const std::string manifest = read(d.path / "pack.strings");
    CHECK(manifest.find("\"kind\" = \"text\";") != std::string::npos);
    CHECK(manifest.find("\"content\" = \"derived\";") != std::string::npos);
    CHECK_FALSE(fs::exists(d.path / "textpack.strings"));
    // It checks clean and opens.
    Lines c;
    const mt_packcheck_report r = mt_textpack_check(d.str().c_str(), Lines::add, &c);
    CHECK(r.opens == 1);
    CHECK(r.rejected == 0);
    CHECK(c.ignored.empty());
    CHECK(r.good == 19);
    CHECK(mt_textpack_open(d.str().c_str()) == 19);
    CHECK(std::strcmp(mt_textpack_lookup("Scripts", "Q.talk.1"), "Hi \"you\"") == 0);
    mt_textpack_close();

    // No bundle, a broken table, another language: refused with the reason.
    Lines e;
    CHECK(mt_textpack_export((app.path / "nope").string().c_str(), "en", d.str().c_str(),
                             Lines::add, &e) == -1);
    CHECK(mt_textpack_export(app.str().c_str(), "fr", d.str().c_str(), Lines::add, &e) == -1);
    write(app.path / "English" / "data" / "npc.dat", std::string("short"));
    CHECK(mt_textpack_export(app.str().c_str(), "en", d.str().c_str(), Lines::add, &e) == -1);
    CHECK(Lines::has(e.rejected, "is not there"));
    CHECK(Lines::has(e.rejected, "the game's text is in en or ko, not fr"));
    CHECK(Lines::has(e.rejected, "npc.dat is not the table the game reads"));
}

TEST_CASE("packcheck: mt_textpack_quote writes what the parser reads back (#371)") {
    char out[64];
    CHECK(mt_textpack_quote("a\"b\\c\nd\te\x01", out, sizeof out) == 21);
    CHECK(std::string(out) == "\"a\\\"b\\\\c\\nd\\te\\U0001\"");
    CHECK(mt_textpack_quote("abcdef", out, 4) == 8); // the length it needs
    CHECK(std::string(out) == "\"ab");
}
