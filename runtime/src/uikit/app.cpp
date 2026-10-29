// UIApplication, UIApplicationMain and MainWindow.nib (#54, ADR 0037); UIDevice and
// UIViewController; the module's per-runtime state.
//
// The boot (#43) hands the guest to mt_uikit_app_main at UIApplicationMain(argc, argv, nil,
// nil) in main. It does what UIKit did on iOS 4:
//   1. [[UIApplication alloc] init] (the principal class: nil -> UIApplication)
//   2. MainWindow.nib (Info.plist NSMainNibFile) with the application as File's Owner: the
//      UIWindow and its EAGLView by -initWithCoder: (-[EAGLView initWithCoder:] 0x4F6D starts
//      the engine), the MapleThiefAppDelegate by -init, then the outlets File's
//      Owner.delegate and delegate.window, and the visible window
//   3. -application:didFinishLaunchingWithOptions: (0x2BFD) with nil options, or
//      -applicationDidFinishLaunching:, then -applicationDidBecomeActive:
//   4. the main run loop: mt_uikit_turn + mt_runloop_run_once, then the host's turn hook
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

#include "cf/cf_objects.h"
#include "foundation/foundation.h"
#include "maplethief/boot.h"
#include "maplethief/cf.h"
#include "maplethief/hle/abi.h"
#include "objc/objc_internal.h"
#include "uikit/nib.h"
#include "uikit/uikit_internal.h"

