// EAGLContext (#117, ADR 0038, maplethief/gles.h): the game's OpenGL ES 1.1 context, with
// exactly the selectors it sends (docs/re/host-surface.md, OpenGLES):
//
//   +alloc (NSObject's)  -initWithAPI:  +setCurrentContext:
//   -renderbufferStorage:fromDrawable:  -presentRenderbuffer:
//
// -[EAGLView createGLes] makes one context with kEAGLRenderingAPIOpenGLES1, makes it current,
// and gives its colour renderbuffer the storage of the view's CAEAGLLayer (the 480x320 bounds
// of the turned view, RGBA8, no retained backing). Every frame then ends in
// -presentRenderbuffer: from -[EAGLView flushLCD]. The GL state lives in gles.cpp; a context
// holds only its API (one context, one state: the game never makes a second one).
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#include "cf/cf_objects.h"
#include "foundation/foundation.h"
#include "gles/gles_internal.h"
#include "maplethief/boot.h"
#include "maplethief/diag.h"
#include "maplethief/widescreen.h"

namespace mt_gles {
namespace {

constexpr u32 k_api_gles1 = 1; // kEAGLRenderingAPIOpenGLES1

struct ContextState {
    u32 api;
};
const mt_objc_state_type k_context_state = {"EAGLContext", sizeof(ContextState), nullptr};

// The current context (retained, as EAGL does), per runtime.
struct Current {
    mt_objc *rt = nullptr;
    u32 binding = 0;
    u32 context = 0;
};
Current g_current;

u32 &current() {
    if (g_current.rt != mt_objc_current() || g_current.binding != mt_cf::binding()) {
        g_current = Current{mt_objc_current(), mt_cf::binding(), 0};
    }
    return g_current.context;
}

// -[EAGLContext initWithAPI:(EAGLRenderingAPI)api]: OpenGL ES 1.1 only (the game asks for
// nothing else); any other API releases the receiver and answers nil.
void context_init_with_api(mt_cpu *cpu) {
    const u32 self = mt_objc_self(cpu);
    mt_args a = mt_objc_args(cpu);
    const u32 api = mt_arg_u32(&a);
    if (api != k_api_gles1) {
        log("-[EAGLContext initWithAPI:%u]: only OpenGL ES 1.1 (1) is emulated: nil",
            static_cast<unsigned>(api));
        mt_objc_release(cpu, self);
        mt_ret_u32(cpu, 0);
        return;
    }
    auto *s = static_cast<ContextState *>(mt_objc_state(mt_objc_current(), self, &k_context_state));
    if (s != nullptr) {
        s->api = api;
    }
    context_created();
    mt_process_milestone("eagl-context", self, "-[EAGLContext initWithAPI:1] (OpenGL ES 1.1)");
    mt_ret_u32(cpu, self);
}

// +[EAGLContext setCurrentContext:(EAGLContext *)context] -> BOOL
void context_set_current(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const u32 context = mt_arg_u32(&a);
    u32 &cur = current();
    if (context != cur) {
        mt_objc_retain(cpu, context);
        const u32 old = cur;
        cur = context;
        mt_objc_release(cpu, old);
    }
    set_current_context(context);
    mt_tracef("gl", "+[EAGLContext setCurrentContext:0x%08X]", static_cast<unsigned>(context));
    mt_ret_u32(cpu, 1);
}

// -[EAGLContext renderbufferStorage:(NSUInteger)target fromDrawable:(id<EAGLDrawable>)drawable]
// -> BOOL: the bound renderbuffer gets the layer's bounds (scale 1) and the colour format of
// its drawableProperties (kEAGLColorFormatRGBA8 unless it says kEAGLColorFormatRGB565).
void context_renderbuffer_storage(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const u32 target = mt_arg_u32(&a), drawable = mt_arg_u32(&a);
    mt_objc *rt = mt_objc_current();
    const u32 layer_class = mt_objc_class_named(rt, "CAEAGLLayer");
    if (drawable == 0 || layer_class == 0 || mt_objc_is_kind_of(rt, drawable, layer_class) == 0) {
        log("-[EAGLContext renderbufferStorage:fromDrawable:0x%08X]: not a CAEAGLLayer: NO",
            static_cast<unsigned>(drawable));
        mt_ret_u32(cpu, 0);
        return;
    }
    // [drawable bounds] (a CGRect: objc_msgSend_stret into scratch space below sp)
    const u32 result = (cpu->r[MT_SP] - 0x40u) & ~7u;
    std::memset(cpu->mem + result, 0, 16);
    mt_objc_msg_stret(cpu, result, drawable, mt_objc_sel(rt, "bounds"), nullptr, 0);
    float rect[4];
    std::memcpy(rect, cpu->mem + result, sizeof rect);
    const auto pixels = [](float f) {
        return std::isfinite(f) && f > 0 && f < 4096 ? static_cast<u32>(std::lround(f)) : 0u;
    };
    u32 width = pixels(rect[2]);
    const u32 height = pixels(rect[3]);
    // Widescreen (#86, ADR 0090, maplethief/widescreen.h): the game's 480 x 320-point layer gets
    // a wider storage, so -[EAGLView setupView] sets the viewport and projection to it.
    const u32 wide = mt_widescreen_width();
    if (wide > MT_WIDESCREEN_NARROW && width == MT_WIDESCREEN_NARROW && height == 320u) {
        log("-[EAGLContext renderbufferStorage:fromDrawable:]: widescreen: %ux%u for the %ux%u "
            "layer",
            static_cast<unsigned>(wide), static_cast<unsigned>(height),
            static_cast<unsigned>(width), static_cast<unsigned>(height));
        width = wide;
    }

    // [drawable drawableProperties]: the colour format and retained backing
    u32 format = k_gl_rgba8;
    bool retained = false;
    const u32 props = mt_objc_send0(cpu, drawable, "drawableProperties");
    if (props != 0) {
        const u32 key_format = mt_ns::string_from_utf8(cpu, "EAGLDrawablePropertyColorFormat");
        const u32 key_retained =
            mt_ns::string_from_utf8(cpu, "EAGLDrawablePropertyRetainedBacking");
        const u32 color = mt_ns::dictionary_get(cpu, props, key_format);
        const u32 backing = mt_ns::dictionary_get(cpu, props, key_retained);
        mt_objc_release(cpu, key_format);
        mt_objc_release(cpu, key_retained);
        if (color != 0 && mt_ns::string_utf8(cpu, color) == "EAGLColorFormatRGB565") {
            format = k_gl_rgb565;
        }
        mt_cf::Number n{};
        retained = backing != 0 && mt_cf::number_value(cpu, backing, &n) &&
                   (n.is_float ? n.f != 0 : n.i != 0);
    }
    if (width == 0 || height == 0) {
        log("-[EAGLContext renderbufferStorage:fromDrawable:]: the layer's bounds are empty "
            "(%gx%g): NO",
            static_cast<double>(rect[2]), static_cast<double>(rect[3]));
        mt_ret_u32(cpu, 0);
        return;
    }
    mt_ret_u32(cpu, renderbuffer_storage(target, width, height, format, retained) ? 1u : 0u);
}

// -[EAGLContext presentRenderbuffer:(NSUInteger)target] -> BOOL
void context_present(mt_cpu *cpu) {
    const u32 self = mt_objc_self(cpu);
    mt_args a = mt_objc_args(cpu);
    const u32 target = mt_arg_u32(&a);
    if (self != current()) {
        log("-[EAGLContext presentRenderbuffer:]: 0x%08X is not the current context",
            static_cast<unsigned>(self));
    }
    mt_ret_u32(cpu, present(cpu, target) ? 1u : 0u);
}

const mt_objc_host_method k_context_methods[] = {
    {"initWithAPI:", context_init_with_api},
    {"renderbufferStorage:fromDrawable:", context_renderbuffer_storage},
    {"presentRenderbuffer:", context_present},
};
const mt_objc_host_method k_context_class_methods[] = {
    {"setCurrentContext:", context_set_current},
};

} // namespace

// EAGLContext's SDK instance: isa and its _private pointer.
const mt_objc_host_class k_eaglcontext = {
    "EAGLContext", "NSObject", 8, k_context_methods, 3, k_context_class_methods, 1};

} // namespace mt_gles
