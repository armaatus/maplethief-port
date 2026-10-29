// The synthetic guest of the OpenGL ES, EAGL and audio tests (#117 ADR 0038, #56 ADR 0040):
// a guest address space with the runtime's host classes, CF bound to it, a data directory, a
// view whose layer is a CAEAGLLayer, and helpers to call HLE imports with word arguments.
// Shared by gles_test.cpp and gles_fixed_test.cpp (each has its own copy: an anonymous
// namespace).
#ifndef MAPLETHIEF_TESTS_CPP_GLES_FIXTURE_H
#define MAPLETHIEF_TESTS_CPP_GLES_FIXTURE_H

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <random>
#include <string>
#include <vector>

#include "foundation/foundation.h"
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

    // The game's -[EAGLView createGLes] on a GlesTestView (480x320 unless told otherwise): a
    // context, framebuffer and renderbuffer, the renderbuffer's storage from the layer. Returns
    // the context.
    u32 create_gl(const char *color_format = "EAGLColorFormatRGBA8", u32 *layer_out = nullptr,
                  float width = 480, float height = 320) {
        const u32 view = send(send(klass("GlesTestView"), "alloc"),
                              "initWithFrame:", {fb(0), fb(0), fb(width), fb(height)});
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

} // namespace

#endif // MAPLETHIEF_TESTS_CPP_GLES_FIXTURE_H