namespace mt_ui {

namespace {

struct Module {
    State state;
    u32 binding = 0; // mt_cf::binding() of the runtime the state belongs to
    std::set<std::string> logged;
};
Module g_module;

// --- the nib decoder (UINibDecoder) ------------------------------------------------------------

constexpr u32 k_max_depth = 64; // nested -initWithCoder: (subviews): deeper nibs are refused

struct Decoding {
    nib::Archive archive;
    std::vector<u32> objects; // per archive object: the decoded guest object (retained), or 0
    std::vector<u8> status;   // 0 not decoded, 1 decoding, 2 decoded
    std::vector<u32> current; // the objects whose -initWithCoder: is running, innermost last
    u32 owner = 0;            // File's Owner
    u32 coder = 0;            // the UINibDecoder guest object
};

const mt_objc_state_type k_decoder_state = {"UINibDecoder", sizeof(Decoding *), nullptr};

Decoding *decoding_of(mt_cpu *cpu, u32 coder) {
    mt_objc *rt = runtime(cpu);
    void *s =
        rt != nullptr && coder != 0 ? mt_objc_find_state(rt, coder, &k_decoder_state) : nullptr;
    Decoding *d = s != nullptr ? *static_cast<Decoding **>(s) : nullptr;
    return d != nullptr && !d->current.empty() ? d : nullptr;
}

const nib::Value *coder_value(mt_cpu *cpu, u32 coder, const char *key, Decoding **out) {
    Decoding *d = decoding_of(cpu, coder);
    if (out != nullptr) {
        *out = d;
    }
    return d != nullptr ? d->archive.find(d->current.back(), key) : nullptr;
}

// The UTF-8 text of an NSString object of the archive (its NS.bytes), without a guest object.
bool nib_string(const nib::Archive &a, u32 index, std::string *out) {
    const std::string &cls = a.class_name(index);
    if (cls != "NSString" && cls != "NSMutableString") {
        return false;
    }
    const nib::Value *v = a.find(index, "NS.bytes");
    if (v == nullptr || v->type != nib::k_data) {
        return false;
    }
    out->assign(reinterpret_cast<const char *>(a.data(*v)), v->data_length);
    return true;
}

// The text of the NSString a key of object `index` refers to.
bool nib_string_at(const nib::Archive &a, u32 index, const char *key, std::string *out) {
    const nib::Value *v = a.find(index, key);
    return v != nullptr && v->type == nib::k_object && nib_string(a, v->object, out);
}

double nib_number(const nib::Archive &a, u32 index, const char *key, double fallback) {
    const nib::Value *v = a.find(index, key);
    double n = fallback;
    return v != nullptr && a.number(*v, &n) ? n : fallback;
}

u32 decode(mt_cpu *cpu, Decoding &d, u32 index);

// [[cls alloc] initWithCoder:coder] for object `index` (or -init if the class does not
// respond to -initWithCoder:, like an NSObject subclass from a UICustomObject).
u32 instantiate(mt_cpu *cpu, Decoding &d, u32 index, u32 cls, bool with_coder) {
    u32 obj = mt_objc_send0(cpu, cls, "alloc");
    if (obj == 0) {
        return 0;
    }
    if (with_coder && responds(cpu, obj, "initWithCoder:")) {
        d.current.push_back(index);
        obj = mt_objc_send1(cpu, obj, "initWithCoder:", d.coder);
        d.current.pop_back();
    } else {
        obj = mt_objc_send0(cpu, obj, "init");
    }
    return obj;
}

u32 decode_color(mt_cpu *cpu, const nib::Archive &a, u32 index) {
    float c[4] = {0, 0, 0, 1};
    std::string name;
    if (nib_string_at(a, index, "UISystemColorName", &name)) {
        if (name == "whiteColor") {
            c[0] = c[1] = c[2] = 1;
        } else if (name == "clearColor") {
            c[3] = 0;
        } else if (name != "blackColor") {
            log(cpu, "MainWindow.nib: system colour %s is not known: black", name.c_str());
        }
    } else if (a.find(index, "UIWhite") != nullptr) {
        c[0] = c[1] = c[2] = static_cast<float>(nib_number(a, index, "UIWhite", 0));
        c[3] = static_cast<float>(nib_number(a, index, "UIAlpha", 1));
    } else {
        c[0] = static_cast<float>(nib_number(a, index, "UIRed", 0));
        c[1] = static_cast<float>(nib_number(a, index, "UIGreen", 0));
        c[2] = static_cast<float>(nib_number(a, index, "UIBlue", 0));
        c[3] = static_cast<float>(nib_number(a, index, "UIAlpha", 1));
    }
    mt_objc *rt = runtime(cpu);
    const u32 cls = mt_objc_class_named(rt, "UIColor");
    const u32 obj = cls != 0 ? mt_objc_send0(cpu, cls, "alloc") : 0u;
    const u32 args[4] = {mt_f32_bits(c[0]), mt_f32_bits(c[1]), mt_f32_bits(c[2]),
                         mt_f32_bits(c[3])};
    return obj != 0
               ? mt_objc_msg(cpu, obj, mt_objc_sel(rt, "initWithRed:green:blue:alpha:"), args, 4)
               : 0u;
}

// Decodes archive object `index` once (cycles and nesting deeper than k_max_depth give nil,
// logged). The Decoding keeps a reference to the result.
u32 decode(mt_cpu *cpu, Decoding &d, u32 index) {
    const nib::Archive &a = d.archive;
    if (index >= a.objects.size()) {
        return 0;
    }
    if (d.status[index] == 2) {
        return d.objects[index];
    }
    if (d.status[index] == 1 || d.current.size() >= k_max_depth) {
        log(cpu, "MainWindow.nib: object %u refers back to itself or nests too deep: nil",
            static_cast<unsigned>(index));
        return 0;
    }
    d.status[index] = 1;
    mt_objc *rt = runtime(cpu);
    const std::string &cls_name = a.class_name(index);
    u32 obj = 0;
    std::string text;
    if (cls_name == "NSString" || cls_name == "NSMutableString") {
        if (nib_string(a, index, &text)) {
            obj = mt_ns::string_from_utf8(cpu, text);
        }
    } else if (cls_name == "NSArray" || cls_name == "NSMutableArray") {
        std::vector<u32> items;
        for (const u32 e : a.elements(index)) {
            if (const u32 o = decode(cpu, d, e)) {
                items.push_back(o);
            }
        }
        obj = mt_ns::array_create(cpu, items.empty() ? nullptr : items.data(),
                                  static_cast<u32>(items.size()), cls_name == "NSMutableArray");
    } else if (cls_name == "UIColor") {
        obj = decode_color(cpu, a, index);
    } else if (cls_name == "UIProxyObject") {
        nib_string_at(a, index, "UIProxiedObjectIdentifier", &text);
        if (text == "IBFilesOwner") {
            obj = mt_objc_retain(cpu, d.owner);
        } else if (text != "IBFirstResponder") {
            log(cpu, "MainWindow.nib: proxy %s is not known: nil", text.c_str());
        }
    } else if (cls_name == "UIRuntimeOutletConnection" || cls_name == "UIRuntimeEventConnection") {
        // Connections are made after every object exists (load_nib).
    } else if (cls_name == "UIClassSwapper") {
        std::string name, original;
        nib_string_at(a, index, "UIClassName", &name);
        nib_string_at(a, index, "UIOriginalClassName", &original);
        u32 cls = mt_objc_class_named(rt, name.c_str());
        if (cls == 0 || mt_objc_class_is_placeholder(rt, cls) != 0) {
            log(cpu, "MainWindow.nib: class %s is unknown, using %s", name.c_str(),
                original.c_str());
            cls = original == "UICustomObject" ? mt_objc_class_named(rt, "NSObject")
                                               : mt_objc_class_named(rt, original.c_str());
        }
        if (cls != 0) {
            obj = instantiate(cpu, d, index, cls, original != "UICustomObject");
        }
    } else if (cls_name == "UICustomObject") {
        obj = instantiate(cpu, d, index, mt_objc_class_named(rt, "NSObject"), false);
    } else {
        const u32 cls = mt_objc_class_named(rt, cls_name.c_str());
        if (cls == 0 || mt_objc_class_is_placeholder(rt, cls) != 0) {
            log(cpu, "MainWindow.nib: object %u of class %s: no such class: nil",
                static_cast<unsigned>(index), cls_name.c_str());
        } else {
            obj = instantiate(cpu, d, index, cls, true);
        }
    }
    d.objects[index] = obj;
    d.status[index] = 2;
    return obj;
}

// [source setValue:destination forKey:label] for an outlet: -set<Label>: if the source has it.
void connect_outlet(mt_cpu *cpu, u32 source, const std::string &label, u32 destination) {
    if (source == 0 || label.empty()) {
        return;
    }
    std::string setter = "set" + label + ":";
    setter[3] = static_cast<char>(std::toupper(static_cast<unsigned char>(setter[3])));
    if (!responds(cpu, source, setter.c_str())) {
        log(cpu, "MainWindow.nib: outlet %s of 0x%08X (%s): no -%s, not connected", label.c_str(),
            static_cast<unsigned>(source),
            mt_objc_class_name(runtime(cpu), mt_objc_class_of(runtime(cpu), source)),
            setter.c_str());
        return;
    }
    mt_objc_send1(cpu, source, setter.c_str(), destination);
}

struct NibResult {
    u32 archive_objects = 0; // objects of the archive
    u32 objects = 0;         // guest objects made from them (strings and nil proxies aside)
    u32 window = 0;
};

// Loads the bundle's nib `name` with `owner` as File's Owner (see the file comment). False,
// with the reason, if the file is missing or no NIBArchive.
bool load_nib(mt_cpu *cpu, const std::string &name, u32 owner, NibResult *result,
              std::string *error) {
    const std::string rel = mt_cf::bundle_find(cpu, name, "nib", "", "");
    std::vector<u8> bytes;
    if (rel.empty() || !mt_cf::bundle_read(rel, bytes)) {
        *error = name + ".nib is not in the bundle";
        return false;
    }
    Decoding d;
    std::string why;
    if (!nib::parse(bytes.data(), bytes.size(), &d.archive, &why)) {
        *error = rel + ": " + why;
        return false;
    }
    const nib::Archive &a = d.archive;
    if (a.objects.empty()) {
        *error = rel + ": no root object";
        return false;
    }
    mt_objc *rt = runtime(cpu);
    result->archive_objects = static_cast<u32>(a.objects.size());
    d.objects.assign(a.objects.size(), 0);
    d.status.assign(a.objects.size(), 0);
    d.owner = owner;
    d.coder = mt_objc_create_instance(rt, mt_objc_class_named(rt, "UINibDecoder"));
    if (d.coder == 0) {
        *error = "no UINibDecoder";
        return false;
    }
    Decoding *dp = &d;
    *static_cast<Decoding **>(mt_objc_state(rt, d.coder, &k_decoder_state)) = dp;

    auto root_array = [&](const char *key) {
        const nib::Value *v = a.find(0, key);
        return v != nullptr && v->type == nib::k_object ? a.elements(v->object)
                                                        : std::vector<u32>();
    };
    const std::vector<u32> top = root_array("UINibTopLevelObjectsKey");
    const std::vector<u32> all = root_array("UINibObjectsKey");
    for (const u32 i : top) {
        decode(cpu, d, i);
    }
    for (const u32 i : all) {
        decode(cpu, d, i);
    }
    for (const u32 c : root_array("UINibConnectionsKey")) {
        std::string label;
        nib_string_at(a, c, "UILabel", &label);
        const nib::Value *src = a.find(c, "UISource"), *dst = a.find(c, "UIDestination");
        if (a.class_name(c) != "UIRuntimeOutletConnection" || src == nullptr || dst == nullptr ||
            src->type != nib::k_object || dst->type != nib::k_object) {
            log(cpu, "MainWindow.nib: connection %u (%s) is not supported: not connected",
                static_cast<unsigned>(c), a.class_name(c).c_str());
            continue;
        }
        connect_outlet(cpu, decode(cpu, d, src->object), label, decode(cpu, d, dst->object));
    }
    State &st = state(cpu);
    for (const u32 w : root_array("UINibVisibleWindowsKey")) {
        const u32 obj = decode(cpu, d, w);
        if (obj != 0 && is_kind(cpu, obj, "UIWindow")) {
            st.windows.push_back(mt_objc_retain(cpu, obj));
            if (result->window == 0) {
                result->window = obj;
            }
        }
    }
    for (const u32 i : all) {
        const u32 obj = i < d.objects.size() ? d.objects[i] : 0u;
        if (obj != 0 && obj != owner && responds(cpu, obj, "awakeFromNib")) {
            mt_objc_send0(cpu, obj, "awakeFromNib");
        }
    }
    // The application keeps the top-level objects (the delegate and the window) for ever.
    for (const u32 i : top) {
        if (const u32 obj = d.objects[i]) {
            st.nib_objects.push_back(mt_objc_retain(cpu, obj));
        }
    }
    for (u32 &obj : d.objects) {
        if (obj != 0) {
            result->objects++;
        }
        mt_objc_release(cpu, obj);
        obj = 0;
    }
    *static_cast<Decoding **>(mt_objc_state(rt, d.coder, &k_decoder_state)) = nullptr;
    mt_objc_release(cpu, d.coder);
    return true;
}

// --- UIApplication -------------------------------------------------------------------------------

// +[UIApplication sharedApplication]
void app_shared(mt_cpu *cpu) { mt_ret_u32(cpu, state(cpu).app); }

void app_delegate(mt_cpu *cpu) { mt_ret_u32(cpu, state(cpu).delegate); }

// -[UIApplication setDelegate:]: not retained (UIKit's delegate is assign).
void app_set_delegate(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    state(cpu).delegate = mt_arg_u32(&a);
}

// -[UIApplication openURL:(NSURL *)url]: the host's open_url hook (MainMenuView's More Games,
// About links; docs/re/network.md#urls).
void app_open_url(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const u32 url = mt_arg_u32(&a);
    std::string text;
    bool is_file = false;
    if (!mt_ns::url_string(cpu, url, &text, &is_file)) {
        log(cpu, "-[UIApplication openURL:]: 0x%08X is no NSURL: NO", static_cast<unsigned>(url));
        mt_ret_u32(cpu, 0);
        return;
    }
    const State &st = state(cpu);
    const int opened =
        st.config.open_url != nullptr ? st.config.open_url(st.config.user, text.c_str()) : 0;
    log(cpu, "-[UIApplication openURL:] %s: %s", text.c_str(),
        opened != 0 ? "opened" : "not opened (no handler)");
    mt_ret_u32(cpu, opened != 0 ? 1u : 0u);
}

// -[UIApplication registerForRemoteNotificationTypes:]: no push service offline; UIKit never
// answers (neither -didRegister... nor -didFail...), as on a device without one.
void app_register_remote(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const u32 types = mt_arg_u32(&a);
    log_once(cpu, "registerForRemoteNotificationTypes",
             "-[UIApplication registerForRemoteNotificationTypes:%u]: no push service: ignored",
             static_cast<unsigned>(types));
}

void app_set_badge(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    state(cpu).badge = mt_arg_u32(&a);
}

void app_key_window(mt_cpu *cpu) {
    const State &st = state(cpu);
    mt_ret_u32(cpu, st.windows.empty() ? 0u : st.windows.back());
}

// --- UIDevice --------------------------------------------------------------------------------

// What the game reads (-[EAGLView initWithCoder:] → Device_SetModel; CStateTitle's hello
// packet; +[GameCenterManager IsGameCenterAvailable] needs systemVersion >= 4.1): an iPhone on
// the last iOS 4 release.
constexpr const char *k_device_model = "iPhone";
constexpr const char *k_device_name = "iPhone";
constexpr const char *k_system_name = "iPhone OS";
constexpr const char *k_system_version = "4.3.5";

void device_current(mt_cpu *cpu) {
    State &st = state(cpu);
    if (st.device == 0) {
        mt_objc *rt = runtime(cpu);
        st.device = mt_objc_create_instance(rt, mt_objc_class_named(rt, "UIDevice"));
    }
    mt_ret_u32(cpu, st.device);
}

void ret_string(mt_cpu *cpu, const char *text) {
    mt_ret_u32(cpu, mt_objc_autorelease(cpu, mt_ns::string_from_utf8(cpu, text)));
}
void device_model(mt_cpu *cpu) { ret_string(cpu, k_device_model); }
void device_name(mt_cpu *cpu) { ret_string(cpu, k_device_name); }
void device_system_name(mt_cpu *cpu) { ret_string(cpu, k_system_name); }
void device_system_version(mt_cpu *cpu) { ret_string(cpu, k_system_version); }

// --- UIViewController ----------------------------------------------------------------------------

struct Controller {
    u32 view;      // retained
    u32 modal;     // the presented controller, retained
    u32 presenter; // who presented this one, not retained
};

void controller_destroy(mt_cpu *cpu, u32, void *block) {
    auto *c = static_cast<Controller *>(block);
    mt_objc_release(cpu, c->view);
    mt_objc_release(cpu, c->modal);
}
const mt_objc_state_type k_controller_state = {"UIViewController", sizeof(Controller),
                                               controller_destroy};

Controller *controller(mt_cpu *cpu, u32 obj) {
    return obj != 0
               ? static_cast<Controller *>(mt_objc_state(runtime(cpu), obj, &k_controller_state))
               : nullptr;
}

// -[UIViewController initWithNibName:(NSString *)name bundle:(NSBundle *)bundle]: the view is
// set by the caller (-[MapleThiefAppDelegate application:didFinishLaunchingWithOptions:] sets
// the EAGLView); a missing nib ("EAGLViewController" is not in the bundle) would only matter
// when -view loads it, which the game never makes it do.
void controller_init_with_nib(mt_cpu *cpu) { mt_ret_u32(cpu, mt_objc_self(cpu)); }

void controller_init(mt_cpu *cpu) {
    const u32 self = mt_objc_self(cpu);
    mt_ret_u32(cpu, mt_objc_send2(cpu, self, "initWithNibName:bundle:", 0, 0));
}

// -[UIViewController view]: a plain full-screen view, made (and -viewDidLoad sent) on first use.
void controller_view(mt_cpu *cpu) {
    const u32 self = mt_objc_self(cpu);
    Controller *c = controller(cpu, self);
    if (c != nullptr && c->view == 0) {
        mt_objc *rt = runtime(cpu);
        u32 view = mt_objc_send0(cpu, mt_objc_class_named(rt, "UIView"), "alloc");
        const u32 frame[4] = {0, 0, mt_f32_bits(320), mt_f32_bits(480)};
        view = mt_objc_msg(cpu, view, mt_objc_sel(rt, "initWithFrame:"), frame, 4);
        c = controller(cpu, self);
        if (c != nullptr) {
            c->view = view;
            mt_objc_send0(cpu, self, "viewDidLoad");
        }
        c = controller(cpu, self);
    }
    mt_ret_u32(cpu, c != nullptr ? c->view : 0u);
}

void controller_set_view(mt_cpu *cpu) {
    const u32 self = mt_objc_self(cpu);
    mt_args a = mt_objc_args(cpu);
    const u32 view = mt_arg_u32(&a);
    if (Controller *c = controller(cpu, self)) {
        assign(cpu, &c->view, view);
    }
}

// -[UIViewController presentModalViewController:(UIViewController *)vc animated:(BOOL)a]: the
// controller is presented and gets -viewDidAppear: at the next UIKit turn. Its view is not
// shown: the only one the game presents is GameKit's leaderboard (-[EAGLView
// viewGameCenterRank:] 0x4E09), which the offline GameKit (#52) dismisses when it appears.
void controller_present(mt_cpu *cpu) {
    const u32 self = mt_objc_self(cpu);
    mt_args a = mt_objc_args(cpu);
    const u32 vc = mt_arg_u32(&a);
    Controller *c = controller(cpu, self);
    Controller *p = controller(cpu, vc);
    if (c == nullptr || p == nullptr) {
        return;
    }
    if (c->modal != 0) {
        log(cpu,
            "-[UIViewController presentModalViewController:animated:]: 0x%08X already "
            "presents 0x%08X: ignored",
            static_cast<unsigned>(self), static_cast<unsigned>(c->modal));
        return;
    }
    p->presenter = self;
    c->modal = mt_objc_retain(cpu, vc);
    state(cpu).presented.push_back(mt_objc_retain(cpu, vc));
}

// -[UIViewController dismissModalViewControllerAnimated:]: sent to the presenting controller (or
// to the presented one, which passes it on).
void controller_dismiss(mt_cpu *cpu) {
    const u32 self = mt_objc_self(cpu);
    Controller *c = controller(cpu, self);
    if (c != nullptr && c->modal == 0 && c->presenter != 0) {
        c = controller(cpu, c->presenter);
    }
    if (c == nullptr || c->modal == 0) {
        return;
    }
    const u32 modal = c->modal;
    c->modal = 0;
    if (Controller *m = controller(cpu, modal)) {
        m->presenter = 0;
    }
    mt_objc_release(cpu, modal);
}

// UIViewController's own -viewDidLoad etc.: nothing (subclasses send them to super).
void controller_nothing(mt_cpu *) {}

// --- class tables --------------------------------------------------------------------------------

const mt_objc_host_method k_app_class_methods[] = {
    {"sharedApplication", app_shared},
};
const mt_objc_host_method k_app_methods[] = {
    {"delegate", app_delegate},
    {"setDelegate:", app_set_delegate},
    {"openURL:", app_open_url},
    {"registerForRemoteNotificationTypes:", app_register_remote},
    {"setApplicationIconBadgeNumber:", app_set_badge},
    {"keyWindow", app_key_window},
};
const mt_objc_host_method k_device_class_methods[] = {
    {"currentDevice", device_current},
};
const mt_objc_host_method k_device_methods[] = {
    {"model", device_model},
    {"name", device_name},
    {"systemName", device_system_name},
    {"systemVersion", device_system_version},
};
const mt_objc_host_method k_controller_methods[] = {
    {"init", controller_init},
    {"initWithNibName:bundle:", controller_init_with_nib},
    {"view", controller_view},
    {"setView:", controller_set_view},
    {"presentModalViewController:animated:", controller_present},
    {"dismissModalViewControllerAnimated:", controller_dismiss},
    {"viewDidLoad", controller_nothing},
    {"viewDidUnload", controller_nothing},
    {"viewDidAppear:", controller_nothing},
    {"didReceiveMemoryWarning", controller_nothing},
};

template <typename T, std::size_t N> constexpr u32 countof(const T (&)[N]) {
    return static_cast<u32>(N);
}

// --- the run
// --------------------------------------------------------------------------------------

// The launch callback the delegate implements, sent with the nib's window in place.
void launch_delegate(mt_cpu *cpu, u32 window) {
    State &st = state(cpu);
    const u32 delegate = st.delegate;
    char detail[200];
    const char *cls =
        delegate != 0 ? mt_objc_class_name(runtime(cpu), mt_objc_class_of(runtime(cpu), delegate))
                      : "nil";
    if (delegate != 0 && responds(cpu, delegate, "application:didFinishLaunchingWithOptions:")) {
        std::snprintf(detail, sizeof detail,
                      "-[%s application:didFinishLaunchingWithOptions:] (window 0x%08X)", cls,
                      static_cast<unsigned>(window));
        mt_process_milestone("app-delegate", delegate, detail);
        mt_objc_send2(cpu, delegate, "application:didFinishLaunchingWithOptions:", st.app, 0);
    } else if (delegate != 0 && responds(cpu, delegate, "applicationDidFinishLaunching:")) {
        std::snprintf(detail, sizeof detail, "-[%s applicationDidFinishLaunching:] (window 0x%08X)",
                      cls, static_cast<unsigned>(window));
        mt_process_milestone("app-delegate", delegate, detail);
        mt_objc_send1(cpu, delegate, "applicationDidFinishLaunching:", st.app);
    } else {
        std::snprintf(detail, sizeof detail, "the delegate (%s) has no launch callback", cls);
        mt_process_milestone("app-delegate", delegate, detail);
    }
    mt_uikit_lifecycle_event(cpu, MT_UIKIT_DID_BECOME_ACTIVE);
}

} // namespace

const char *launch(mt_cpu *cpu, const mt_app_main_args *args) {
    State &st = state(cpu);
    mt_objc *rt = runtime(cpu);
    const u32 pool = mt_objc_pool_push(rt);
    u32 cls = mt_objc_class_named(rt, "UIApplication");
    if (args != nullptr && args->principal_class_name != 0) {
        const std::string name = mt_ns::string_utf8(cpu, args->principal_class_name);
        const u32 named = mt_objc_class_named(rt, name.c_str());
        if (named != 0) {
            cls = named;
        } else {
            log(cpu, "UIApplicationMain: principal class %s not found: UIApplication",
                name.c_str());
        }
    }
    u32 app = mt_objc_send0(cpu, cls, "alloc");
    app = app != 0 ? mt_objc_send0(cpu, app, "init") : 0u;
    st.app = app;
    if (app == 0) {
        mt_objc_pool_pop(rt, pool);
        return "UIApplicationMain: the application could not be created";
    }
    // Info.plist's NSMainNibFile.
    std::string nib_name = "MainWindow";
    const u32 bundle = mt_objc_send0(cpu, mt_objc_class_named(rt, "NSBundle"), "mainBundle");
    const u32 key = mt_ns::string_from_utf8(cpu, "NSMainNibFile");
    const u32 value =
        bundle != 0 ? mt_objc_send1(cpu, bundle, "objectForInfoDictionaryKey:", key) : 0u;
    mt_objc_release(cpu, key);
    if (value != 0) {
        nib_name = mt_ns::string_utf8(cpu, value);
    }
    NibResult nib;
    std::string error;
    if (!load_nib(cpu, nib_name, app, &nib, &error)) {
        log(cpu, "UIApplicationMain: %s", error.c_str());
        mt_objc_pool_pop(rt, pool);
        return "UIApplicationMain: the main nib could not be loaded";
    }
    char detail[160];
    std::snprintf(detail, sizeof detail,
                  "%s.nib: %u archive objects -> %u guest objects, window 0x%08X, delegate "
                  "0x%08X",
                  nib_name.c_str(), static_cast<unsigned>(nib.archive_objects),
                  static_cast<unsigned>(nib.objects), static_cast<unsigned>(nib.window),
                  static_cast<unsigned>(st.delegate));
    mt_process_milestone("nib", nib.archive_objects, detail);
    if (args != nullptr && args->delegate_class_name != 0) {
        const std::string name = mt_ns::string_utf8(cpu, args->delegate_class_name);
        const u32 dcls = mt_objc_class_named(rt, name.c_str());
        u32 delegate = dcls != 0 ? mt_objc_send0(cpu, dcls, "alloc") : 0u;
        delegate = delegate != 0 ? mt_objc_send0(cpu, delegate, "init") : 0u;
        if (delegate != 0) {
            st.nib_objects.push_back(delegate);
            st.delegate = delegate;
        }
    }
    launch_delegate(cpu, nib.window);
    st.launched = true;
    mt_objc_pool_pop(rt, pool);
    return nullptr;
}

// --- shared ------------------------------------------------------------------------------------

mt_objc *runtime(mt_cpu *cpu) { return mt_cf::runtime(cpu); }

State &state(mt_cpu *cpu) {
    mt_objc *rt = runtime(cpu);
    const u32 binding = mt_cf::binding();
    if (g_module.state.rt != rt || g_module.binding != binding) {
        // A new runtime (process or test, also one at the old one's host address): the old
        // one's objects are gone with it.
        const mt_uikit_config config = g_module.state.config;
        g_module.state = State();
        g_module.state.rt = rt;
        g_module.binding = binding;
        g_module.state.config = config;
        g_module.logged.clear();
        touch_reset();
    }
    return g_module.state;
}

double now(mt_cpu *cpu) {
    const mt_uikit_config &c = state(cpu).config;
    return c.clock != nullptr ? c.clock(c.user) : mt_cf_absolute_time();
}

bool responds(mt_cpu *cpu, u32 obj, const char *selector) {
    mt_objc *rt = runtime(cpu);
    if (rt == nullptr || obj == 0) {
        return false;
    }
    mt_objc_method m{};
    return mt_objc_lookup(rt, mt_objc_class_of(rt, obj), mt_objc_sel(rt, selector), &m) != 0;
}

bool overrides(mt_cpu *cpu, u32 obj, const char *selector, const char *base) {
    mt_objc *rt = runtime(cpu);
    if (rt == nullptr || obj == 0) {
        return false;
    }
    mt_objc_method m{};
    return mt_objc_lookup(rt, mt_objc_class_of(rt, obj), mt_objc_sel(rt, selector), &m) != 0 &&
           m.owner != mt_objc_class_named(rt, base);
}

std::u16string string_text(mt_cpu *cpu, u32 str) {
    if (str == 0) {
        return std::u16string();
    }
    if (const std::u16string *chars = mt_cf::string_chars(cpu, str)) {
        return *chars;
    }
    const std::string utf8 = mt_ns::string_utf8(cpu, str);
    std::u16string out;
    mt_cf::decode(reinterpret_cast<const std::uint8_t *>(utf8.data()), utf8.size(),
                  0x08000100u /* kCFStringEncodingUTF8 */, out);
    return out;
}

bool uicolor_value(mt_cpu *cpu, u32 color, Color *out) {
    if (color == 0) {
        return false;
    }
    const u32 cg = mt_objc_send0(cpu, color, "CGColor");
    return cg != 0 && mt_cg::color_value(cpu, cg, out);
}

void assign(mt_cpu *cpu, u32 *slot, u32 value) {
    const u32 old = *slot;
    *slot = mt_objc_retain(cpu, value);
    mt_objc_release(cpu, old);
}

void log(mt_cpu *cpu, const char *fmt, ...) noexcept {
    char line[512] = "uikit: ";
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line + 7, sizeof line - 7, fmt, ap);
    va_end(ap);
    mt_objc *rt = runtime(cpu);
    if (rt != nullptr) {
        mt_objc_impl::log_line(rt, line);
    } else {
        std::fprintf(stderr, "%s\n", line);
    }
}

