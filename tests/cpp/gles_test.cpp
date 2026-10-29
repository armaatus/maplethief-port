// OpenGL ES 1.1 and EAGL recorded, audio silent, CommonCrypto's digests (#117, ADR 0038) on a
// real guest address space with synthetic data only: the GL state tracker (names, binds,
// glIsTexture, texture sizes, framebuffer status, renderbuffer queries, matrix stacks, misuse),
// the command and present hooks, EAGLContext over a view whose layer is a CAEAGLLayer (the
// renderbuffer storage from the layer's bounds and drawableProperties, the first frame),
// OpenAL's names and error state, the audio session, ExtAudioFile on a synthetic WAV (and
// hostile ones), AVAudioPlayer, and CC_MD5 / CC_SHA1 against the RFC 1321 and FIPS 180
// test vectors.
#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <random>
#include <string>
#include <vector>

#include "audio/audio_internal.h"
#include "foundation/foundation.h"
#include "gles/gles_internal.h"
#include "hle/hle.h"
#include "maplethief/audio.h"
#include "maplethief/cf.h"
#include "maplethief/gles.h"
#include "maplethief/hle/imports.h"
#include "maplethief/memory.h"
#include "maplethief/objc/host.h"
#include "maplethief/objc/objc.h"
#include "maplethief/stubs.h"

