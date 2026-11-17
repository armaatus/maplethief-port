// UIKit (#54, ADR 0037) on a real guest address space with synthetic data only: the
// NIBArchive reader (valid archives and hostile ones: truncation, bad indices, trailing bytes),
// UIApplicationMain's launch from a synthetic MainWindow.nib (the delegate gets its launch
// callback with the nib's window), the view tree's geometry, UIView animations, touch delivery,
// controls, scrolling and paging, view controllers, and golden images of the compositor
// (tests/golden/data/uikit_*.png; MAPLETHIEF_UPDATE_GOLDEN=1 rewrites them).
#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

#include "cf/cf_objects.h"
#include "cg/cg_objects.h"
#include "foundation/foundation.h"
#include "golden.h"
#include "maplethief/cf.h"
#include "maplethief/cg.h"
#include "maplethief/diag.h"
#include "maplethief/hle/imports.h"
#include "maplethief/memory.h"
#include "maplethief/objc/host.h"
#include "maplethief/objc/objc.h"
#include "maplethief/uikit.h"
#include "uikit/nib.h"
#include "uikit/uikit_internal.h"

namespace {

using u32 = std::uint32_t;
using u8 = std::uint8_t;
namespace golden = maplethief::golden;
namespace nib = mt_ui::nib;

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

// --- a NIBArchive writer ---------------------------------------------------------------------

struct NibWriter {
    struct Val {
        std::string key;
        u8 type;
        std::vector<u8> payload;
    };
    struct Obj {
        std::string cls;
        std::vector<Val> vals;
    };
    std::vector<Obj> objs;

    u32 add(const char *cls) {
        objs.push_back(Obj{cls, {}});
        return static_cast<u32>(objs.size() - 1);
    }
    void raw(u32 obj, const char *key, u8 type, std::vector<u8> payload = {}) {
        objs[obj].vals.push_back(Val{key, type, std::move(payload)});
    }
    void object(u32 obj, const char *key, u32 target) {
        raw(obj, key, nib::k_object,
            {u8(target), u8(target >> 8), u8(target >> 16), u8(target >> 24)});
    }
    void boolean(u32 obj, const char *key, bool b) {
        raw(obj, key, b ? nib::k_true : nib::k_false);
    }
    void i8(u32 obj, const char *key, int v) { raw(obj, key, nib::k_i8, {u8(v)}); }
    void f32(u32 obj, const char *key, float f) {
        const u32 b = fb(f);
        raw(obj, key, nib::k_f32, {u8(b), u8(b >> 8), u8(b >> 16), u8(b >> 24)});
    }
    static void varint(std::vector<u8> &out, u32 v) {
        while (v >= 0x80) {
            out.push_back(u8(v & 0x7F));
            v >>= 7;
        }
        out.push_back(u8(v | 0x80));
    }
    void data(u32 obj, const char *key, const std::vector<u8> &bytes) {
        std::vector<u8> p;
        varint(p, static_cast<u32>(bytes.size()));
        p.insert(p.end(), bytes.begin(), bytes.end());
        raw(obj, key, nib::k_data, p);
    }
    // A geometry value: 0x06, then little-endian f32s.
    void floats(u32 obj, const char *key, std::initializer_list<float> fs) {
        std::vector<u8> bytes = {6};
        for (const float f : fs) {
            const u32 b = fb(f);
            bytes.insert(bytes.end(), {u8(b), u8(b >> 8), u8(b >> 16), u8(b >> 24)});
        }
        data(obj, key, bytes);
    }
    u32 string(const char *text) {
        const u32 s = add("NSString");
        data(s, "NS.bytes", std::vector<u8>(text, text + std::strlen(text)));
        return s;
    }
    u32 array(std::initializer_list<u32> items) { return array(std::vector<u32>(items)); }
    u32 array(const std::vector<u32> &items) {
        const u32 a = add("NSArray");
        for (const u32 i : items) {
            object(a, "UINibEncoderEmptyKey", i);
        }
        return a;
    }

    std::vector<u8> bytes() const {
        std::vector<std::string> keys, classes;
        auto index = [](std::vector<std::string> &v, const std::string &s) {
            for (u32 i = 0; i < v.size(); i++) {
                if (v[i] == s) {
                    return i;
                }
            }
            v.push_back(s);
            return static_cast<u32>(v.size() - 1);
        };
        std::vector<u8> objects, values, keytab, classtab;
        u32 nvalues = 0;
        for (const Obj &o : objs) {
            varint(objects, index(classes, o.cls));
            varint(objects, nvalues);
            varint(objects, static_cast<u32>(o.vals.size()));
            for (const Val &v : o.vals) {
                varint(values, index(keys, v.key));
                values.push_back(v.type);
                values.insert(values.end(), v.payload.begin(), v.payload.end());
                nvalues++;
            }
        }
        for (const std::string &k : keys) {
            varint(keytab, static_cast<u32>(k.size()));
            keytab.insert(keytab.end(), k.begin(), k.end());
        }
        for (const std::string &c : classes) {
            varint(classtab, static_cast<u32>(c.size() + 1));
            varint(classtab, 0);
            classtab.insert(classtab.end(), c.begin(), c.end());
            classtab.push_back(0);
        }
        std::vector<u8> out = {'N', 'I', 'B', 'A', 'r', 'c', 'h', 'i', 'v', 'e'};
        auto put = [&](u32 v) {
            out.insert(out.end(), {u8(v), u8(v >> 8), u8(v >> 16), u8(v >> 24)});
        };
        put(1);
        put(10);
        u32 offset = 10 + 40;
        const u32 counts[4] = {static_cast<u32>(objs.size()), static_cast<u32>(keys.size()),
                               nvalues, static_cast<u32>(classes.size())};
        const std::vector<u8> *tables[4] = {&objects, &keytab, &values, &classtab};
        for (int i = 0; i < 4; i++) {
            put(counts[i]);
            put(offset);
            offset += static_cast<u32>(tables[i]->size());
        }
        for (const auto *t : tables) {
            out.insert(out.end(), t->begin(), t->end());
        }
        return out;
    }
};

// A MainWindow.nib like the game's: a 320x480 white UIWindow holding a TestView (a
// UIClassSwapper from UIView), a TestDelegate (from UICustomObject), the outlets File's
// Owner.delegate and delegate.window, and the window visible. `more` adds subviews of the
// window after the TestView.
std::vector<u8> main_window_nib(const std::function<std::vector<u32>(NibWriter &)> &more = {}) {
    NibWriter w;
    const u32 root = w.add("NSObject");
    const u32 owner = w.add("UIProxyObject");
    w.object(owner, "UIProxiedObjectIdentifier", w.string("IBFilesOwner"));
    const u32 responder = w.add("UIProxyObject");
    w.object(responder, "UIProxiedObjectIdentifier", w.string("IBFirstResponder"));
    const u32 delegate = w.add("UIClassSwapper");
    w.object(delegate, "UIClassName", w.string("TestDelegate"));
    w.object(delegate, "UIOriginalClassName", w.string("UICustomObject"));
    const u32 white = w.add("UIColor");
    w.f32(white, "UIWhite", 1);
    w.f32(white, "UIAlpha", 1);
    const u32 tint = w.add("UIColor");
    w.f32(tint, "UIRed", 0.25f);
    w.f32(tint, "UIGreen", 0.5f);
    w.f32(tint, "UIBlue", 0.75f);
    w.f32(tint, "UIAlpha", 1);
    const u32 view = w.add("UIClassSwapper");
    w.object(view, "UIClassName", w.string("TestView"));
    w.object(view, "UIOriginalClassName", w.string("UIView"));
    w.floats(view, "UIBounds", {0, 0, 320, 480});
    w.floats(view, "UICenter", {160, 240});
    w.object(view, "UIBackgroundColor", tint);
    w.boolean(view, "UIOpaque", false);
    w.i8(view, "UIAutoresizingMask", 18);
    w.i8(view, "UISomethingNew", 1);
    const u32 window = w.add("UIWindow");
    w.floats(window, "UIBounds", {0, 0, 320, 480});
    w.floats(window, "UICenter", {160, 240});
    w.object(window, "UIBackgroundColor", white);
    std::vector<u32> subviews = {view};
    if (more) {
        for (const u32 s : more(w)) {
            subviews.push_back(s);
        }
    }
    w.object(window, "UISubviews", w.array(subviews));
    const u32 c1 = w.add("UIRuntimeOutletConnection");
    w.object(c1, "UILabel", w.string("delegate"));
    w.object(c1, "UISource", owner);
    w.object(c1, "UIDestination", delegate);
    const u32 c2 = w.add("UIRuntimeOutletConnection");
    w.object(c2, "UILabel", w.string("window"));
    w.object(c2, "UISource", delegate);
    w.object(c2, "UIDestination", window);
    w.object(root, "UINibTopLevelObjectsKey", w.array({owner, responder, delegate, window}));
    w.object(root, "UINibObjectsKey", w.array({owner, responder, delegate, window, view}));
    w.object(root, "UINibConnectionsKey", w.array({c1, c2}));
    w.object(root, "UINibVisibleWindowsKey", w.array({window}));
    return w.bytes();
}

// --- test classes ----------------------------------------------------------------------------

struct Record {
    u32 window_at_launch = 0, launch_app = 0, launches = 0, became_active = 0, resigned = 0;
    u32 window = 0;
    u32 coder_inits = 0;
    std::vector<std::string> touches; // "began 0x... (x, y)"
    // mt_uikit_touch_delivering inside each delivery (#194): "finger/phase".
    std::vector<std::string> delivering;
    // Per touch callback: the touch's tapCount, and the counts of the event's touchesForView:
    // self, touchesForWindow: (self's window) and allTouches.
    std::vector<u32> tap_counts, for_view, for_window, all;
    std::vector<std::string> actions; // "down:" ...
    u32 scrolls = 0, appeared = 0;
    bool draw = false;
};
Record g_rec;

void delegate_set_window(mt_cpu *cpu) { g_rec.window = cpu->r[2]; }
void delegate_window(mt_cpu *cpu) { cpu->r[0] = g_rec.window; }
void delegate_did_finish(mt_cpu *cpu) {
    g_rec.launches++;
    g_rec.launch_app = cpu->r[2];
    g_rec.window_at_launch = g_rec.window;
    cpu->r[0] = 1;
}
void delegate_active(mt_cpu *) { g_rec.became_active++; }
void delegate_resign(mt_cpu *) { g_rec.resigned++; }
const mt_objc_host_method k_delegate_methods[] = {
    {"setWindow:", delegate_set_window},
    {"window", delegate_window},
    {"application:didFinishLaunchingWithOptions:", delegate_did_finish},
    {"applicationDidBecomeActive:", delegate_active},
    {"applicationWillResignActive:", delegate_resign},
};
const mt_objc_host_class k_test_delegate = {"TestDelegate", "NSObject", 4, k_delegate_methods, 5,
                                            nullptr,        0};

// TestView: records touches with -[UITouch locationInView:self] and draws a rectangle in
// -drawRect: when asked.
u32 g_scratch = 0; // a guest block for stret results

void record_touch(mt_cpu *cpu, const char *phase) {
    const u32 self = cpu->r[0], touches = cpu->r[2], event = cpu->r[3];
    mt_objc *rt = mt_objc_current();
    const std::vector<u32> *items = mt_ns::set_items(cpu, touches);
    if (items == nullptr || items->empty()) {
        return;
    }
    const u32 touch = items->front();
    const u32 args[1] = {self};
    std::uint32_t d_finger = 0, d_phase = 0;
    if (mt_uikit_touch_delivering(&d_finger, &d_phase) != 0) {
        g_rec.delivering.push_back(std::to_string(d_finger) + "/" + std::to_string(d_phase));
    }
    mt_objc_msg_stret(cpu, g_scratch, touch, mt_objc_sel(rt, "locationInView:"), args, 1);
    float p[2] = {};
    std::memcpy(p, cpu->mem + g_scratch, 8);
    const u32 view = mt_objc_msg(cpu, touch, mt_objc_sel(rt, "view"), nullptr, 0);
    g_rec.tap_counts.push_back(mt_objc_msg(cpu, touch, mt_objc_sel(rt, "tapCount"), nullptr, 0));
    auto count = [&](const char *sel, u32 arg) {
        const u32 set = mt_objc_msg(cpu, event, mt_objc_sel(rt, sel), &arg, arg != 0 ? 1 : 0);
        return mt_objc_msg(cpu, set, mt_objc_sel(rt, "count"), nullptr, 0);
    };
    const u32 window = mt_objc_msg(cpu, self, mt_objc_sel(rt, "window"), nullptr, 0);
    g_rec.for_view.push_back(count("touchesForView:", self));
    g_rec.for_window.push_back(count("touchesForWindow:", window));
    g_rec.all.push_back(count("allTouches", 0));
    char line[96] = {};
    std::snprintf(line, sizeof line, "%s %s (%g, %g)%s", phase,
                  mt_objc_class_name(rt, mt_objc_class_of(rt, self)), p[0], p[1],
                  view == self ? "" : " (forwarded)");
    g_rec.touches.emplace_back(line);
}
void view_began(mt_cpu *cpu) { record_touch(cpu, "began"); }
void view_moved(mt_cpu *cpu) { record_touch(cpu, "moved"); }
void view_ended(mt_cpu *cpu) { record_touch(cpu, "ended"); }
void view_cancelled(mt_cpu *cpu) { record_touch(cpu, "cancelled"); }
void view_init_with_coder(mt_cpu *cpu) {
    g_rec.coder_inits++;
    mt_objc *rt = mt_objc_current();
    const u32 args[1] = {cpu->r[2]};
    cpu->r[0] = mt_objc_msg_super(cpu, cpu->r[0], mt_objc_class_named(rt, "TestView"),
                                  mt_objc_sel(rt, "initWithCoder:"), args, 1);
}
// -drawRect: a 10x10 orange square at (5, 5) of the bounds.
void view_draw_rect(mt_cpu *cpu) {
    if (!g_rec.draw) {
        return;
    }
    mt_cg::Canvas *canvas = nullptr;
    mt_cg::Surface s;
    REQUIRE(mt_cg::context_canvas(cpu, mt_cg::current_context(cpu), &canvas, &s));
    canvas->state().fill = mt_cg::Color{1, 0.5f, 0, 1};
    canvas->fill_rect(s, mt_cg::Rect{5, 5, 10, 10});
}
const mt_objc_host_method k_view_methods[] = {
    {"touchesBegan:withEvent:", view_began},  {"touchesMoved:withEvent:", view_moved},
    {"touchesEnded:withEvent:", view_ended},  {"touchesCancelled:withEvent:", view_cancelled},
    {"initWithCoder:", view_init_with_coder}, {"drawRect:", view_draw_rect},
};
const mt_objc_host_class k_test_view = {"TestView", "UIView", 46, k_view_methods, 6, nullptr, 0};

// TestGLView: a view whose layer is a CAEAGLLayer (the GL slot), like EAGLView.
void gl_layer_class(mt_cpu *cpu) {
    cpu->r[0] = mt_objc_class_named(mt_objc_current(), "CAEAGLLayer");
}
const mt_objc_host_method k_gl_class_methods[] = {{"layerClass", gl_layer_class}};
const mt_objc_host_class k_test_gl_view = {"TestGLView",       "UIView", 46, nullptr, 0,
                                           k_gl_class_methods, 1};

// TestTarget: button actions and scroll view delegate callbacks.
void target_down(mt_cpu *) { g_rec.actions.emplace_back("down:"); }
void target_up(mt_cpu *) { g_rec.actions.emplace_back("up:"); }
void target_up_outside(mt_cpu *) { g_rec.actions.emplace_back("upOutside"); }
void target_did_scroll(mt_cpu *) { g_rec.scrolls++; }
void target_appeared(mt_cpu *) { g_rec.appeared++; }
const mt_objc_host_method k_target_methods[] = {
    {"down:", target_down},
    {"up:", target_up},
    {"upOutside", target_up_outside},
    {"scrollViewDidScroll:", target_did_scroll},
};
const mt_objc_host_class k_test_target = {"TestTarget", "NSObject", 4, k_target_methods, 4,
                                          nullptr,      0};
const mt_objc_host_method k_test_vc_methods[] = {{"viewDidAppear:", target_appeared}};
const mt_objc_host_class k_test_vc = {
    "TestViewController", "UIViewController", 152, k_test_vc_methods, 1, nullptr, 0};

double g_now = 1000;
double test_clock(void *) { return g_now; }

// A guest address space with the runtime's host classes, CF bound to it, the test classes and
// UIKit's clock on g_now.
struct Ui {
    mt_mem *mem = nullptr;
    mt_cpu cpu{};
    mt_stack stack{};
    mt_objc *rt = nullptr;
    u32 pool = 0;