void log_once(mt_cpu *cpu, const char *key, const char *fmt, ...) noexcept {
    state(cpu);
    if (!g_module.logged.insert(key).second) {
        return;
    }
    char text[480];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    log(cpu, "%s", text);
}

bool coder_bool(mt_cpu *cpu, u32 coder, const char *key, bool *out) {
    const nib::Value *v = coder_value(cpu, coder, key, nullptr);
    if (v == nullptr ||
        (v->type != nib::k_true && v->type != nib::k_false && v->type > nib::k_i64)) {
        return false;
    }
    *out = v->integer != 0;
    return true;
}

bool coder_number(mt_cpu *cpu, u32 coder, const char *key, double *out) {
    Decoding *d = nullptr;
    const nib::Value *v = coder_value(cpu, coder, key, &d);
    return v != nullptr && d->archive.number(*v, out);
}

bool coder_floats(mt_cpu *cpu, u32 coder, const char *key, float *out, u32 count) {
    Decoding *d = nullptr;
    const nib::Value *v = coder_value(cpu, coder, key, &d);
    if (v == nullptr) {
        return false;
    }
    if (!d->archive.floats(*v, out, count)) {
        log(cpu, "MainWindow.nib: %s of object %u is no geometry of %u floats: ignored", key,
            static_cast<unsigned>(d->current.back()), static_cast<unsigned>(count));
        return false;
    }
    return true;
}