namespace {

using u32 = std::uint32_t;
using u8 = std::uint8_t;
namespace fs = std::filesystem;

std::vector<std::string> g_log;
void capture(void *, const char *line) { g_log.emplace_back(line); }

bool logged(const char *text) {
    for (const auto &l : g_log) {
        if (l.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

u32 fb(float f) {
    u32 b;
    std::memcpy(&b, &f, 4);
    return b;
}

// GL enums
enum : u32 {
    GL_TEXTURE_2D = 0x0DE1,
    GL_UNSIGNED_BYTE = 0x1401,
    GL_UNSIGNED_SHORT_5_6_5 = 0x8363,
    GL_RGB = 0x1907,
    GL_RGBA = 0x1908,
    GL_MODELVIEW = 0x1700,
    GL_PROJECTION = 0x1701,
    GL_TRIANGLES = 0x0004,
    GL_FRAMEBUFFER_OES = 0x8D40,
    GL_RENDERBUFFER_OES = 0x8D41,
    GL_COLOR_ATTACHMENT0_OES = 0x8CE0,
    GL_FRAMEBUFFER_COMPLETE_OES = 0x8CD5,
    GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT_OES = 0x8CD6,
    GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT_OES = 0x8CD7,
    GL_RENDERBUFFER_WIDTH_OES = 0x8D42,
    GL_RENDERBUFFER_HEIGHT_OES = 0x8D43,
    GL_RENDERBUFFER_INTERNAL_FORMAT_OES = 0x8D44,
    GL_RENDERBUFFER_GREEN_SIZE_OES = 0x8D51,
    GL_RENDERBUFFER_ALPHA_SIZE_OES = 0x8D53,
    GL_RGBA8_OES = 0x8058,
    GL_RGB565_OES = 0x8D62,
};

// One data directory per test process, removed when the process ends.
struct TestDir {
    fs::path path = fs::temp_directory_path() /
                    ("maplethief-gles-test-" + std::to_string(std::random_device{}()) + "-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ~TestDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};
fs::path test_dir() {
    static const TestDir dir;
    return dir.path;
}

// GlesTestView: a view whose layer is a CAEAGLLayer, like the game's EAGLView.
void gl_layer_class(mt_cpu *cpu) {
    cpu->r[0] = mt_objc_class_named(mt_objc_current(), "CAEAGLLayer");
}
const mt_objc_host_method k_gl_class_methods[] = {{"layerClass", gl_layer_class}};
const mt_objc_host_class k_gles_test_view = {"GlesTestView",     "UIView", 46, nullptr, 0,
                                             k_gl_class_methods, 1};

// A guest address space with the runtime's host classes, CF bound to it, a data directory.
struct Env {
    mt_mem *mem = nullptr;
    mt_cpu cpu{};
    mt_stack stack{};
    mt_objc *rt = nullptr;
    u32 pool = 0;

    Env() {
        g_log.clear();
        mem = mt_mem_create(nullptr);
        REQUIRE(mem != nullptr);
        mt_mem_attach(mem, &cpu);
        REQUIRE(mt_stack_create(mem, 0, &stack) == 0);
        cpu.r[MT_SP] = stack.top;
        cpu.r[MT_LR] = 0x2345u;
        mt_import_config cfg = mt_import_config_default();
        cfg.log = capture;
        rt = mt_objc_create(&cpu, mt_mem_guest_alloc, mem, &cfg);
        REQUIRE(rt != nullptr);
        mt_objc_set_free(rt, mt_mem_guest_free);
        mt_objc_set_current(rt);
        REQUIRE(mt_objc_add_host_classes(rt) == 0u);
        REQUIRE(mt_objc_add_host_class(rt, &k_gles_test_view, 0, 0) == 0);
        mt_cf_start(&cpu, rt);
        std::error_code ec;
        fs::create_directories(test_dir() / "Documents", ec);
        REQUIRE(mt_stubs_set_data_dir(test_dir().string().c_str()) == 0);
        pool = mt_objc_pool_push(rt);
        mt_gles_configure(nullptr);
    }
    ~Env() {
        mt_gles_configure(nullptr);
        mt_objc_pool_pop(rt, pool);
        mt_objc_set_current(nullptr);
        mt_objc_destroy(rt);
        mt_mem_destroy(mem);
        mt_stubs_set_data_dir(nullptr);
    }
    Env(const Env &) = delete;
    Env &operator=(const Env &) = delete;

    u32 klass(const char *name) const { return mt_objc_class_named(rt, name); }
    u32 send(u32 self, const char *sel, std::initializer_list<u32> args = {}) {
        const std::vector<u32> v(args);
        return mt_objc_msg(&cpu, self, mt_objc_sel(rt, sel), v.data(), static_cast<u32>(v.size()));
    }
    u32 string(const char *text) { return mt_ns::string_from_utf8(&cpu, text); }
    // Calls an HLE import with word arguments (those past r3 on the guest stack).
    u32 call(mt_import_fn fn, std::initializer_list<u32> args) {
        const u32 sp = cpu.r[MT_SP];
        const u32 stacked = args.size() > 4 ? static_cast<u32>(args.size()) - 4u : 0u;
        cpu.r[MT_SP] = sp - 8u * ((stacked + 1u) / 2u);
        u32 i = 0;
        for (u32 a : args) {
            if (i < 4) {
                cpu.r[i] = a;
            } else {
                std::memcpy(cpu.mem + cpu.r[MT_SP] + 4u * (i - 4u), &a, 4);
            }
            i++;
        }
        fn(&cpu, 0);
        cpu.r[MT_SP] = sp;
        return cpu.r[0];
    }
    u32 alloc(u32 bytes) {
        const u32 p = mt_heap_calloc(mem, 1, bytes);
        REQUIRE(p != 0);
        return p;
    }
    u32 get32(u32 addr) const {
        u32 v;
        std::memcpy(&v, cpu.mem + addr, 4);
        return v;
    }
    void put32(u32 addr, u32 v) { std::memcpy(cpu.mem + addr, &v, 4); }
    u32 bytes(const void *data, u32 n) {
        const u32 p = alloc(n + 1);
        std::memcpy(cpu.mem + p, data, n);
        return p;
    }
    std::string hex(u32 addr, u32 n) const {
        std::string s;
        char b[3];
        for (u32 i = 0; i < n; i++) {
            std::snprintf(b, sizeof b, "%02x", cpu.mem[addr + i]);
            s += b;
        }
        return s;
    }
    // A file URL for <data dir>/<rest> as the guest names it (its home directory).
    u32 home_url(const char *rest) {
        const u32 path = string((mt_ns::home_guest_path() + "/" + rest).c_str());
        return send(send(klass("NSURL"), "alloc"), "initFileURLWithPath:", {path});
    }

    // The game's -[EAGLView createGLes] on a 480x320 GlesTestView: a context, framebuffer and
    // renderbuffer, the renderbuffer's storage from the layer. Returns the context.
    u32 create_gl(const char *color_format = "EAGLColorFormatRGBA8", u32 *layer_out = nullptr) {
        const u32 view = send(send(klass("GlesTestView"), "alloc"),
                              "initWithFrame:", {fb(0), fb(0), fb(480), fb(320)});
        const u32 layer = send(view, "layer");
        const u32 no = send(klass("NSNumber"), "numberWithBool:", {0});
        const u32 props =
            send(klass("NSDictionary"), "dictionaryWithObjectsAndKeys:",
                 {no, string("EAGLDrawablePropertyRetainedBacking"), string(color_format),
                  string("EAGLDrawablePropertyColorFormat"), 0u});
        send(layer, "setDrawableProperties:", {props});
        const u32 context =
            send(send(klass("EAGLContext"), "alloc"), "initWithAPI:", {1u /* OpenGL ES 1 */});
        REQUIRE(context != 0);
        REQUIRE(send(klass("EAGLContext"), "setCurrentContext:", {context}) == 1);
        const u32 names = alloc(8);
        call(mt_hle_glGenFramebuffersOES, {1, names});
        call(mt_hle_glGenRenderbuffersOES, {1, names + 4});
        call(mt_hle_glBindFramebufferOES, {GL_FRAMEBUFFER_OES, get32(names)});
        call(mt_hle_glBindRenderbufferOES, {GL_RENDERBUFFER_OES, get32(names + 4)});
        CHECK(call(mt_hle_glCheckFramebufferStatusOES, {GL_FRAMEBUFFER_OES}) ==
              GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT_OES);
        REQUIRE(send(context, "renderbufferStorage:fromDrawable:", {GL_RENDERBUFFER_OES, layer}) ==
                1);
        call(mt_hle_glFramebufferRenderbufferOES,
             {GL_FRAMEBUFFER_OES, GL_COLOR_ATTACHMENT0_OES, GL_RENDERBUFFER_OES, get32(names + 4)});
        if (layer_out != nullptr) {
            *layer_out = layer;
        }
        return context;
    }
};

// --- OpenGL ES: the state tracker ----------------------------------------------------------------

TEST_CASE("gles: texture names, binds, glIsTexture and texture sizes") {
    Env e;
    const u32 names = e.alloc(16);
    e.call(mt_hle_glGenTextures, {3, names});
    CHECK(e.get32(names) == 1);
    CHECK(e.get32(names + 4) == 2);
    CHECK(e.get32(names + 8) == 3);
    // a name from glGenTextures is no texture object until it is bound (GL ES 1.1, 3.8.13)
    CHECK(e.call(mt_hle_glIsTexture, {1}) == 0);
    e.call(mt_hle_glBindTexture, {GL_TEXTURE_2D, 1});
    CHECK(e.call(mt_hle_glIsTexture, {1}) == 1);
    CHECK(e.call(mt_hle_glIsTexture, {0}) == 0);
    CHECK(e.call(mt_hle_glIsTexture, {99}) == 0);
    e.call(mt_hle_glTexImage2D,
           {GL_TEXTURE_2D, 0, GL_RGBA, 64, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0});
    e.call(mt_hle_glBindTexture, {GL_TEXTURE_2D, 2});
    e.call(mt_hle_glTexImage2D,
           {GL_TEXTURE_2D, 0, GL_RGB, 16, 16, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 0});
    mt_gles_stats st = mt_gles_get_stats();
    CHECK(st.textures == 3);
    CHECK(st.texture_bytes == 64u * 32u * 4u + 16u * 16u * 2u);
    CHECK(st.bound_texture == 2);
    // a re-upload replaces the level-0 size
    e.call(mt_hle_glTexImage2D, {GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0});
    CHECK(mt_gles_get_stats().texture_bytes == 64u * 32u * 4u + 8u * 8u * 4u);
    // deleting frees the name and unbinds it; unused names and 0 are ignored
    e.put32(names + 12, 0);
    e.call(mt_hle_glDeleteTextures, {2, names + 4}); // 2 and 3
    e.put32(names + 12, 77);
    e.call(mt_hle_glDeleteTextures, {1, names + 12});
    st = mt_gles_get_stats();
    CHECK(st.textures == 1);
    CHECK(st.texture_bytes == 64u * 32u * 4u);
    CHECK(st.bound_texture == 0);
    CHECK(e.call(mt_hle_glIsTexture, {2}) == 0);
    // a fresh glGenTextures skips the name still in use
    e.call(mt_hle_glGenTextures, {1, names});
    CHECK(e.get32(names) == 4);
    CHECK(st.misuses == 0);
    CHECK(st.op_calls[MT_GLES_TexImage2D] == 3);
    CHECK(std::string(mt_gles_op_name(MT_GLES_TexImage2D)) == "glTexImage2D");
    CHECK(mt_gles_op_name(MT_GLES_OP_COUNT) == nullptr);
}

TEST_CASE("gles: misuse is counted and logged, never fatal") {
    Env e;
    e.call(mt_hle_glBindTexture, {0x1234, 1});
    CHECK(logged("gl: glBindTexture: target is not GL_TEXTURE_2D"));
    e.call(mt_hle_glTexImage2D,
           {GL_TEXTURE_2D, 0, GL_RGBA, 4096, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0});
    e.call(mt_hle_glMatrixMode, {GL_PROJECTION});
    e.call(mt_hle_glPushMatrix, {});
    e.call(mt_hle_glPushMatrix, {}); // projection stack: 2 deep
    CHECK(logged("gl: glPushMatrix: stack overflow"));
    e.call(mt_hle_glPopMatrix, {});
    e.call(mt_hle_glPopMatrix, {});
    CHECK(logged("gl: glPopMatrix: stack underflow"));
    e.call(mt_hle_glMatrixMode, {GL_MODELVIEW});
    for (int i = 0; i < 15; i++) {
        e.call(mt_hle_glPushMatrix, {}); // modelview: 16 deep, no misuse
    }
    e.call(mt_hle_glFramebufferRenderbufferOES,
           {GL_FRAMEBUFFER_OES, GL_COLOR_ATTACHMENT0_OES, GL_RENDERBUFFER_OES, 1});
    CHECK(logged("gl: glFramebufferRenderbufferOES: no framebuffer is bound"));
    e.call(mt_hle_glViewport, {0, 0, static_cast<u32>(-1), 10});
    e.call(mt_hle_glDrawArrays, {GL_TRIANGLES, 0, static_cast<u32>(-3)});
    const mt_gles_stats st = mt_gles_get_stats();
    CHECK(st.misuses == 7);
    CHECK(st.viewport[2] == 0);
}

TEST_CASE("gles: the command hook sees every call after the state tracker, with its result") {
    Env e;
    struct Seen {
        std::vector<mt_gles_cmd> cmds;
        u32 pointer_word = 0;
    } seen;
    mt_gles_config cfg{};
    cfg.user = &seen;
    cfg.command = [](void *user, mt_cpu *cpu, const mt_gles_cmd *cmd) {
        auto *s = static_cast<Seen *>(user);
        s->cmds.push_back(*cmd);
        if (cmd->op == MT_GLES_VertexPointer) {
            // the client array is valid inside the call
            std::memcpy(&s->pointer_word, cpu->mem + cmd->args[3], 4);
        }
    };
    mt_gles_configure(&cfg);
    const u32 verts = e.alloc(16);
    e.put32(verts, 0xCAFEF00Du);
    e.call(mt_hle_glVertexPointer, {2, 0x140C /* GL_FIXED */, 0, verts});
    e.call(mt_hle_glOrthox, {0, 480u << 16, 320u << 16, 0, static_cast<u32>(-1) << 16, 1u << 16});
    e.call(mt_hle_glScalef, {fb(2.0f), fb(0.5f), fb(1.0f)});
    e.call(mt_hle_glIsTexture, {5});
    REQUIRE(seen.cmds.size() == 4);
    CHECK(seen.pointer_word == 0xCAFEF00Du);
    CHECK(seen.cmds[0].op == MT_GLES_VertexPointer);
    CHECK(seen.cmds[0].argc == 4);
    CHECK(seen.cmds[1].op == MT_GLES_Orthox);
    CHECK(seen.cmds[1].argc == 6);
    CHECK(seen.cmds[1].args[1] == 480u << 16);
    CHECK(seen.cmds[1].args[5] == 1u << 16);
    CHECK(seen.cmds[2].args[0] == fb(2.0f));
    CHECK(seen.cmds[3].op == MT_GLES_IsTexture);
    CHECK(seen.cmds[3].result == 0);
    CHECK(mt_gles_get_stats().calls == 4);
}

// --- EAGL
// ------------------------------------------------------------------------------------------

TEST_CASE("eagl: the renderbuffer gets the CAEAGLLayer's size and format; presents make frames") {
    Env e;
    struct Frames {
        std::vector<mt_gles_frame> frames;
    } rec;
    mt_gles_config cfg{};
    cfg.user = &rec;
    cfg.present = [](void *user, mt_cpu *, const mt_gles_frame *f) {
        static_cast<Frames *>(user)->frames.push_back(*f);
        return 0;
    };
    mt_gles_configure(&cfg);
    const u32 context = e.create_gl();
    mt_gles_stats st = mt_gles_get_stats();
    CHECK(st.contexts == 1);
    CHECK(st.current_context == context);
    CHECK(e.call(mt_hle_glCheckFramebufferStatusOES, {GL_FRAMEBUFFER_OES}) ==
          GL_FRAMEBUFFER_COMPLETE_OES);
    const u32 out = e.alloc(4);
    e.call(mt_hle_glGetRenderbufferParameterivOES,
           {GL_RENDERBUFFER_OES, GL_RENDERBUFFER_WIDTH_OES, out});
    CHECK(e.get32(out) == 480);
    e.call(mt_hle_glGetRenderbufferParameterivOES,
           {GL_RENDERBUFFER_OES, GL_RENDERBUFFER_HEIGHT_OES, out});
    CHECK(e.get32(out) == 320);
    e.call(mt_hle_glGetRenderbufferParameterivOES,
           {GL_RENDERBUFFER_OES, GL_RENDERBUFFER_INTERNAL_FORMAT_OES, out});
    CHECK(e.get32(out) == GL_RGBA8_OES);
    e.call(mt_hle_glGetRenderbufferParameterivOES,
           {GL_RENDERBUFFER_OES, GL_RENDERBUFFER_ALPHA_SIZE_OES, out});
    CHECK(e.get32(out) == 8);

    // a frame with no draw call is no "first frame"
    e.call(mt_hle_glViewport, {0, 0, 480, 320});
    CHECK(e.send(context, "presentRenderbuffer:", {GL_RENDERBUFFER_OES}) == 1);
    CHECK(mt_gles_get_stats().first_frame == 0);
    e.call(mt_hle_glDrawArrays, {GL_TRIANGLES, 0, 6});
    e.call(mt_hle_glDrawArrays, {GL_TRIANGLES, 6, 6});
    e.call(mt_hle_glTexImage2D, {GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0});
    CHECK(e.send(context, "presentRenderbuffer:", {GL_RENDERBUFFER_OES}) == 1);
    REQUIRE(rec.frames.size() == 2);
    const mt_gles_frame &f = rec.frames[1];
    CHECK(f.index == 2);
    CHECK(f.width == 480);
    CHECK(f.height == 320);
    CHECK(f.format == GL_RGBA8_OES);
    CHECK(f.retained_backing == 0);
    CHECK(f.calls == 3);
    CHECK(f.draws == 2);
    CHECK(f.uploads == 1);
    CHECK(rec.frames[0].calls == 12); // the set-up and the queries above
    st = mt_gles_get_stats();
    CHECK(st.frames == 2);
    CHECK(st.first_frame == 2);
    CHECK(st.viewport[2] == 480);
    CHECK(st.misuses == 0);
    // presenting a renderbuffer target that is not GL_RENDERBUFFER_OES fails
    CHECK(e.send(context, "presentRenderbuffer:", {GL_FRAMEBUFFER_OES}) == 0);
    CHECK(mt_gles_get_stats().misuses == 1);
}

TEST_CASE("eagl: RGB565 drawables, other APIs, other drawables") {
    Env e;
    e.create_gl("EAGLColorFormatRGB565");
    const u32 out = e.alloc(4);
    e.call(mt_hle_glGetRenderbufferParameterivOES,
           {GL_RENDERBUFFER_OES, GL_RENDERBUFFER_GREEN_SIZE_OES, out});
    CHECK(e.get32(out) == 6);
    e.call(mt_hle_glGetRenderbufferParameterivOES,
           {GL_RENDERBUFFER_OES, GL_RENDERBUFFER_INTERNAL_FORMAT_OES, out});
    CHECK(e.get32(out) == GL_RGB565_OES);
    // OpenGL ES 2 is not emulated: nil
    CHECK(e.send(e.send(e.klass("EAGLContext"), "alloc"), "initWithAPI:", {2}) == 0);
    CHECK(logged("gl: -[EAGLContext initWithAPI:2]"));
    // a plain CALayer is no drawable
    const u32 context = e.send(e.send(e.klass("EAGLContext"), "alloc"), "initWithAPI:", {1});
    const u32 view = e.send(e.send(e.klass("UIView"), "alloc"),
                            "initWithFrame:", {fb(0), fb(0), fb(10), fb(10)});
    CHECK(e.send(context, "renderbufferStorage:fromDrawable:",
                 {GL_RENDERBUFFER_OES, e.send(view, "layer")}) == 0);
    CHECK(logged("not a CAEAGLLayer"));
    CHECK(mt_gles_get_stats().contexts == 2);
}

TEST_CASE("eagl: a new runtime starts with no GL state") {
    {
        Env e;
        e.create_gl();
        CHECK(mt_gles_get_stats().contexts == 1);
    }
    Env e;
    const mt_gles_stats st = mt_gles_get_stats();
    CHECK(st.contexts == 0);
    CHECK(st.calls == 0);
    CHECK(st.bound_framebuffer == 0);
}

// --- audio
// -----------------------------------------------------------------------------------------

TEST_CASE("audio: OpenAL names and the error state") {
    Env e;
    // no current context: AL_INVALID_OPERATION
    CHECK(e.call(mt_hle_alGetError, {}) == 0xA004);
    const u32 device = e.call(mt_hle_alcOpenDevice, {0});
    REQUIRE(device != 0);
    const u32 context = e.call(mt_hle_alcCreateContext, {device, 0});
    REQUIRE(context != 0);
    CHECK(e.call(mt_hle_alcCreateContext, {0x1234, 0}) == 0);
    CHECK(e.call(mt_hle_alcMakeContextCurrent, {context}) == 1);
    CHECK(e.call(mt_hle_alcGetCurrentContext, {}) == context);
    CHECK(e.call(mt_hle_alcGetContextsDevice, {context}) == device);
    CHECK(e.call(mt_hle_alGetError, {}) == 0);
    const u32 names = e.alloc(12);
    e.call(mt_hle_alGenBuffers, {2, names});
    e.call(mt_hle_alGenSources, {1, names + 8});
    CHECK(e.get32(names) == 1);
    CHECK(e.get32(names + 4) == 2);
    CHECK(e.get32(names + 8) == 1);
    const u32 pcm = e.alloc(200);
    e.call(mt_hle_alBufferData, {1, 0x1101 /* AL_FORMAT_MONO16 */, pcm, 200, 22050});
    e.call(mt_hle_alSourcei, {1, 0x1009 /* AL_BUFFER */, 1});
    e.call(mt_hle_alSourcePlay, {1});
    e.call(mt_hle_alListenerf, {0x100A /* AL_GAIN */, fb(0.5f)});
    CHECK(e.call(mt_hle_alGetError, {}) == 0);
    // the game's ResumeBGM plays source -1 (docs/re/audio.md): AL_INVALID_NAME, kept until read
    e.call(mt_hle_alSourcePlay, {0xFFFFFFFFu});
    e.call(mt_hle_alBufferData, {9, 0x1101, pcm, 200, 22050}); // the first error is kept
    CHECK(e.call(mt_hle_alGetError, {}) == 0xA001);
    CHECK(e.call(mt_hle_alGetError, {}) == 0);
    e.call(mt_hle_alBufferData, {2, 0x9999, pcm, 200, 22050});
    CHECK(e.call(mt_hle_alGetError, {}) == 0xA002);
    e.call(mt_hle_alSourcei, {1, 0x1009, 77});
    CHECK(e.call(mt_hle_alGetError, {}) == 0xA003);
    const mt_audio_stats st = mt_audio_get_stats();
    CHECK(st.buffers == 2);
    CHECK(st.buffers_filled == 1);
    CHECK(st.sources == 1);
    CHECK(st.source_plays == 1);
    CHECK(e.call(mt_hle_alcGetProcAddress, {device, e.bytes("alBufferDataStatic", 19)}) == 0);
    e.call(mt_hle_alDeleteSources, {1, names + 8});
    e.call(mt_hle_alDeleteBuffers, {2, names});
    CHECK(e.call(mt_hle_alGetError, {}) == 0);
    CHECK(mt_audio_get_stats().buffers == 0);
    CHECK(e.call(mt_hle_alcMakeContextCurrent, {0}) == 1);
    e.call(mt_hle_alcDestroyContext, {context});
    CHECK(e.call(mt_hle_alcCloseDevice, {device}) == 1);
    CHECK(e.call(mt_hle_alcCloseDevice, {device}) == 0);
}

TEST_CASE("audio: the audio session and system sounds") {
    Env e;
    CHECK(e.call(mt_hle_AudioSessionSetActive, {1}) == 0x21696E69u); // '!ini'
    CHECK(e.call(mt_hle_AudioSessionInitialize, {0, 0, 0x1001, 0}) == 0);
    CHECK(e.call(mt_hle_AudioSessionInitialize, {0, 0, 0x1001, 0}) == 0x696E6974u); // 'init'
    const u32 category = e.alloc(4);
    e.put32(category, 0x616D6269u); // 'ambi'
    CHECK(e.call(mt_hle_AudioSessionSetProperty, {0x61636174u /* 'acat' */, 4, category}) == 0);
    CHECK(e.call(mt_hle_AudioSessionSetProperty, {0x61636174u, 2, category}) ==
          static_cast<u32>(-50));
    CHECK(e.call(mt_hle_AudioSessionSetActive, {1}) == 0);
    e.call(mt_hle_AudioServicesPlaySystemSound, {0xFFF});
    const mt_audio_stats st = mt_audio_get_stats();
    CHECK(st.session_active == 1);
    CHECK(st.system_sounds == 1);
    // AVAudioSession: one shared instance
    const u32 session = e.send(e.klass("AVAudioSession"), "sharedInstance");
    CHECK(session != 0);
    CHECK(e.send(e.klass("AVAudioSession"), "sharedInstance") == session);
    CHECK(e.send(session, "setCategory:error:", {e.string("AVAudioSessionCategoryAmbient"), 0}) ==
          1);
}

// A PCM WAV: `rate` Hz, `channels` x 16 bits, `frames` frames, with a LIST chunk of odd size
// (padded) before "data".
std::vector<u8> wav(u32 rate, u32 channels, u32 frames, u32 tag = 1) {
    std::vector<u8> b;
    const auto put = [&b](const char *s) { b.insert(b.end(), s, s + 4); };
    const auto u16 = [&b](u32 v) { b.insert(b.end(), {u8(v), u8(v >> 8)}); };
    const auto u32le = [&b](u32 v) {
        b.insert(b.end(), {u8(v), u8(v >> 8), u8(v >> 16), u8(v >> 24)});
    };
    const u32 align = channels * 2, data = frames * align;
    put("RIFF");
    u32le(4 + 8 + 16 + 8 + 4 + 8 + data);
    put("WAVE");
    put("fmt ");
    u32le(16);
    u16(tag);
    u16(channels);
    u32le(rate);
    u32le(rate * align);
    u16(align);
    u16(16);
    put("LIST");
    u32le(3);
    b.insert(b.end(), {'a', 'b', 'c', 0});
    put("data");
    u32le(data);
    b.insert(b.end(), data, 0x11);
    return b;
}

void write_file(const fs::path &path, const std::vector<u8> &bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

TEST_CASE("audio: ExtAudioFile reads a WAV's format and length and gives silence") {
    Env e;
    write_file(test_dir() / "Documents" / "effect.wav", wav(22050, 1, 100));
    const u32 out = e.alloc(4);
    REQUIRE(e.call(mt_hle_ExtAudioFileOpenURL, {e.home_url("Documents/effect.wav"), out}) == 0);
    const u32 file = e.get32(out);
    REQUIRE(file != 0);
    const u32 size = e.alloc(4), asbd = e.alloc(40);
    e.put32(size, 40);
    REQUIRE(e.call(mt_hle_ExtAudioFileGetProperty, {file, 0x66666D74u /* 'ffmt' */, size, asbd}) ==
            0);
    double rate = 0;
    std::memcpy(&rate, e.cpu.mem + asbd, 8);
    CHECK(rate == 22050.0);
    CHECK(e.get32(asbd + 8) == 0x6C70636Du); // 'lpcm'
    CHECK(e.get32(asbd + 24) == 2);          // bytes per frame
    CHECK(e.get32(asbd + 28) == 1);          // channels
    CHECK(e.get32(asbd + 32) == 16);         // bits
    e.put32(size, 8);
    const u32 frames = e.alloc(8);
    REQUIRE(e.call(mt_hle_ExtAudioFileGetProperty, {file, 0x2366726Du /* '#frm' */, frames, 0}) ==
            static_cast<u32>(-50));
    REQUIRE(e.call(mt_hle_ExtAudioFileGetProperty, {file, 0x2366726Du, size, frames}) == 0);
    CHECK(e.get32(frames) == 100);
    e.put32(size, 39);
    CHECK(e.call(mt_hle_ExtAudioFileGetProperty, {file, 0x66666D74u, size, asbd}) ==
          static_cast<u32>(-66562));
    // the client format: 44.1 kHz stereo 16-bit, as Apple's MyGetOpenALAudioData asks
    const double client_rate = 44100.0;
    std::memcpy(e.cpu.mem + asbd, &client_rate, 8);
    e.put32(asbd + 8, 0x6C70636Du);
    e.put32(asbd + 12, 0xC);
    e.put32(asbd + 16, 4);
    e.put32(asbd + 20, 1);
    e.put32(asbd + 24, 4);
    e.put32(asbd + 28, 2);
    e.put32(asbd + 32, 16);
    REQUIRE(e.call(mt_hle_ExtAudioFileSetProperty, {file, 0x63666D74u /* 'cfmt' */, 40, asbd}) ==
            0);
    // read: 100 frames at 22.05 kHz are 200 at 44.1 kHz, as silence
    const u32 pcm = e.alloc(4000);
    std::memset(e.cpu.mem + pcm, 0xAB, 4000);
    const u32 list = e.alloc(16);
    e.put32(list, 1);
    e.put32(list + 4, 2);
    e.put32(list + 8, 4000);
    e.put32(list + 12, pcm);
    const u32 io_frames = e.alloc(4);
    e.put32(io_frames, 1000);
    REQUIRE(e.call(mt_hle_ExtAudioFileRead, {file, io_frames, list}) == 0);
    CHECK(e.get32(io_frames) == 200);
    CHECK(e.get32(list + 8) == 800);
    CHECK(e.cpu.mem[pcm] == 0);
    CHECK(e.cpu.mem[pcm + 799] == 0);
    CHECK(e.cpu.mem[pcm + 800] == 0xAB);
    e.put32(io_frames, 1000);
    e.put32(list + 8, 4000);
    REQUIRE(e.call(mt_hle_ExtAudioFileRead, {file, io_frames, list}) == 0);
    CHECK(e.get32(io_frames) == 0); // the end of the file
    CHECK(e.call(mt_hle_ExtAudioFileDispose, {file}) == 0);
    CHECK(e.call(mt_hle_ExtAudioFileDispose, {file}) == static_cast<u32>(-50));
    CHECK(mt_audio_get_stats().files_opened == 1);
}

TEST_CASE("audio: ExtAudioFile rejects missing, foreign and hostile files") {
    Env e;
    const fs::path docs = test_dir() / "Documents";
    write_file(docs / "float.wav", wav(22050, 1, 10, 3)); // IEEE float: not PCM
    std::vector<u8> truncated = wav(22050, 1, 10);
    truncated.resize(30); // inside the fmt chunk
    write_file(docs / "truncated.wav", truncated);
    std::vector<u8> huge = wav(22050, 1, 10);
    huge[19] = 0xFF; // fmt chunk size 0xFF000010: past the end
    write_file(docs / "huge.wav", huge);
    write_file(docs / "notwav.wav", std::vector<u8>(64, 'x'));
    std::vector<u8> short_data = wav(22050, 2, 50);
    short_data.resize(short_data.size() - 60); // a truncated data chunk: the frames that are there
    write_file(docs / "short.wav", short_data);
    const u32 out = e.alloc(4);
    const auto open = [&](const char *rest) {
        e.put32(out, 0);
        return e.call(mt_hle_ExtAudioFileOpenURL, {e.home_url(rest), out});
    };
    CHECK(open("Documents/missing.wav") == static_cast<u32>(-43));
    CHECK(open("Documents/../escape.wav") == static_cast<u32>(-43));
    CHECK(open("Documents/float.wav") == 0x7479703Fu);
    CHECK(open("Documents/truncated.wav") == 0x7479703Fu);
    CHECK(open("Documents/huge.wav") == 0x7479703Fu);
    CHECK(open("Documents/notwav.wav") == 0x7479703Fu);
    CHECK(e.call(mt_hle_ExtAudioFileOpenURL, {0, out}) == static_cast<u32>(-43));
    CHECK(e.call(mt_hle_ExtAudioFileOpenURL, {e.home_url("Documents/float.wav"), 0}) ==
          static_cast<u32>(-50));
    REQUIRE(open("Documents/short.wav") == 0);
    const u32 size = e.alloc(4), frames = e.alloc(8);
    e.put32(size, 8);
    REQUIRE(e.call(mt_hle_ExtAudioFileGetProperty, {e.get32(out), 0x2366726Du, size, frames}) == 0);
    CHECK(e.get32(frames) == 35); // (200 - 60) / 4
    const mt_audio_stats st = mt_audio_get_stats();
    CHECK(st.files_opened == 1);
    CHECK(st.files_failed == 7);
}

TEST_CASE("audio: AVAudioPlayer keeps the play state of a file that exists") {
    Env e;
    write_file(test_dir() / "Documents" / "music.mp3", std::vector<u8>(16, 0));
    const u32 missing =
        e.send(e.send(e.klass("AVAudioPlayer"), "alloc"),
               "initWithContentsOfURL:error:", {e.home_url("Documents/none.mp3"), 0});
    CHECK(missing == 0);
    const u32 player =
        e.send(e.send(e.klass("AVAudioPlayer"), "alloc"),
               "initWithContentsOfURL:error:", {e.home_url("Documents/music.mp3"), 0});
    REQUIRE(player != 0);
    e.send(player, "setNumberOfLoops:", {static_cast<u32>(-1)});
    e.send(player, "setVolume:", {fb(0.8f)});
    CHECK(e.send(player, "prepareToPlay") == 1);
    CHECK((e.send(player, "isPlaying") & 0xFFu) == 0);
    CHECK(e.send(player, "play") == 1);
    CHECK((e.send(player, "isPlaying") & 0xFFu) == 1);
    CHECK(mt_audio_get_stats().players_playing == 1);
    e.send(player, "stop");
    CHECK((e.send(player, "isPlaying") & 0xFFu) == 0);
    const mt_audio_stats st = mt_audio_get_stats();
    CHECK(st.players == 1);
    CHECK(st.players_playing == 0);
    mt_objc_release(&e.cpu, player);
}

// --- CommonCrypto --------------------------------------------------------------------------------

TEST_CASE("commoncrypto: CC_MD5 and CC_SHA1 match the RFC 1321 and FIPS 180 test vectors") {
    Env e;
    const u32 md = e.alloc(20);
    const auto md5 = [&](const std::string &text) {
        const u32 p = e.bytes(text.data(), static_cast<u32>(text.size()));
        CHECK(e.call(mt_hle_CC_MD5, {p, static_cast<u32>(text.size()), md}) == md);
        return e.hex(md, 16);
    };
    const auto sha1 = [&](const std::string &text) {
        const u32 p = e.bytes(text.data(), static_cast<u32>(text.size()));
        CHECK(e.call(mt_hle_CC_SHA1, {p, static_cast<u32>(text.size()), md}) == md);
        return e.hex(md, 20);
    };
    CHECK(md5("") == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(md5("abc") == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(md5("message digest") == "f96b697d7cb7938d525a2f31aaf161d0");
    CHECK(md5("12345678901234567890123456789012345678901234567890123456789012345678901234567890") ==
          "57edf4a22be3c955ac49da2e2107b67a");
    CHECK(sha1("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(sha1("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    CHECK(sha1(std::string(1000000, 'a')) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
    // 55, 56 and 64 bytes: the padding's edge cases
    CHECK(md5(std::string(55, 'a')) == "ef1772b6dff9a122358552954ad0df65");
    CHECK(md5(std::string(56, 'a')) == "3b0c8ac703f828b04c6c197006d17218");
    CHECK(sha1(std::string(64, 'a')) == "0098ba824b5c16427bd7a1122a5a442a25ec644d");
}

} // namespace