    Ui() {
        g_log.clear();
        g_rec = Record();
        g_now = 1000;
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
        for (const mt_objc_host_class *c :
             {&k_test_delegate, &k_test_view, &k_test_gl_view, &k_test_target, &k_test_vc}) {
            REQUIRE(mt_objc_add_host_class(rt, c, 0, 0) == 0);
        }
        mt_cf_start(&cpu, rt);
        pool = mt_objc_pool_push(rt);
        g_scratch = mt_heap_calloc(mem, 1, 64);
        mt_ui::state(&cpu).config.clock = test_clock;
    }
    ~Ui() {
        mt_objc_pool_pop(rt, pool);
        mt_objc_set_current(nullptr);
        mt_objc_destroy(rt);
        mt_mem_destroy(mem);
    }
    Ui(const Ui &) = delete;
    Ui &operator=(const Ui &) = delete;

    u32 klass(const char *name) const { return mt_objc_class_named(rt, name); }
    const char *class_name(u32 obj) const {
        return mt_objc_class_name(rt, mt_objc_class_of(rt, obj));
    }
    u32 send(u32 self, const char *sel, std::initializer_list<u32> args = {}) {
        const std::vector<u32> v(args);
        return mt_objc_msg(&cpu, self, mt_objc_sel(rt, sel), v.data(), static_cast<u32>(v.size()));
    }
    std::vector<float> send_stret(u32 self, const char *sel, u32 floats,
                                  std::initializer_list<u32> args = {}) {
        const std::vector<u32> v(args);
        std::memset(cpu.mem + g_scratch, 0, 64);
        mt_objc_msg_stret(&cpu, g_scratch, self, mt_objc_sel(rt, sel), v.data(),
                          static_cast<u32>(v.size()));
        std::vector<float> out(floats);
        std::memcpy(out.data(), cpu.mem + g_scratch, 4 * floats);
        return out;
    }
    std::vector<float> frame(u32 view) { return send_stret(view, "frame", 4); }
    u32 string(const char *text) { return mt_ns::string_from_utf8(&cpu, text); }
    u32 view(const char *cls, float x, float y, float w, float h) {
        const u32 obj = send(klass(cls), "alloc");
        return send(obj, "initWithFrame:", {fb(x), fb(y), fb(w), fb(h)});
    }
    u32 color(float r, float g, float b, float a) {
        return send(klass("UIColor"),
                    "colorWithRed:green:blue:alpha:", {fb(r), fb(g), fb(b), fb(a)});
    }
    // An autoreleased UIImage of the synthetic test pattern (straight RGBA).
    u32 pattern_image(int w, int h) {
        const golden::Image p = golden::test_pattern(w, h);
        auto pixels = std::make_shared<std::vector<u8>>(p.bytes());
        const u32 cg =
            mt_cg::image_create_host(&cpu, static_cast<u32>(w), static_cast<u32>(h), 32,
                                     mt_cg::k_alpha_last | mt_cg::k_byte_order_32_big, pixels);
        REQUIRE(cg != 0);
        const u32 img = mt_cg::uiimage_create(&cpu, cg);
        mt_objc_release(&cpu, cg);
        return img;
    }
    mt_ui::View &v(u32 obj) {
        mt_ui::View *p = mt_ui::view_of(&cpu, obj);
        REQUIRE(p != nullptr);
        return *p;
    }