u32 coder_object(mt_cpu *cpu, u32 coder, const char *key) {
    Decoding *d = nullptr;
    const nib::Value *v = coder_value(cpu, coder, key, &d);
    return v != nullptr && v->type == nib::k_object ? decode(cpu, *d, v->object) : 0u;
}

std::vector<u32> coder_objects(mt_cpu *cpu, u32 coder, const char *key) {
    Decoding *d = nullptr;
    const nib::Value *v = coder_value(cpu, coder, key, &d);
    std::vector<u32> out;
    if (v == nullptr || v->type != nib::k_object) {
        return out;
    }
    for (const u32 e : d->archive.elements(v->object)) {
        if (const u32 obj = decode(cpu, *d, e)) {
            out.push_back(obj);
        }
    }
    return out;
}

std::vector<std::string> coder_keys(mt_cpu *cpu, u32 coder) {
    Decoding *d = decoding_of(cpu, coder);
    std::vector<std::string> out;
    if (d == nullptr) {
        return out;
    }
    const nib::Object &o = d->archive.objects[d->current.back()];
    for (u32 i = 0; i < o.count; i++) {
        out.push_back(d->archive.keys[d->archive.values[o.first + i].key]);
    }
    return out;
}

// --- the classes ---------------------------------------------------------------------------------