    // An application with a visible 320x480 window and, in it, a view turned to landscape like
    // the EAGLView (its local space is the 480x320 screen); returns that view.
    u32 screen(const char *root_class = "UIView") {
        mt_ui::State &st = mt_ui::state(&cpu);
        st.app = send(send(klass("UIApplication"), "alloc"), "init");
        const u32 window = view("UIWindow", 0, 0, 320, 480);
        st.windows.push_back(window);
        const u32 root = view(root_class, 0, 0, 320, 480);
        send(root, "setTransform:", {fb(0), fb(-1), fb(1), fb(0), fb(0), fb(0)});
        send(root, "setBounds:", {fb(0), fb(0), fb(480), fb(320)});
        send(window, "addSubview:", {root});
        mt_objc_release(&cpu, root);
        return root;
    }
    void touch(u32 finger, u32 phase, float x, float y) {
        REQUIRE(mt_uikit_touch(finger, phase, x, y) == 0);
    }
    void turn(double dt = 0.0) {
        g_now += dt;
        mt_uikit_turn(&cpu, g_now);
    }
    golden::Image composite(u32 mode, int width, int height,
                            const mt_uikit_gl_frame *gl = nullptr) {
        golden::Image img(width, height);
        REQUIRE(mt_uikit_composite(&cpu, g_now, mode, gl, img.bytes().data(),
                                   static_cast<u32>(width), static_cast<u32>(height),
                                   static_cast<u32>(width) * 4) == 0);
        return img;
    }
};

std::string rect(const std::vector<float> &f) {
    std::string s;
    for (const float x : f) {
        s += (s.empty() ? "" : ", ") + std::to_string(static_cast<int>(std::lround(x)));
    }
    return "(" + s + ")";
}

void check_golden(const golden::Image &img, const char *name) {
    const std::filesystem::path path =
        std::filesystem::path(MT_GOLDEN_DATA_DIR) / (std::string(name) + ".png");
    const auto r = golden::check_golden(img, path, std::string("uikit/") + name, {2, 0.002});
    CHECK_MESSAGE(r.passed, r.message);
}

// A synthetic bundle directory holding one file.
struct Bundle {
    std::filesystem::path dir;
    std::string path;
    explicit Bundle(const std::vector<u8> &nib_bytes, const char *name = "MainWindow.nib") {
        // One directory per process: CTest runs the test cases in parallel (#153).
        dir = std::filesystem::temp_directory_path() /
              ("maplethief-uikit-test-bundle-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        std::ofstream(dir / name, std::ios::binary)
            .write(reinterpret_cast<const char *>(nib_bytes.data()),
                   static_cast<std::streamsize>(nib_bytes.size()));
        mt_cf_config cfg = mt_cf_config_default();
        path = dir.string();
        cfg.bundle_dir = path.c_str();
        mt_cf_configure(&cfg);
    }
    ~Bundle() {
        mt_cf_configure(nullptr);
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    Bundle(const Bundle &) = delete;
    Bundle &operator=(const Bundle &) = delete;
};

} // namespace

// --- the NIBArchive reader ---------------------------------------------------------------------

TEST_CASE("nib: a synthetic MainWindow.nib parses into its tables") {
    const std::vector<u8> bytes = main_window_nib();
    nib::Archive a;
    std::string error;
    REQUIRE_MESSAGE(nib::parse(bytes.data(), bytes.size(), &a, &error), error);
    CHECK(a.format_version == 1);
    CHECK(a.coder_version == 10);
    CHECK(a.class_name(0) == "NSObject");
    const nib::Value *top = a.find(0, "UINibTopLevelObjectsKey");
    REQUIRE(top != nullptr);
    CHECK(top->type == nib::k_object);
    CHECK(a.elements(top->object).size() == 4);
    // The view's geometry (0x06 + f32s) and its other values.
    u32 view = 0;
    for (u32 i = 0; i < a.objects.size(); i++) {
        if (a.class_name(i) == "UIClassSwapper" && a.find(i, "UIBounds") != nullptr) {
            view = i;
        }
    }
    REQUIRE(view != 0);
    float f[4] = {};
    CHECK(a.floats(*a.find(view, "UIBounds"), f, 4));
    CHECK(f[2] == 320);
    CHECK(f[3] == 480);
    CHECK_FALSE(a.floats(*a.find(view, "UIBounds"), f, 2)); // the wrong count
    double n = 0;
    CHECK(a.number(*a.find(view, "UIAutoresizingMask"), &n));
    CHECK(n == 18);
    CHECK(a.number(*a.find(view, "UIOpaque"), &n));
    CHECK(n == 0);
    CHECK(a.find(view, "NoSuchKey") == nullptr);
    CHECK(a.find(9999, "UIBounds") == nullptr);
    CHECK(a.class_name(9999).empty());
}

TEST_CASE("nib: every truncation of the file is refused with a reason") {
    const std::vector<u8> bytes = main_window_nib();
    for (std::size_t n = 0; n < bytes.size(); n++) {
        nib::Archive a;
        std::string error;
        CHECK_FALSE(nib::parse(bytes.data(), n, &a, &error));
        CHECK_FALSE(error.empty());
    }
    // A trailing byte is refused too.
    std::vector<u8> longer = bytes;
    longer.push_back(0);
    nib::Archive a;
    std::string error;
    CHECK_FALSE(nib::parse(longer.data(), longer.size(), &a, &error));
    CHECK(error.find("trailing") != std::string::npos);
}

TEST_CASE("nib: bad magic, counts, offsets, indices, varints and class names are refused") {
    auto parse_error = [](const std::vector<u8> &bytes) {
        nib::Archive a;
        std::string error;
        const bool ok = nib::parse(bytes.data(), bytes.size(), &a, &error);
        CHECK_FALSE(ok);
        return error;
    };
    auto put32 = [](std::vector<u8> &b, std::size_t at, u32 v) {
        b[at] = u8(v);
        b[at + 1] = u8(v >> 8);
        b[at + 2] = u8(v >> 16);
        b[at + 3] = u8(v >> 24);
    };
    const std::vector<u8> good = main_window_nib();
    {
        std::vector<u8> b = good;
        b[0] = 'X';
        CHECK(parse_error(b).find("magic") != std::string::npos);
    }
    {
        std::vector<u8> b = good;
        put32(b, 18, 0x10000000u); // object count
        CHECK(parse_error(b).find("do not fit") != std::string::npos);
    }
    {
        std::vector<u8> b = good;
        put32(b, 22, 51); // objects table offset
        CHECK(parse_error(b).find("contiguous") != std::string::npos);
    }
    {
        // An object value pointing past the objects table.
        NibWriter w;
        const u32 root = w.add("NSObject");
        w.object(root, "x", 7);
        CHECK(parse_error(w.bytes()).find("object index 7 out of range") != std::string::npos);
    }
    {
        // An unknown value type.
        NibWriter w;
        w.raw(w.add("NSObject"), "x", 42);
        CHECK(parse_error(w.bytes()).find("unknown type 42") != std::string::npos);
    }
    {
        // A data value longer than the file.
        NibWriter w;
        w.raw(w.add("NSObject"), "x", nib::k_data, {0xFF});
        CHECK(parse_error(w.bytes()).find("truncated data value") != std::string::npos);
    }
    {
        // A varint of six bytes (the object's class index).
        NibWriter w;
        w.add("NSObject");
        std::vector<u8> b = w.bytes();
        // objects table at 50: class varint 0x80 -> replace with 0 0 0 0 0 0x80 (6 bytes).
        b[50] = 0x00;
        b.insert(b.begin() + 51, {0x00, 0x00, 0x00, 0x00, 0x80});
        CHECK(parse_error(b).find("varint") != std::string::npos);
    }
    {
        // A class name without its NUL.
        NibWriter w;
        w.add("NSObject");
        std::vector<u8> b = w.bytes();
        b.back() = 'x';
        CHECK(parse_error(b).find("NUL-terminated") != std::string::npos);
    }
    {
        // A class fallback out of range: one fallback 9 for the only class.
        NibWriter w;
        w.add("NSObject");
        std::vector<u8> b = w.bytes();
        const std::size_t classes = b.size() - (1 + 1 + 9); // length, fallbacks, "NSObject\0"
        b[classes + 1] = 0x81;
        b.insert(b.begin() + static_cast<std::ptrdiff_t>(classes) + 2, {9, 0, 0, 0});
        CHECK(parse_error(b).find("fallback 9 out of range") != std::string::npos);
    }
    {
        // A key index out of range.
        NibWriter w;
        w.i8(w.add("NSObject"), "x", 1);
        std::vector<u8> b = w.bytes();
        // the values table: key varint, type, payload; the key varint is 0x80 (key 0) -> 0x85
        const u32 values_offset = static_cast<u32>(b[38]) | static_cast<u32>(b[39]) << 8;
        b[values_offset] = 0x85;
        CHECK(parse_error(b).find("key index 5 out of range") != std::string::npos);
    }
}

// --- UIApplicationMain's launch ------------------------------------------------------------------

TEST_CASE("launch: the delegate gets -application:didFinishLaunchingWithOptions: with the nib's "
          "window") {
    Bundle bundle(main_window_nib());
    Ui ui;
    REQUIRE(mt_ui::launch(&ui.cpu, nullptr) == nullptr);
    const u32 app = ui.send(ui.klass("UIApplication"), "sharedApplication");
    REQUIRE(app != 0);
    const u32 delegate = ui.send(app, "delegate");
    CHECK(std::string(ui.class_name(delegate)) == "TestDelegate");
    // The outlet delegate.window was connected before the launch callback ran.
    CHECK(g_rec.launches == 1);
    CHECK(g_rec.launch_app == app);
    REQUIRE(g_rec.window_at_launch != 0);
    CHECK(std::string(ui.class_name(g_rec.window_at_launch)) == "UIWindow");
    CHECK(rect(ui.frame(g_rec.window_at_launch)) == "(0, 0, 320, 480)");
    CHECK(g_rec.became_active == 1);
    // The window is visible and holds the TestView made by -initWithCoder: from the nib.
    CHECK(ui.send(app, "keyWindow") == g_rec.window_at_launch);
    const u32 subviews = ui.send(g_rec.window_at_launch, "subviews");
    REQUIRE(ui.send(subviews, "count") == 1);
    const u32 view = ui.send(subviews, "objectAtIndex:", {0});
    CHECK(std::string(ui.class_name(view)) == "TestView");
    CHECK(g_rec.coder_inits == 1);
    CHECK(ui.send(view, "superview") == g_rec.window_at_launch);
    CHECK(ui.send(view, "window") == g_rec.window_at_launch);
    CHECK(rect(ui.frame(view)) == "(0, 0, 320, 480)");
    const mt_ui::View &v = ui.v(view);
    CHECK_FALSE(v.opaque);
    CHECK(v.autoresizing == 18u);
    CHECK(v.background_color.r == doctest::Approx(0.25));
    CHECK(v.background_color.b == doctest::Approx(0.75));
    CHECK(ui.v(g_rec.window_at_launch).background_color.g == doctest::Approx(1));
    // A nib key UIView does not read is logged, not fatal.
    CHECK(logged("nib key UISomethingNew"));
    // The lifecycle reaches the delegate.
    CHECK(mt_uikit_lifecycle_event(&ui.cpu, MT_UIKIT_WILL_RESIGN_ACTIVE) == 1);
    CHECK(g_rec.resigned == 1);
    CHECK(mt_uikit_lifecycle_event(&ui.cpu, MT_UIKIT_WILL_TERMINATE) == 0); // not implemented
    CHECK(mt_uikit_lifecycle_event(&ui.cpu, 99) == 0);
}

TEST_CASE("launch: a missing or broken main nib fails with the reason") {
    {
        Bundle bundle({1, 2, 3}, "Other.nib");
        Ui ui;
        CHECK(std::string(mt_ui::launch(&ui.cpu, nullptr)) ==
              "UIApplicationMain: the main nib could not be loaded");
        CHECK(logged("MainWindow.nib is not in the bundle"));
    }
    {
        std::vector<u8> bytes = main_window_nib();
        bytes.resize(bytes.size() / 2);
        Bundle bundle(bytes);
        Ui ui;
        CHECK(mt_ui::launch(&ui.cpu, nullptr) != nullptr);
        std::string why;
        for (const auto &l : g_log) {
            if (l.find("UIApplicationMain: ") != std::string::npos) {
                why = l;
            }
        }
        CHECK_MESSAGE(why.find("MainWindow.nib: ") != std::string::npos, why);
        CHECK_MESSAGE(why.find("do not fit the file") != std::string::npos, why);
    }
}

TEST_CASE("launch: a nib nested deeper than 64 objects or referring back to itself gives nil, "
          "logged (#413)") {
    // A chain of 70 UIViews, each the only subview of the one before: -initWithCoder: nests
    // once a level, and the 64th level under the window is refused, so 63 are made.
    {
        Bundle bundle(main_window_nib([](NibWriter &w) {
            u32 inner = w.add("UIView");
            for (int i = 1; i < 70; i++) {
                const u32 outer = w.add("UIView");
                w.object(outer, "UISubviews", w.array({inner}));
                inner = outer;
            }
            return std::vector<u32>{inner};
        }));
        Ui ui;
        REQUIRE(mt_ui::launch(&ui.cpu, nullptr) == nullptr);
        REQUIRE(g_rec.window_at_launch != 0);
        const u32 subviews = ui.send(g_rec.window_at_launch, "subviews");
        REQUIRE(ui.send(subviews, "count") == 2u);
        u32 view = ui.send(subviews, "objectAtIndex:", {1});
        int levels = 0;
        while (view != 0) {
            levels++;
            const u32 below = ui.send(view, "subviews");
            view = ui.send(below, "count") != 0 ? ui.send(below, "objectAtIndex:", {0}) : 0u;
        }
        CHECK(levels == 63);
        CHECK(logged("refers back to itself or nests too deep: nil"));
    }
    // A view whose subviews array holds the view itself, and an array that holds itself.
    {
        Bundle bundle(main_window_nib([](NibWriter &w) {
            const u32 view = w.add("UIView");
            const u32 loop = w.add("NSArray");
            w.object(loop, "UINibEncoderEmptyKey", view);
            w.object(view, "UISubviews", loop);
            const u32 other = w.add("UIView");
            const u32 self_array = w.add("NSArray");
            w.object(self_array, "UINibEncoderEmptyKey", self_array);
            w.object(other, "UISubviews", self_array);
            return std::vector<u32>{view, other};
        }));
        Ui ui;
        REQUIRE(mt_ui::launch(&ui.cpu, nullptr) == nullptr);
        REQUIRE(g_rec.window_at_launch != 0);
        const u32 subviews = ui.send(g_rec.window_at_launch, "subviews");
        REQUIRE(ui.send(subviews, "count") == 3u);
        for (const u32 i : {1u, 2u}) {
            CAPTURE(i);
            const u32 view = ui.send(subviews, "objectAtIndex:", {i});
            REQUIRE(view != 0u);
            CHECK(ui.send(ui.send(view, "subviews"), "count") == 0u);
        }
        CHECK(logged("refers back to itself or nests too deep: nil"));
    }
}

// --- views ---------------------------------------------------------------------------------

TEST_CASE("views: frames, bounds, centres, transforms and the tree") {
    Ui ui;
    const u32 root = ui.screen();
    // The turned root: its frame in the window is portrait, its bounds landscape.
    CHECK(rect(ui.frame(root)) == "(0, 0, 320, 480)");
    CHECK(rect(ui.send_stret(root, "bounds", 4)) == "(0, 0, 480, 320)");
    CHECK(rect(ui.send_stret(root, "center", 2)) == "(160, 240)");
    float gl_rect[4] = {};
    CHECK(mt_uikit_gl_rect(gl_rect, nullptr, nullptr) == 0); // no GL view yet
    // A subview in landscape coordinates lands on the screen where it says.
    const u32 a = ui.view("UIView", 10, 20, 100, 50);
    ui.send(root, "addSubview:", {a});
    CHECK(mt_objc_retain_count(ui.rt, a) == 2);
    mt_objc_release(&ui.cpu, a);
    const mt_cg::Affine m =
        mt_ui::local_to_screen(&ui.cpu, ui.v(a)).then(mt_ui::screen_from_window());
    const mt_cg::Point p = m.apply({0, 0});
    CHECK(p.x == doctest::Approx(10));
    CHECK(p.y == doctest::Approx(20));
    // setFrame: / setCenter: / setBounds:, tag, hidden, alpha.
    ui.send(a, "setFrame:", {fb(30), fb(40), fb(60), fb(70)});
    CHECK(rect(ui.frame(a)) == "(30, 40, 60, 70)");
    CHECK(rect(ui.send_stret(a, "center", 2)) == "(60, 75)");
    ui.send(a, "setCenter:", {fb(100), fb(100)});
    CHECK(rect(ui.frame(a)) == "(70, 65, 60, 70)");
    ui.send(a, "setTag:", {7});
    CHECK(ui.send(a, "tag") == 7);
    ui.send(a, "setHidden:", {1});
    CHECK(ui.send(a, "isHidden") == 1);
    ui.send(a, "setAlpha:", {fb(0.5f)});
    CHECK(mt_f32(ui.send(a, "alpha")) == doctest::Approx(0.5));
    // Re-parenting and removal (the superview's reference is released).
    const u32 b = ui.view("UIView", 0, 0, 10, 10);
    ui.send(a, "addSubview:", {b});
    ui.send(root, "addSubview:", {b});
    CHECK(ui.send(b, "superview") == root);
    CHECK(mt_objc_retain_count(ui.rt, b) == 2);
    const u32 inner_pool = mt_objc_pool_push(ui.rt); // -subviews arrays are autoreleased
    CHECK(ui.send(ui.send(a, "subviews"), "count") == 0);
    CHECK(ui.send(ui.send(root, "subviews"), "count") == 2);
    mt_objc_pool_pop(ui.rt, inner_pool);
    ui.send(b, "removeFromSuperview");
    CHECK(mt_objc_retain_count(ui.rt, b) == 1);
    CHECK(ui.send(b, "superview") == 0);
    CHECK(ui.send(b, "window") == 0);
    mt_objc_release(&ui.cpu, b);
    // Not a view: ignored with a log line.
    ui.send(root, "addSubview:", {ui.string("not a view")});
    CHECK(logged("is no UIView"));
}

TEST_CASE("views: begin/commit animations move presentation values, not model values") {
    Ui ui;
    const u32 root = ui.screen();
    const u32 a = ui.view("UIView", 0, 0, 100, 100);
    ui.send(root, "addSubview:", {a});
    mt_objc_release(&ui.cpu, a);
    const u32 cls = ui.klass("UIView");
    ui.send(cls, "beginAnimations:context:", {0, 0});
    ui.send(cls, "setAnimationDuration:", {0, 0}); // replaced below
    mt_cpu &cpu = ui.cpu;
    {
        // +setAnimationDuration:(double) takes two words.
        const double d = 1.0, delay = 0.5;
        u32 w[2] = {};
        std::memcpy(w, &d, 8);
        ui.send(cls, "setAnimationDuration:", {w[0], w[1]});
        std::memcpy(w, &delay, 8);
        ui.send(cls, "setAnimationDelay:", {w[0], w[1]});
    }
    ui.send(cls, "setAnimationCurve:", {3}); // linear
    ui.send(a, "setAlpha:", {fb(0)});
    ui.send(a, "setCenter:", {fb(150), fb(50)});
    ui.send(cls, "commitAnimations");
    // The model values change at once.
    CHECK(mt_f32(ui.send(a, "alpha")) == 0);
    CHECK(rect(ui.send_stret(a, "center", 2)) == "(150, 50)");
    const mt_ui::View &v = ui.v(a);
    CHECK(mt_uikit_animation_count(g_now) == 2);
    // Before the delay: the old values; halfway: half; after: the new ones.
    CHECK(mt_ui::presentation(v, g_now + 0.25).alpha == doctest::Approx(1));
    CHECK(mt_ui::presentation(v, g_now + 1.0).alpha == doctest::Approx(0.5));
    CHECK(mt_ui::presentation(v, g_now + 1.0).center.x == doctest::Approx(100));
    CHECK(mt_ui::presentation(v, g_now + 2.0).alpha == doctest::Approx(0));
    CHECK(mt_uikit_animation_count(g_now + 2.0) == 0);
    ui.turn(2.0); // prunes them
    CHECK(ui.v(a).anims.empty());
    // A change outside a block is immediate; an unbalanced commit is logged.
    ui.send(a, "setAlpha:", {fb(1)});
    CHECK(mt_ui::presentation(ui.v(a), g_now).alpha == doctest::Approx(1));
    ui.send(cls, "commitAnimations");
    CHECK(logged("without +beginAnimations:context:"));
    (void)cpu;
}

TEST_CASE("views: the host's animation scale shortens delays and durations, 0 skips them") {
    Ui ui;
    const u32 root = ui.screen();
    const u32 a = ui.view("UIView", 0, 0, 100, 100);
    ui.send(root, "addSubview:", {a});
    mt_objc_release(&ui.cpu, a);
    const u32 cls = ui.klass("UIView");
    // A block of 1 s after a 0.5 s delay, fading `a` to `to`.
    const auto fade = [&](float to) {
        ui.send(cls, "beginAnimations:context:", {0, 0});
        const double d = 1.0, delay = 0.5;
        u32 w[2] = {};
        std::memcpy(w, &d, 8);
        ui.send(cls, "setAnimationDuration:", {w[0], w[1]});
        std::memcpy(w, &delay, 8);
        ui.send(cls, "setAnimationDelay:", {w[0], w[1]});
        ui.send(cls, "setAnimationCurve:", {3}); // linear
        ui.send(a, "setAlpha:", {fb(to)});
        ui.send(cls, "commitAnimations");
    };
    CHECK(mt_uikit_animation_scale() == 1.0);
    CHECK(mt_uikit_set_animation_scale(-1) == 0);
    CHECK(mt_uikit_set_animation_scale(std::nan("")) == 0);
    CHECK(mt_uikit_animation_scale() == 1.0);
    // A quarter: the delay 0.125 s, the duration 0.25 s.
    REQUIRE(mt_uikit_set_animation_scale(0.25) == 1);
    fade(0);
    CHECK(mt_f32(ui.send(a, "alpha")) == 0);
    CHECK(mt_ui::presentation(ui.v(a), g_now + 0.1).alpha == doctest::Approx(1));
    CHECK(mt_ui::presentation(ui.v(a), g_now + 0.25).alpha == doctest::Approx(0.5));
    CHECK(mt_ui::presentation(ui.v(a), g_now + 0.375).alpha == doctest::Approx(0));
    CHECK(mt_uikit_animation_count(g_now + 0.375) == 0);
    ui.turn(1.0);
    // 0: no animation, the model value shows at once.
    REQUIRE(mt_uikit_set_animation_scale(0) == 1);
    fade(1);
    CHECK(ui.v(a).anims.empty());
    CHECK(mt_uikit_animation_count(g_now) == 0);
    CHECK(mt_ui::presentation(ui.v(a), g_now).alpha == doctest::Approx(1));
    // Above 1 is clamped; 1 is the game's own timing again.
    REQUIRE(mt_uikit_set_animation_scale(3) == 1);
    CHECK(mt_uikit_animation_scale() == 1.0);
    fade(0);
    CHECK(mt_ui::presentation(ui.v(a), g_now + 1.0).alpha == doctest::Approx(0.5));
    CHECK(mt_uikit_animation_count(g_now + 1.4) == 1);
}

// --- touches -----------------------------------------------------------------------------------

TEST_CASE("touches: hit testing, delivery with locationInView:, forwarding and multitouch") {
    Ui ui;
    const u32 root = ui.screen();
    const u32 back = ui.view("TestView", 0, 0, 480, 320);
    ui.send(root, "addSubview:", {back});
    mt_objc_release(&ui.cpu, back);
    const u32 front = ui.view("TestView", 100, 100, 100, 50);
    ui.send(back, "addSubview:", {front});
    mt_objc_release(&ui.cpu, front);
    // A plain UIView inside `front` forwards its touches to it (UIResponder).
    const u32 plain = ui.view("UIView", 0, 0, 20, 20);
    ui.send(front, "addSubview:", {plain});
    mt_objc_release(&ui.cpu, plain);

    ui.touch(1, MT_UIKIT_TOUCH_BEGAN, 150, 120);
    ui.turn();
    ui.touch(1, MT_UIKIT_TOUCH_MOVED, 160, 125);
    ui.touch(1, MT_UIKIT_TOUCH_ENDED, 160, 125);
    ui.turn(0.1);
    REQUIRE(g_rec.touches.size() == 3);
    CHECK(g_rec.touches[0] == "began TestView (50, 20)");
    CHECK(g_rec.touches[1] == "moved TestView (60, 25)");
    CHECK(g_rec.touches[2] == "ended TestView (60, 25)");
    g_rec.touches.clear();

    ui.touch(2, MT_UIKIT_TOUCH_BEGAN, 105, 105);
    ui.touch(2, MT_UIKIT_TOUCH_CANCELLED, 105, 105);
    ui.turn();
    REQUIRE(g_rec.touches.size() == 2);
    CHECK(g_rec.touches[0] == "began TestView (5, 5) (forwarded)");
    CHECK(g_rec.touches[1] == "cancelled TestView (5, 5) (forwarded)");
    g_rec.touches.clear();

    // Hidden and non-interactive views are skipped: the touch goes to the view behind.
    ui.send(front, "setUserInteractionEnabled:", {0});
    ui.touch(3, MT_UIKIT_TOUCH_BEGAN, 150, 120);
    ui.touch(3, MT_UIKIT_TOUCH_ENDED, 150, 120);
    ui.turn();
    REQUIRE(g_rec.touches.size() == 2);
    CHECK(g_rec.touches[0] == "began TestView (150, 120)");
    g_rec.touches.clear();

    // A view without multipleTouchEnabled gets only the first of two fingers.
    ui.touch(4, MT_UIKIT_TOUCH_BEGAN, 300, 200);
    ui.touch(5, MT_UIKIT_TOUCH_BEGAN, 310, 210);
    ui.touch(5, MT_UIKIT_TOUCH_ENDED, 310, 210);
    ui.touch(4, MT_UIKIT_TOUCH_ENDED, 300, 200);
    ui.turn();
    CHECK(g_rec.touches.size() == 2);
    g_rec.touches.clear();
    ui.send(back, "setMultipleTouchEnabled:", {1});
    ui.touch(4, MT_UIKIT_TOUCH_BEGAN, 300, 200);
    ui.touch(5, MT_UIKIT_TOUCH_BEGAN, 310, 210);
    ui.touch(5, MT_UIKIT_TOUCH_ENDED, 310, 210);
    ui.touch(4, MT_UIKIT_TOUCH_ENDED, 300, 200);
    ui.turn();
    CHECK(g_rec.touches.size() == 4);
    // An invalid phase is refused.
    CHECK(mt_uikit_touch(1, 9, 0, 0) == -1);
}

TEST_CASE("touches: the touch being delivered, inside its view's touches...: only (#194)") {
    Ui ui;
    const u32 root = ui.screen();
    const u32 back = ui.view("TestView", 0, 0, 480, 320);
    ui.send(back, "setMultipleTouchEnabled:", {1});
    ui.send(root, "addSubview:", {back});
    mt_objc_release(&ui.cpu, back);
    g_rec.delivering.clear();
    std::uint32_t finger = 7, phase = 7;
    CHECK(mt_uikit_touch_delivering(&finger, &phase) == 0); // outside a delivery
    CHECK(finger == 7);
    CHECK(phase == 7);
    ui.touch(0x100, MT_UIKIT_TOUCH_BEGAN, 50, 280);
    ui.touch(0x101, MT_UIKIT_TOUCH_BEGAN, 440, 290);
    ui.turn();
    ui.touch(0x100, MT_UIKIT_TOUCH_MOVED, 120, 280);
    ui.touch(0x101, MT_UIKIT_TOUCH_ENDED, 440, 290);
    ui.touch(0x100, MT_UIKIT_TOUCH_CANCELLED, 120, 280);
    ui.turn();
    CHECK(g_rec.delivering ==
          std::vector<std::string>{"256/0", "257/0", "256/1", "257/2", "256/3"});
    CHECK(mt_uikit_touch_delivering(nullptr, nullptr) == 0); // and after it
}

TEST_CASE("touches: tapCount, UIEvent touchesForView: and touchesForWindow:, merged moves") {
    Ui ui;
    const u32 root = ui.screen();
    const u32 left = ui.view("TestView", 0, 0, 240, 320);
    ui.send(root, "addSubview:", {left});
    mt_objc_release(&ui.cpu, left);
    const u32 right = ui.view("TestView", 240, 0, 240, 320);
    ui.send(root, "addSubview:", {right});
    mt_objc_release(&ui.cpu, right);

    // Taps about 0.3 s apart (end to start) at about the same point count up; 0.6 s, or 40
    // points away, starts again at 1.
    auto tap = [&](float x, float y, double pause) {
        ui.turn(pause);
        ui.touch(1, MT_UIKIT_TOUCH_BEGAN, x, y);
        ui.turn(0.05);
        ui.touch(1, MT_UIKIT_TOUCH_ENDED, x, y);
        ui.turn(0.05);
    };
    tap(100, 100, 0);
    tap(102, 101, 0.2);
    tap(101, 99, 0.2);
    tap(100, 100, 0.5); // too late: 1
    tap(100, 100, 0.2); // 2
    tap(140, 100, 0.2); // too far: 1
    REQUIRE(g_rec.tap_counts.size() == 12);
    const std::vector<u32> began = {g_rec.tap_counts[0], g_rec.tap_counts[2], g_rec.tap_counts[4],
                                    g_rec.tap_counts[6], g_rec.tap_counts[8], g_rec.tap_counts[10]};
    CHECK(began == std::vector<u32>{1, 2, 3, 1, 2, 1});
    CHECK(g_rec.tap_counts[3] == 2); // touchesEnded: has the touch's count too
    // A touch that moves 10 points is no tap: the next one is 1.
    ui.touch(1, MT_UIKIT_TOUCH_BEGAN, 140, 100);
    ui.touch(1, MT_UIKIT_TOUCH_MOVED, 160, 100);
    ui.touch(1, MT_UIKIT_TOUCH_ENDED, 160, 100);
    ui.turn(0.1);
    tap(160, 100, 0.1);
    CHECK(g_rec.tap_counts.back() == 1);
    // Nor is a cancelled one.
    tap(160, 100, 0.1);
    CHECK(g_rec.tap_counts.back() == 2);
    ui.touch(1, MT_UIKIT_TOUCH_BEGAN, 160, 100);
    ui.touch(1, MT_UIKIT_TOUCH_CANCELLED, 160, 100);
    ui.turn(0.05);
    tap(160, 100, 0.05);
    CHECK(g_rec.tap_counts.back() == 1);

    // Two fingers on two views: touchesForView: has the view's own touch, touchesForWindow:
    // and allTouches both.
    g_rec = Record();
    ui.touch(1, MT_UIKIT_TOUCH_BEGAN, 100, 100);
    ui.turn(1);
    ui.touch(2, MT_UIKIT_TOUCH_BEGAN, 300, 100);
    ui.turn(0.05);
    REQUIRE(g_rec.touches.size() == 2);
    CHECK(g_rec.for_view == std::vector<u32>{1, 1});
    CHECK(g_rec.for_window == std::vector<u32>{1, 2});
    CHECK(g_rec.all == std::vector<u32>{1, 2});

    // Moves of one finger queued in one turn are merged: one touchesMoved: at the last point.
    g_rec = Record();
    ui.touch(2, MT_UIKIT_TOUCH_MOVED, 310, 100);
    ui.touch(2, MT_UIKIT_TOUCH_MOVED, 320, 100);
    ui.touch(1, MT_UIKIT_TOUCH_MOVED, 110, 100);
    ui.touch(2, MT_UIKIT_TOUCH_MOVED, 330, 110);
    ui.turn(0.05);
    CHECK(g_rec.touches ==
          std::vector<std::string>{"moved TestView (90, 110)", "moved TestView (110, 100)"});
    // Another event of the finger in between keeps its moves apart.
    g_rec = Record();
    ui.touch(2, MT_UIKIT_TOUCH_MOVED, 340, 110);
    ui.touch(2, MT_UIKIT_TOUCH_ENDED, 340, 110);
    ui.touch(2, MT_UIKIT_TOUCH_BEGAN, 350, 110);
    ui.touch(2, MT_UIKIT_TOUCH_MOVED, 360, 110);
    ui.touch(1, MT_UIKIT_TOUCH_ENDED, 110, 100);
    ui.touch(2, MT_UIKIT_TOUCH_ENDED, 360, 110);
    ui.turn(0.05);
    CHECK(g_rec.touches ==
          std::vector<std::string>{"moved TestView (100, 110)", "ended TestView (100, 110)",
                                   "began TestView (110, 110)", "moved TestView (120, 110)",
                                   "ended TestView (110, 100)", "ended TestView (120, 110)"});
}

TEST_CASE("touches: UIButton actions for touch down, up inside and up outside") {
    Ui ui;
    const u32 root = ui.screen();
    const u32 button = ui.send(ui.klass("UIButton"), "buttonWithType:", {0});
    ui.send(button, "setFrame:", {fb(100), fb(100), fb(80), fb(40)});
    ui.send(root, "addSubview:", {button});
    const u32 target = mt_objc_create_instance(ui.rt, ui.klass("TestTarget"));
    ui.send(button, "addTarget:action:forControlEvents:",
            {target, mt_objc_sel(ui.rt, "down:"), mt_ui::k_event_touch_down});
    ui.send(button, "addTarget:action:forControlEvents:",
            {target, mt_objc_sel(ui.rt, "up:"), mt_ui::k_event_touch_up_inside});
    ui.send(button, "addTarget:action:forControlEvents:",
            {target, mt_objc_sel(ui.rt, "upOutside"), mt_ui::k_event_touch_up_outside});
    ui.touch(1, MT_UIKIT_TOUCH_BEGAN, 120, 110);
    ui.turn();
    CHECK(ui.send(button, "isHighlighted") == 1);
    // 60 points outside is still "inside" (UIControl's margin); 100 is outside.
    ui.touch(1, MT_UIKIT_TOUCH_MOVED, 240, 110);
    ui.turn();
    CHECK(ui.send(button, "isHighlighted") == 1);
    ui.touch(1, MT_UIKIT_TOUCH_ENDED, 240, 110);
    ui.turn();
    CHECK(ui.send(button, "isHighlighted") == 0);
    ui.touch(2, MT_UIKIT_TOUCH_BEGAN, 120, 110);
    ui.touch(2, MT_UIKIT_TOUCH_MOVED, 300, 110);
    ui.touch(2, MT_UIKIT_TOUCH_ENDED, 300, 110);
    ui.turn();
    CHECK(g_rec.actions == std::vector<std::string>{"down:", "up:", "down:", "upOutside"});
    mt_objc_release(&ui.cpu, target);
}

TEST_CASE("touches: a paging UIScrollView drags, settles on a page and tells its delegate") {
    Ui ui;
    const u32 root = ui.screen();
    const u32 scroll = ui.view("UIScrollView", 0, 0, 480, 320);
    ui.send(root, "addSubview:", {scroll});
    mt_objc_release(&ui.cpu, scroll);
    ui.send(scroll, "setContentSize:", {fb(1440), fb(320)});
    ui.send(scroll, "setPagingEnabled:", {1});
    const u32 target = mt_objc_create_instance(ui.rt, ui.klass("TestTarget"));
    ui.send(scroll, "setDelegate:", {target});
    // A button on page 0: a tap reaches it after the scroll view held it back.
    const u32 button = ui.send(ui.klass("UIButton"), "buttonWithType:", {0});
    ui.send(button, "setFrame:", {fb(10), fb(10), fb(100), fb(100)});
    ui.send(scroll, "addSubview:", {button});
    ui.send(button, "addTarget:action:forControlEvents:",
            {target, mt_objc_sel(ui.rt, "up:"), mt_ui::k_event_touch_up_inside});
    ui.touch(1, MT_UIKIT_TOUCH_BEGAN, 50, 50);
    ui.turn();
    ui.touch(1, MT_UIKIT_TOUCH_ENDED, 50, 50);
    ui.turn(0.05);
    CHECK(g_rec.actions == std::vector<std::string>{"up:"});
    // A flick to the left from the button: the scroll view takes the touch, the button does
    // not fire.
    ui.touch(2, MT_UIKIT_TOUCH_BEGAN, 400, 60);
    ui.turn(0.01);
    ui.touch(2, MT_UIKIT_TOUCH_MOVED, 300, 60);
    ui.turn(0.05);
    ui.touch(2, MT_UIKIT_TOUCH_MOVED, 200, 60);
    ui.turn(0.05);
    // The content follows the finger from where the drag was recognised (the move to 300).
    CHECK(ui.send_stret(scroll, "contentOffset", 2)[0] == doctest::Approx(100));
    ui.touch(2, MT_UIKIT_TOUCH_ENDED, 200, 60);
    ui.turn(0.01);
    for (int i = 0; i < 40; i++) {
        ui.turn(0.02);
    }
    CHECK(ui.send_stret(scroll, "contentOffset", 2)[0] == doctest::Approx(480));
    CHECK(g_rec.scrolls > 3);
    CHECK(g_rec.actions.size() == 1);
    // A slow drag back by more than half a page settles on page 0.
    ui.touch(3, MT_UIKIT_TOUCH_BEGAN, 100, 60);
    ui.turn(0.2);
    ui.touch(3, MT_UIKIT_TOUCH_MOVED, 120, 60);
    ui.turn(0.2);
    ui.touch(3, MT_UIKIT_TOUCH_MOVED, 420, 60);
    ui.turn(0.2);
    CHECK(ui.send_stret(scroll, "contentOffset", 2)[0] == doctest::Approx(180));
    ui.turn(0.2);
    ui.touch(3, MT_UIKIT_TOUCH_ENDED, 420, 60);
    for (int i = 0; i < 40; i++) {
        ui.turn(0.02);
    }
    CHECK(ui.send_stret(scroll, "contentOffset", 2)[0] == doctest::Approx(0));
    mt_objc_release(&ui.cpu, target);
}

// --- controls ------------------------------------------------------------------------------------

TEST_CASE("controls: image views, labels, text views, page controls and indicators") {
    Ui ui;
    ui.screen();
    const u32 img = ui.pattern_image(30, 20);
    const u32 iv = ui.send(ui.send(ui.klass("UIImageView"), "alloc"), "initWithImage:", {img});
    CHECK(rect(ui.frame(iv)) == "(0, 0, 30, 20)");
    CHECK(ui.send(iv, "image") == img);
    CHECK_FALSE(ui.v(iv).interaction);
    mt_objc_release(&ui.cpu, iv);

    const u32 label = ui.view("UILabel", 0, 0, 100, 40);
    CHECK(ui.v(label).background_color.a == 1); // iOS 4: opaque white
    ui.send(label, "setText:", {ui.string("Please Wait...")});
    CHECK(mt_ns::string_utf8(&ui.cpu, ui.send(label, "text")) == "Please Wait...");
    ui.send(label, "setTextAlignment:", {1});
    ui.send(label, "setNumberOfLines:", {2});
    CHECK(ui.v(label).alignment == 1);
    CHECK(ui.v(label).lines == 2);
    mt_objc_release(&ui.cpu, label);

    // Word wrapping: words to the width, long words between characters, a truncated tail.
    mt_cg::Font f;
    f.size = 12;
    const double word = mt_cg::text_width(u"word", f);
    const auto lines = mt_ui::wrap_text(u"word word word", f, word * 2.5, 0);
    CHECK(lines == std::vector<std::u16string>{u"word word", u"word"});
    CHECK(mt_ui::wrap_text(u"a\nb", f, 100, 0).size() == 2);
    CHECK(mt_ui::wrap_text(u"wordwordword", f, word, 0).size() == 3);
    const auto cut = mt_ui::wrap_text(u"word word word word", f, word * 1.5, 1);
    REQUIRE(cut.size() == 1);
    CHECK(cut[0].substr(cut[0].size() - 3) == u"...");

    // UITextView: wrapped text sets the content height.
    const u32 tv = ui.view("UITextView", 0, 0, 100, 50);
    ui.send(tv, "setEditable:", {0});
    ui.send(tv, "setText:", {ui.string("one two three four five six seven eight nine ten")});
    CHECK(ui.v(tv).content_height > 50);
    mt_objc_release(&ui.cpu, tv);

    const u32 pc = ui.view("UIPageControl", 0, 0, 100, 20);
    ui.send(pc, "setNumberOfPages:", {3});
    ui.send(pc, "setCurrentPage:", {5});
    CHECK(ui.send(pc, "currentPage") == 2);
    mt_objc_release(&ui.cpu, pc);

    const u32 act = ui.send(ui.send(ui.klass("UIActivityIndicatorView"), "alloc"),
                            "initWithActivityIndicatorStyle:", {0});
    CHECK(rect(ui.frame(act)) == "(0, 0, 37, 37)");
    CHECK(ui.send(act, "isAnimating") == 0);
    ui.send(act, "startAnimating");
    CHECK(ui.send(act, "isAnimating") == 1);
    mt_objc_release(&ui.cpu, act);
}

TEST_CASE("app: UIDevice, UIApplication and view controllers") {
    Ui ui;
    ui.screen();
    const u32 device = ui.send(ui.klass("UIDevice"), "currentDevice");
    CHECK(mt_ns::string_utf8(&ui.cpu, ui.send(device, "model")) == "iPhone");
    CHECK(mt_ns::string_utf8(&ui.cpu, ui.send(device, "systemVersion")) == "4.3.5");
    const u32 app = ui.send(ui.klass("UIApplication"), "sharedApplication");
    ui.send(app, "setApplicationIconBadgeNumber:", {0});
    ui.send(app, "registerForRemoteNotificationTypes:", {7});
    CHECK(logged("no push service"));
    const u32 url =
        ui.send(ui.klass("NSURL"), "URLWithString:", {ui.string("http://example.com/")});
    CHECK(ui.send(app, "openURL:", {url}) == 0); // no handler
    CHECK(logged("http://example.com/: not opened"));

    // A controller presents another: -viewDidAppear: at the next turn; dismiss from either.
    const u32 vc = ui.send(ui.send(ui.klass("UIViewController"), "alloc"),
                           "initWithNibName:bundle:", {ui.string("Missing"), 0});
    const u32 view = ui.view("UIView", 0, 0, 320, 480);
    ui.send(vc, "setView:", {view});
    CHECK(ui.send(vc, "view") == view);
    mt_objc_release(&ui.cpu, view);
    const u32 modal = ui.send(ui.send(ui.klass("TestViewController"), "alloc"), "init");
    ui.send(vc, "presentModalViewController:animated:", {modal, 1});
    CHECK(g_rec.appeared == 0);
    ui.turn();
    CHECK(g_rec.appeared == 1);
    ui.send(modal, "dismissModalViewControllerAnimated:", {1});
    ui.send(vc, "presentModalViewController:animated:", {modal, 1}); // possible again
    ui.turn();
    CHECK(g_rec.appeared == 2);
    mt_objc_release(&ui.cpu, modal);
    mt_objc_release(&ui.cpu, vc);
    // GameKit's leaderboard controller (#52) is a UIViewController of the SDK's size.
    const u32 gk = ui.klass("GKLeaderboardViewController");
    CHECK(mt_objc_superclass(ui.rt, gk) == ui.klass("UIViewController"));
    CHECK(mt_objc_instance_size(ui.rt, ui.klass("UIViewController")) == 152u);
    CHECK(mt_objc_instance_size(ui.rt, ui.klass("UIView")) == 46u);
}

// --- the compositor
// --------------------------------------------------------------------------------

TEST_CASE("compositor: views, group opacity, clipping, images, text and the GL slot") {
    Ui ui;
    const u32 root = ui.screen();
    ui.send(root, "setBackgroundColor:", {ui.color(0.1f, 0.1f, 0.3f, 1)});
    auto add = [&](u32 parent, const char *cls, float x, float y, float w, float h) {
        const u32 v = ui.view(cls, x, y, w, h);
        ui.send(parent, "addSubview:", {v});
        mt_objc_release(&ui.cpu, v);
        return v;
    };
    const u32 red = add(root, "UIView", 20, 20, 100, 60);
    ui.send(red, "setBackgroundColor:", {ui.color(1, 0, 0, 1)});
    // Group opacity: the white child is blended once with its green parent's 50%.
    const u32 group = add(root, "UIView", 140, 20, 120, 80);
    ui.send(group, "setBackgroundColor:", {ui.color(0, 1, 0, 1)});
    ui.send(group, "setAlpha:", {fb(0.5f)});
    const u32 child = add(group, "UIView", 10, 10, 40, 40);
    ui.send(child, "setBackgroundColor:", {ui.color(1, 1, 1, 1)});
    // Clipping.
    const u32 clip = add(root, "UIView", 280, 20, 80, 80);
    ui.send(clip, "setBackgroundColor:", {ui.color(1, 1, 0, 1)});
    ui.send(clip, "setClipsToBounds:", {1});
    const u32 inner = add(clip, "UIView", 40, 40, 80, 80);
    ui.send(inner, "setBackgroundColor:", {ui.color(1, 0, 1, 1)});
    // An image view (scaled to its frame), a label, a button, a page control.
    const u32 iv = add(root, "UIImageView", 20, 120, 64, 48);
    ui.send(iv, "setImage:", {ui.pattern_image(32, 24)});
    const u32 label = add(root, "UILabel", 100, 120, 160, 40);
    ui.send(label, "setBackgroundColor:", {ui.send(ui.klass("UIColor"), "clearColor")});
    ui.send(label, "setTextColor:", {ui.color(1, 1, 1, 1)});
    ui.send(label, "setTextAlignment:", {1});
    ui.send(label, "setText:", {ui.string("UIKit 54")});
    CHECK(ui.v(label).text_color.r == 1);
    CHECK(ui.v(label).text_color.a == 1);
    const u32 button = add(root, "UIButton", 280, 120, 64, 48);
    ui.send(button, "setImage:forState:", {ui.pattern_image(16, 16), 0});
    ui.send(button, "setImageEdgeInsets:", {fb(0), fb(0), fb(0), fb(16)});
    const u32 pc = add(root, "UIPageControl", 20, 260, 120, 20);
    ui.send(pc, "setNumberOfPages:", {4});
    ui.send(pc, "setCurrentPage:", {1});
    // -drawRect: of a subclass runs in a UIGraphics context at the next turn.
    g_rec.draw = true;
    add(root, "TestView", 360, 200, 40, 40);
    // The GL view.
    const u32 gl = add(root, "TestGLView", 160, 200, 160, 100);
    CHECK(ui.send(gl, "layer") != 0);
    ui.turn();

    float r[4] = {};
    u32 dw = 0, dh = 0;
    REQUIRE(mt_uikit_gl_rect(r, &dw, &dh) == 1);
    CHECK(r[0] == 160);
    CHECK(r[1] == 200);
    CHECK(r[2] == 160);
    CHECK(r[3] == 100);
    CHECK(dw == 160);
    CHECK(dh == 100);

    const golden::Image full = ui.composite(MT_UIKIT_COMPOSITE_FULL, 480, 320);
    CHECK(full.at(70, 50) == golden::Rgba{255, 0, 0, 255});     // red
    CHECK(full.at(240, 300) == golden::Rgba{26, 26, 77, 255});  // the root's colour
    CHECK(full.at(240, 250) == golden::Rgba{0, 0, 0, 255});     // GL placeholder
    CHECK(full.at(365, 205) == golden::Rgba{255, 128, 0, 255}); // drawRect:
    const golden::Rgba g = full.at(170, 50);                    // white in the group
    CHECK(g.r == doctest::Approx(140).epsilon(0.03));
    CHECK(g.g == doctest::Approx(140).epsilon(0.03));
    CHECK(full.at(350, 90) == golden::Rgba{255, 0, 255, 255}); // inside the clip
    CHECK(full.at(370, 90) == golden::Rgba{26, 26, 77, 255});  // clipped away
    check_golden(full, "uikit_screen");

    // A GL frame fills the GL view; the overlay leaves the GL view transparent.
    std::vector<u8> frame(4 * 4 * 4, 0);
    for (int i = 0; i < 16; i++) {
        frame[4 * i] = 0;
        frame[4 * i + 1] = 200;
        frame[4 * i + 2] = 200;
        frame[4 * i + 3] = 255;
    }
    const mt_uikit_gl_frame glf{frame.data(), 4, 4, 16};
    const golden::Image with_gl = ui.composite(MT_UIKIT_COMPOSITE_FULL, 480, 320, &glf);
    CHECK(with_gl.at(240, 250) == golden::Rgba{0, 200, 200, 255});
    const golden::Image overlay = ui.composite(MT_UIKIT_COMPOSITE_OVERLAY, 480, 320);
    CHECK(overlay.at(240, 250) == golden::Rgba{0, 0, 0, 0});
    CHECK(overlay.at(70, 50) == golden::Rgba{255, 0, 0, 255});
    // 2x: the same screen at twice the pixels.
    const golden::Image big = ui.composite(MT_UIKIT_COMPOSITE_FULL, 960, 640);
    CHECK(big.at(140, 100) == golden::Rgba{255, 0, 0, 255});
    // The rasterizer's shortcuts (#134) give the generic path's bytes, at every render scale and
    // in both modes (with a GL frame drawn 1:1 into the slot at 4x).
    for (const int k : {1, 2, 3, 4}) {
        std::vector<u8> gl4(static_cast<std::size_t>(160 * k) * (100 * k) * 4);
        for (std::size_t i = 0; i < gl4.size(); i++) {
            gl4[i] = static_cast<u8>(i % 4 == 3 ? (i / 4) % 7 * 40 : i * 37 % 251);
        }
        const mt_uikit_gl_frame glk{gl4.data(), static_cast<u32>(160 * k),
                                    static_cast<u32>(100 * k), static_cast<u32>(160 * k * 4)};
        for (const u32 mode : {MT_UIKIT_COMPOSITE_FULL, MT_UIKIT_COMPOSITE_OVERLAY}) {
            const golden::Image fast = ui.composite(mode, 480 * k, 320 * k, &glk);
            mt_cg::set_raster_shortcuts(false);
            const golden::Image slow = ui.composite(mode, 480 * k, 320 * k, &glk);
            mt_cg::set_raster_shortcuts(true);
            CHECK_MESSAGE(fast == slow, "scale ", k, ", mode ", mode);
        }
    }
    // Bad arguments.
    std::vector<u8> px(16);
    CHECK(mt_uikit_composite(&ui.cpu, g_now, 7, nullptr, px.data(), 2, 2, 8) == -1);
    CHECK(mt_uikit_composite(&ui.cpu, g_now, 0, nullptr, px.data(), 2, 2, 4) == -1);
}

TEST_CASE("compositor: the composite trace channel times each view (#134)") {
    Ui ui;
    const u32 root = ui.screen();
    ui.send(root, "setBackgroundColor:", {ui.color(0, 0, 1, 1)});
    const u32 iv = ui.view("UIImageView", 20, 10, 64, 48);
    ui.send(iv, "setImage:", {ui.pattern_image(32, 24)});
    ui.send(root, "addSubview:", {iv});
    mt_objc_release(&ui.cpu, iv);
    ui.turn();
    std::vector<std::string> lines;
    mt_diag_config cfg = mt_diag_config_default();
    cfg.trace = "composite";
    cfg.log = [](void *user, const char *line) {
        static_cast<std::vector<std::string> *>(user)->emplace_back(line);
    };
    cfg.log_user = &lines;
    mt_diag_configure(&cfg);
    ui.composite(MT_UIKIT_COMPOSITE_OVERLAY, 960, 640);
    mt_diag_reset();
    // The window, the screen view, the image view (indented by depth), then the composite.
    REQUIRE(lines.size() == 4);
    CHECK(lines[0].rfind("composite: UIWindow 0x", 0) == 0);
    CHECK(lines[1].rfind("composite:   UIView 0x", 0) == 0);
    CHECK(lines[1].find(" (0,0 960x640) alpha 1.000 ") != std::string::npos);
    CHECK(lines[2].rfind("composite:     UIImageView 0x", 0) == 0);
    CHECK(lines[2].find(" kind 2 (40,20 128x96) alpha 1.000 ") != std::string::npos);
    CHECK(lines[3].rfind("composite: 960x640 overlay at ", 0) == 0);
    CHECK(lines[3].find(" ms") != std::string::npos);
    // Off: no lines.
    lines.clear();
    ui.composite(MT_UIKIT_COMPOSITE_OVERLAY, 480, 320);
    CHECK(lines.empty());
}

TEST_CASE("compositor: the host's opacity fades a view without the guest seeing it") {
    Ui ui;
    const u32 root = ui.screen();
    ui.send(root, "setBackgroundColor:", {ui.color(0, 0, 1, 1)});
    // A full-screen touch view (like TouchKeyPadView) holding a panel (like its subview 0)
    // with a red key in it.
    const u32 pad = ui.view("TestView", 0, 0, 480, 320);
    ui.send(root, "addSubview:", {pad});
    mt_objc_release(&ui.cpu, pad);
    const u32 panel = ui.view("UIView", 0, 0, 480, 320);
    ui.send(pad, "addSubview:", {panel});
    mt_objc_release(&ui.cpu, panel);
    const u32 key = ui.view("UIView", 20, 200, 100, 100);
    ui.send(key, "setBackgroundColor:", {ui.color(1, 0, 0, 1)});
    ui.send(panel, "addSubview:", {key});
    mt_objc_release(&ui.cpu, key);
    ui.turn();
    const golden::Image shown = ui.composite(MT_UIKIT_COMPOSITE_FULL, 480, 320);
    CHECK(shown.at(70, 250) == golden::Rgba{255, 0, 0, 255});
    CHECK(mt_uikit_host_opacity(panel) == 1.0f);

    // Host opacity 0: the panel's subtree is not drawn; the guest's alpha and hidden flag are
    // what they were, and a touch on the key still reaches the touch view by hit testing.
    REQUIRE(mt_uikit_set_host_opacity(panel, 0) == 1);
    CHECK(mt_uikit_host_opacity(panel) == 0.0f);
    const golden::Image hidden = ui.composite(MT_UIKIT_COMPOSITE_FULL, 480, 320);
    CHECK(hidden.at(70, 250) == golden::Rgba{0, 0, 255, 255});
    CHECK(hidden.at(300, 50) == shown.at(300, 50));
    float alpha = 0;
    const u32 alpha_bits = ui.send(panel, "alpha");
    std::memcpy(&alpha, &alpha_bits, 4);
    CHECK(alpha == 1.0f);
    CHECK(ui.send(panel, "isHidden") == 0);
    CHECK(ui.v(panel).model.alpha == 1.0);
    ui.touch(1, MT_UIKIT_TOUCH_BEGAN, 70, 250);
    ui.touch(1, MT_UIKIT_TOUCH_ENDED, 70, 250);
    ui.turn();
    REQUIRE(g_rec.touches.size() == 2);
    // The hit view is the key, as when it is shown, which forwards to the touch view.
    CHECK(g_rec.touches[0] == "began TestView (70, 250) (forwarded)");
    CHECK(g_rec.touches[1] == "ended TestView (70, 250) (forwarded)");

    // Half way: the subtree is blended as a group with the host's opacity.
    REQUIRE(mt_uikit_set_host_opacity(panel, 0.5f) == 1);
    const golden::Image half = ui.composite(MT_UIKIT_COMPOSITE_FULL, 480, 320);
    const golden::Rgba h = half.at(70, 250);
    CHECK(h.r == doctest::Approx(128).epsilon(0.02));
    CHECK(h.b == doctest::Approx(128).epsilon(0.02));
    // It multiplies the game's own alpha: the game hiding the panel (alpha 0) still hides it.
    ui.send(panel, "setAlpha:", {fb(0.5f)});
    const golden::Image quarter = ui.composite(MT_UIKIT_COMPOSITE_FULL, 480, 320);
    CHECK(quarter.at(70, 250).r == doctest::Approx(64).epsilon(0.04));
    ui.send(panel, "setAlpha:", {fb(1)});

    // Back to 1: the screen is exactly what it was; values are clamped; non-views refused.
    REQUIRE(mt_uikit_set_host_opacity(panel, 7) == 1);
    CHECK(mt_uikit_host_opacity(panel) == 1.0f);
    CHECK(ui.composite(MT_UIKIT_COMPOSITE_FULL, 480, 320).bytes() == shown.bytes());
    REQUIRE(mt_uikit_set_host_opacity(panel, -1) == 1);
    CHECK(mt_uikit_host_opacity(panel) == 0.0f);
    CHECK(mt_uikit_set_host_opacity(0, 0) == 0);
    CHECK(mt_uikit_set_host_opacity(ui.color(1, 1, 1, 1), 0) == 0);
    CHECK(mt_uikit_host_opacity(0) == 1.0f);
}

TEST_CASE("compositor: the host's transform moves a view, its reveal shows it, hits unchanged") {
    Ui ui;
    const u32 root = ui.screen();
    ui.send(root, "setBackgroundColor:", {ui.color(0, 0, 1, 1)});
    const u32 pad = ui.view("TestView", 0, 0, 480, 320);
    ui.send(root, "addSubview:", {pad});
    mt_objc_release(&ui.cpu, pad);
    const u32 panel = ui.view("UIView", 0, 0, 480, 320);
    ui.send(pad, "addSubview:", {panel});
    mt_objc_release(&ui.cpu, panel);
    // A red key with a green mark inside it (a subview: it moves with it).
    const u32 key = ui.view("UIView", 20, 200, 100, 100);
    ui.send(key, "setBackgroundColor:", {ui.color(1, 0, 0, 1)});
    ui.send(panel, "addSubview:", {key});
    mt_objc_release(&ui.cpu, key);
    const u32 mark = ui.view("UIView", 0, 0, 10, 10);
    ui.send(mark, "setBackgroundColor:", {ui.color(0, 1, 0, 1)});
    ui.send(key, "addSubview:", {mark});
    mt_objc_release(&ui.cpu, mark);
    ui.turn();
    const golden::Image shown = ui.composite(MT_UIKIT_COMPOSITE_FULL, 480, 320);
    CHECK(shown.at(70, 250) == golden::Rgba{255, 0, 0, 255});
    CHECK(shown.at(25, 205) == golden::Rgba{0, 255, 0, 255});

    // The tree's lookups: the subviews in order, the frame in screen points.
    u32 subs[4] = {};
    CHECK(mt_uikit_subviews(panel, subs, 4) == 1);
    CHECK(subs[0] == key);
    CHECK(mt_uikit_subviews(key, nullptr, 0) == 1);
    CHECK(mt_uikit_subviews(mark, subs, 4) == 0);
    CHECK(mt_uikit_subviews(0, subs, 4) == 0);
    float fr[4] = {};
    REQUIRE(mt_uikit_screen_frame(key, fr) == 1);
    CHECK(fr[0] == doctest::Approx(20));
    CHECK(fr[1] == doctest::Approx(200));
    CHECK(fr[2] == doctest::Approx(100));
    CHECK(fr[3] == doctest::Approx(100));
    REQUIRE(mt_uikit_screen_frame(mark, fr) == 1);
    CHECK(fr[0] == doctest::Approx(20));
    CHECK(fr[1] == doctest::Approx(200));
    CHECK(mt_uikit_screen_frame(0, fr) == 0);

    // The identity is no transform: the same bytes.
    const float identity[6] = {1, 0, 0, 1, 0, 0};
    REQUIRE(mt_uikit_set_host_transform(key, identity) == 1);
    CHECK(mt_uikit_host_transform(key, nullptr) == 0);
    CHECK(ui.composite(MT_UIKIT_COMPOSITE_FULL, 480, 320).bytes() == shown.bytes());

    // Half the size, moved: (x, y) -> (0.5 x + 300, 0.5 y + 10): the key at (310, 110, 50, 50),
    // its mark with it; the guest's frame is unchanged, and so is the hit test.
    const float m[6] = {0.5f, 0, 0, 0.5f, 300, 10};
    REQUIRE(mt_uikit_set_host_transform(key, m) == 1);
    float back[6] = {};
    CHECK(mt_uikit_host_transform(key, back) == 1);
    CHECK(back[0] == 0.5f);
    CHECK(back[4] == 300.0f);
    const golden::Image moved = ui.composite(MT_UIKIT_COMPOSITE_FULL, 480, 320);
    CHECK(moved.at(70, 250) == golden::Rgba{0, 0, 255, 255});
    CHECK(moved.at(340, 140) == golden::Rgba{255, 0, 0, 255});
    CHECK(moved.at(312, 112) == golden::Rgba{0, 255, 0, 255});
    CHECK(moved.at(365, 140) == golden::Rgba{0, 0, 255, 255});
    // At twice the scale (the 960 x 640 render scale) the same points.
    const golden::Image moved2 = ui.composite(MT_UIKIT_COMPOSITE_FULL, 960, 640);
    CHECK(moved2.at(680, 280) == golden::Rgba{255, 0, 0, 255});
    CHECK(moved2.at(140, 500) == golden::Rgba{0, 0, 255, 255});
    REQUIRE(mt_uikit_screen_frame(key, fr) == 1);
    CHECK(fr[0] == doctest::Approx(20));
    ui.touch(1, MT_UIKIT_TOUCH_BEGAN, 70, 250);
    ui.touch(1, MT_UIKIT_TOUCH_ENDED, 70, 250);
    ui.turn();
    REQUIRE(g_rec.touches.size() == 2);
    CHECK(g_rec.touches[0] == "began TestView (70, 250) (forwarded)");
    const float bad[6] = {1, 0, 0, 1, std::nanf(""), 0};
    CHECK(mt_uikit_set_host_transform(key, bad) == 0);
    CHECK(mt_uikit_host_transform(key, back) == 1); // kept
    CHECK(mt_uikit_set_host_transform(0, m) == 0);

    // The reveal: a panel the game hid (or faded out) is drawn as shown; the guest's flags stay.
    REQUIRE(mt_uikit_set_host_transform(key, nullptr) == 1);
    CHECK(ui.composite(MT_UIKIT_COMPOSITE_FULL, 480, 320).bytes() == shown.bytes());
    ui.send(panel, "setHidden:", {1});
    CHECK(ui.composite(MT_UIKIT_COMPOSITE_FULL, 480, 320).at(70, 250) ==
          golden::Rgba{0, 0, 255, 255});
    REQUIRE(mt_uikit_set_host_reveal(panel, 1) == 1);
    CHECK(ui.composite(MT_UIKIT_COMPOSITE_FULL, 480, 320).bytes() == shown.bytes());
    CHECK(ui.send(panel, "isHidden") == 1);
    ui.send(panel, "setHidden:", {0});
    ui.send(panel, "setAlpha:", {fb(0)});
    CHECK(ui.composite(MT_UIKIT_COMPOSITE_FULL, 480, 320).bytes() == shown.bytes());
    REQUIRE(mt_uikit_set_host_reveal(panel, 0) == 1);
    CHECK(ui.composite(MT_UIKIT_COMPOSITE_FULL, 480, 320).at(70, 250) ==
          golden::Rgba{0, 0, 255, 255});
    ui.send(panel, "setAlpha:", {fb(1)});
    CHECK(ui.composite(MT_UIKIT_COMPOSITE_FULL, 480, 320).bytes() == shown.bytes());
    CHECK(mt_uikit_set_host_reveal(0, 1) == 0);
}

TEST_CASE("compositor: a view composited alone beside the screen, and the host's touch area") {
    Ui ui;
    const u32 root = ui.screen();
    ui.send(root, "setBackgroundColor:", {ui.color(0, 0, 1, 1)});
    const u32 pad = ui.view("TestView", 0, 0, 480, 320);
    ui.send(root, "addSubview:", {pad});
    mt_objc_release(&ui.cpu, pad);
    const u32 panel = ui.view("UIView", 0, 0, 480, 320);
    ui.send(pad, "addSubview:", {panel});
    mt_objc_release(&ui.cpu, panel);
    const u32 key = ui.view("UIView", 20, 200, 100, 100);
    ui.send(key, "setBackgroundColor:", {ui.color(1, 0, 0, 1)});
    ui.send(panel, "addSubview:", {key});
    mt_objc_release(&ui.cpu, key);
    const u32 mark = ui.view("UIView", 0, 0, 10, 10);
    ui.send(mark, "setBackgroundColor:", {ui.color(0, 1, 0, 1)});
    ui.send(key, "addSubview:", {mark});
    mt_objc_release(&ui.cpu, mark);
    ui.turn();
    const golden::Image shown = ui.composite(MT_UIKIT_COMPOSITE_FULL, 480, 320);

    // The key moved 60 points left, so its left part lies left of the screen (-40 ... 0): the
    // panel alone in a strip 44 points wide from x = -44 shows it, on transparent pixels (no
    // root background); the full composite still shows only the part on the screen.
    const float left[6] = {1, 0, 0, 1, -60, 0};
    REQUIRE(mt_uikit_set_host_transform(key, left) == 1);
    golden::Image strip(44, 320);
    REQUIRE(mt_uikit_composite_view(&ui.cpu, g_now, panel, -44, strip.bytes().data(), 44, 320,
                                    44 * 4) == 0);
    CHECK(strip.at(2, 250) == golden::Rgba{0, 0, 0, 0});
    CHECK(strip.at(30, 250) == golden::Rgba{255, 0, 0, 255});
    CHECK(strip.at(6, 205) == golden::Rgba{0, 255, 0, 255});
    CHECK(strip.at(30, 100) == golden::Rgba{0, 0, 0, 0});
    const golden::Image moved = ui.composite(MT_UIKIT_COMPOSITE_FULL, 480, 320);
    CHECK(moved.at(30, 250) == golden::Rgba{255, 0, 0, 255});
    CHECK(moved.at(70, 250) == golden::Rgba{0, 0, 255, 255});
    // At twice the scale, the same points.
    golden::Image strip2(88, 640);
    REQUIRE(mt_uikit_composite_view(&ui.cpu, g_now, panel, -44, strip2.bytes().data(), 88, 640,
                                    88 * 4) == 0);
    CHECK(strip2.at(60, 500) == golden::Rgba{255, 0, 0, 255});
    CHECK(strip2.at(4, 500) == golden::Rgba{0, 0, 0, 0});
    // Right of the screen: moved 440 points right, the key at 460 ... 560.
    const float right[6] = {1, 0, 0, 1, 440, 0};
    REQUIRE(mt_uikit_set_host_transform(key, right) == 1);
    REQUIRE(mt_uikit_composite_view(&ui.cpu, g_now, panel, 480, strip.bytes().data(), 44, 320,
                                    44 * 4) == 0);
    CHECK(strip.at(20, 250) == golden::Rgba{255, 0, 0, 255});
    // A hidden superview: nothing; not a view in a window, or bad arguments: refused.
    ui.send(pad, "setHidden:", {1});
    REQUIRE(mt_uikit_composite_view(&ui.cpu, g_now, panel, 480, strip.bytes().data(), 44, 320,
                                    44 * 4) == 0);
    CHECK(strip.at(20, 250) == golden::Rgba{0, 0, 0, 0});
    ui.send(pad, "setHidden:", {0});
    const u32 loose = ui.view("UIView", 0, 0, 10, 10);
    CHECK(mt_uikit_composite_view(&ui.cpu, g_now, loose, 0, strip.bytes().data(), 44, 320,
                                  44 * 4) == -1);
    mt_objc_release(&ui.cpu, loose);
    CHECK(mt_uikit_composite_view(&ui.cpu, g_now, 0, 0, strip.bytes().data(), 44, 320, 44 * 4) ==
          -1);
    CHECK(mt_uikit_composite_view(&ui.cpu, g_now, panel, 0, strip.bytes().data(), 44, 320, 4) ==
          -1);
    REQUIRE(mt_uikit_set_host_transform(key, nullptr) == 1);
    CHECK(ui.composite(MT_UIKIT_COMPOSITE_FULL, 480, 320).bytes() == shown.bytes());

    // Without a touch area, a touch beside the screen reaches nothing.
    g_rec.touches.clear();
    ui.touch(1, MT_UIKIT_TOUCH_BEGAN, -20, 250);
    ui.touch(1, MT_UIKIT_TOUCH_ENDED, -20, 250);
    ui.turn();
    CHECK(g_rec.touches.empty());
    // The panel's touch area: shifted 44 points, 44 points each side. A touch beside the screen
    // belongs to the panel (forwarded to the pad) and every point it reports is shifted, moves
    // and the end too; one on the panel inside the screen is shifted the same way.
    REQUIRE(mt_uikit_set_host_touch_area(panel, 44, 44, 44) == 1);
    ui.touch(1, MT_UIKIT_TOUCH_BEGAN, -20, 250);
    ui.turn();
    ui.touch(1, MT_UIKIT_TOUCH_MOVED, -10, 260);
    ui.touch(1, MT_UIKIT_TOUCH_ENDED, -10, 260);
    ui.turn(0.1);
    ui.touch(2, MT_UIKIT_TOUCH_BEGAN, 500, 20);
    ui.touch(2, MT_UIKIT_TOUCH_ENDED, 500, 20);
    ui.touch(3, MT_UIKIT_TOUCH_BEGAN, 200, 100);
    ui.touch(3, MT_UIKIT_TOUCH_ENDED, 200, 100);
    ui.turn(1);
    REQUIRE(g_rec.touches.size() == 7);
    CHECK(g_rec.touches[0] == "began TestView (24, 250) (forwarded)");
    CHECK(g_rec.touches[1] == "moved TestView (34, 260) (forwarded)");
    CHECK(g_rec.touches[2] == "ended TestView (34, 260) (forwarded)");
    CHECK(g_rec.touches[3] == "began TestView (544, 20) (forwarded)");
    CHECK(g_rec.touches[5] == "began TestView (244, 100) (forwarded)");
    g_rec.touches.clear();
    // Past the margins, or on a subview (the key takes its own touch): as without the area.
    ui.touch(1, MT_UIKIT_TOUCH_BEGAN, -50, 250);
    ui.touch(1, MT_UIKIT_TOUCH_ENDED, -50, 250);
    ui.touch(2, MT_UIKIT_TOUCH_BEGAN, 70, 250);
    ui.touch(2, MT_UIKIT_TOUCH_ENDED, 70, 250);
    ui.turn(1);
    REQUIRE(g_rec.touches.size() == 2);
    CHECK(g_rec.touches[0] == "began TestView (70, 250) (forwarded)");
    g_rec.touches.clear();
    // A hidden panel takes no touch beside the screen.
    ui.send(panel, "setHidden:", {1});
    ui.touch(1, MT_UIKIT_TOUCH_BEGAN, -20, 250);
    ui.touch(1, MT_UIKIT_TOUCH_ENDED, -20, 250);
    ui.turn(1);
    CHECK(g_rec.touches.empty());
    ui.send(panel, "setHidden:", {0});
    // Cleared: the screen's points again.
    REQUIRE(mt_uikit_set_host_touch_area(0, 0, 0, 0) == 1);
    ui.touch(3, MT_UIKIT_TOUCH_BEGAN, 200, 100);
    ui.touch(3, MT_UIKIT_TOUCH_ENDED, 200, 100);
    ui.turn(1);
    REQUIRE(g_rec.touches.size() == 2);
    CHECK(g_rec.touches[0] == "began TestView (200, 100) (forwarded)");
    CHECK(mt_uikit_set_host_touch_area(panel, std::nanf(""), 0, 0) == 0);
    CHECK(mt_uikit_set_host_touch_area(0x12345u, 10, 0, 0) == 0);
}

TEST_CASE("compositor: an animation is drawn at its presentation values") {
    Ui ui;
    const u32 root = ui.screen();
    ui.send(root, "setBackgroundColor:", {ui.color(0, 0, 0, 1)});
    const u32 a = ui.view("UIView", 0, 0, 40, 40);
    ui.send(root, "addSubview:", {a});
    mt_objc_release(&ui.cpu, a);
    ui.send(a, "setBackgroundColor:", {ui.color(1, 1, 1, 1)});
    const u32 cls = ui.klass("UIView");
    ui.send(cls, "beginAnimations:context:", {0, 0});
    const double d = 1.0;
    u32 w[2] = {};
    std::memcpy(w, &d, 8);
    ui.send(cls, "setAnimationDuration:", {w[0], w[1]});
    ui.send(cls, "setAnimationCurve:", {3});
    ui.send(a, "setFrame:", {fb(200), fb(0), fb(40), fb(40)});
    ui.send(cls, "commitAnimations");
    g_now += 0.5;
    const golden::Image img = ui.composite(MT_UIKIT_COMPOSITE_FULL, 480, 320);
    CHECK(img.at(120, 20) == golden::Rgba{255, 255, 255, 255}); // halfway: x 100..140
    CHECK(img.at(20, 20) == golden::Rgba{0, 0, 0, 255});
    CHECK(img.at(220, 20) == golden::Rgba{0, 0, 0, 255});
}

namespace {

// The compositor test's image hook (#153): every bundle image becomes a 2x one of opaque green.
std::vector<u8> g_green;
int green_hook(void *, const mt_cg_image_file *image, mt_cg_image_replacement *out) {
    out->width = image->width * 2;
    out->height = image->height * 2;
    g_green.assign(static_cast<std::size_t>(out->width) * out->height * 4, 0);
    for (std::size_t i = 0; i < g_green.size(); i += 4) {
        g_green[i + 1] = 255;
        g_green[i + 3] = 255;
    }
    out->rgba = g_green.data();
    return 1;
}

} // namespace

TEST_CASE("compositor: an image view draws the image hook's replacement of its file (#153)") {
    // A 16 x 12 opaque red Menu.png in the bundle.
    std::vector<u8> red(16 * 12 * 4, 0);
    for (std::size_t i = 0; i < red.size(); i += 4) {
        red[i] = 255;
        red[i + 3] = 255;
    }
    std::vector<u8> png;
    REQUIRE(mt_cg::png_encode(red.data(), 16, 12, 4, png));
    Bundle bundle(png, "Menu.png");
    for (const bool hooked : {false, true}) {
        mt_cg_set_image_replace_hook(hooked ? green_hook : nullptr, nullptr);
        Ui ui;
        const u32 root = ui.screen();
        const u32 img = ui.send(ui.klass("UIImage"), "imageNamed:", {ui.string("Menu")});
        REQUIRE(img != 0u);
        const u32 iv = ui.view("UIImageView", 20, 10, 64, 48);
        ui.send(iv, "setImage:", {img});
        ui.send(root, "addSubview:", {iv});
        mt_objc_release(&ui.cpu, iv);
        ui.turn();
        // At render scale 2 (960 x 640) the view is 128 x 96 pixels at (40, 20).
        const golden::Image full = ui.composite(MT_UIKIT_COMPOSITE_FULL, 960, 640);
        CHECK(full.at(100, 60) ==
              (hooked ? golden::Rgba{0, 255, 0, 255} : golden::Rgba{255, 0, 0, 255}));
        // The guest's image is the file's either way.
        double w = 0, h = 0;
        REQUIRE(mt_cg::uiimage_size(&ui.cpu, img, &w, &h));
        CHECK(w == 16);
        CHECK(h == 12);
    }
    mt_cg_set_image_replace_hook(nullptr, nullptr);
}