const mt_objc_host_class k_uiapplication = {"UIApplication",
                                            "UIResponder",
                                            4,
                                            k_app_methods,
                                            countof(k_app_methods),
                                            k_app_class_methods,
                                            countof(k_app_class_methods)};
const mt_objc_host_class k_uidevice = {"UIDevice",
                                       "NSObject",
                                       4,
                                       k_device_methods,
                                       countof(k_device_methods),
                                       k_device_class_methods,
                                       countof(k_device_class_methods)};
// UIViewController: the SDK's 152 bytes (EAGLViewController's ivars start after them).
const mt_objc_host_class k_uiviewcontroller = {"UIViewController",
                                               "UIResponder",
                                               152,
                                               k_controller_methods,
                                               countof(k_controller_methods),
                                               nullptr,
                                               0};
const mt_objc_host_class k_nscoder = {"NSCoder", "NSObject", 4, nullptr, 0, nullptr, 0};
const mt_objc_host_class k_uinibdecoder = {"UINibDecoder", "NSCoder", 4, nullptr, 0, nullptr, 0};

} // namespace mt_ui

// --- the public API ------------------------------------------------------------------------------

using namespace mt_ui;

extern "C" mt_uikit_config mt_uikit_config_default(void) noexcept {
    mt_uikit_config c{};
    c.clock = nullptr;
    c.turn = nullptr;
    c.open_url = nullptr;
    c.user = nullptr;
    return c;
}

extern "C" int mt_uikit_lifecycle_event(mt_cpu *cpu, std::uint32_t event) noexcept {
    static const char *const k_selectors[] = {
        "applicationWillResignActive:", "applicationDidBecomeActive:", "applicationWillTerminate:"};
    if (event >= sizeof k_selectors / sizeof k_selectors[0]) {
        return 0;
    }
    const State &st = state(cpu);
    if (st.delegate == 0 || !responds(cpu, st.delegate, k_selectors[event])) {
        return 0;
    }
    mt_objc_send1(cpu, st.delegate, k_selectors[event], st.app);
    return 1;
}

extern "C" void mt_uikit_turn(mt_cpu *cpu, double t) noexcept {
    State &st = state(cpu);
    mt_objc *rt = runtime(cpu);
    if (rt == nullptr) {
        return;
    }
    st.last_turn = t;
    const u32 pool = mt_objc_pool_push(rt);
    deliver_touches(cpu, t);
    advance_scrolling(cpu, t);
    // Presented view controllers appear.
    std::vector<u32> presented = std::move(state(cpu).presented);
    state(cpu).presented.clear();
    for (const u32 vc : presented) {
        if (responds(cpu, vc, "viewDidAppear:")) {
            mt_objc_send1(cpu, vc, "viewDidAppear:", 1);
        }
        mt_objc_release(cpu, vc);
    }
    // Finished animations go; then the views draw.
    std::vector<u32> stack(state(cpu).windows);
    while (!stack.empty()) {
        View *v = view_of(cpu, stack.back());
        stack.pop_back();
        if (v != nullptr) {
            prune_animations(*v, t);
            stack.insert(stack.end(), v->subviews.begin(), v->subviews.end());
        }
    }
    display_views(cpu);
    mt_objc_pool_pop(rt, pool);
}

extern "C" std::int32_t mt_uikit_app_main(mt_process *, mt_cpu *cpu, const mt_app_main_args *args,
                                          void *user) noexcept {
    mt_objc *rt = runtime(cpu);
    State &st = state(cpu);
    st.config =
        user != nullptr ? *static_cast<const mt_uikit_config *>(user) : mt_uikit_config_default();
    (void)rt;
    if (const char *failure = launch(cpu, args)) {
        mt_process_stop(cpu, failure);
    }
    if (state(cpu).config.turn == nullptr) {
        mt_process_stop(cpu, "UIApplicationMain: launched; no turn hook installed");
    }
    for (;;) {
        const double t = now(cpu);
        mt_uikit_turn(cpu, t);
        const double next = mt_runloop_run_once(cpu, t);
        const mt_uikit_config &c = state(cpu).config;
        if (c.turn(c.user, cpu, t, next) != 0) {
            mt_process_stop(cpu, "UIApplicationMain: the host ended the run loop");
        }
    }
}
