// The runtime's host classes and host overrides (ADR 0026, ADR 0029). mt_objc_start adds
// these classes in this order (a superclass before its subclasses), then placeholders for
// the imported classes no one implements yet, then loads the image and installs the
// overrides. Framework issues (#49 Foundation, #51 UIImage/UIFont, #52 stubs, #54 UIKit) add
// one line per class here, like runtime/src/hle/registry.cpp for functions, and one line per
// host category (methods on another issue's class).
#include <cstdint>
#include <cstdio>

#include "objc/objc_internal.h"
#ifndef MT_HLE_CORE_ONLY
#include "cf/cf_hle.h"             // the bridged CF/Foundation classes (#48)
#include "foundation/foundation.h" // Foundation (#49)
#endif
#include "stubs/stubs_internal.h" // the offline service stubs (#52)
#ifndef MT_HLE_CORE_ONLY
#include "cg/cg_objects.h"        // UIKit drawing: UIImage, UIFont, UIColor, UIBezierPath (#51)
#include "uikit/uikit_internal.h" // UIKit: views, controls, the application (#54)
#endif

using namespace mt_objc_impl;

namespace {

// clang-format off
const mt_objc_host_class *const k_host_classes[] = {
    // libobjc / Foundation core (#47)
    &k_nsobject,
    &k_nsautoreleasepool,
    // the blocks runtime (#47): __NSStackBlock__ and __NSGlobalBlock__ use the imported
    // _NSConcreteStackBlock / _NSConcreteGlobalBlock objects as class objects
    &k_nsblock,
    &k_stack_block,
    &k_malloc_block,
    &k_global_block,
#ifndef MT_HLE_CORE_ONLY
    // CoreFoundation's bridged classes (runtime/src/cf/, #48, ADR 0032); Foundation (#49) adds
    // the NS selectors to their tables in runtime/src/cf/bridged_classes.cpp
    &mt_cf::k_nsstring,
    &mt_cf::k_nsmutablestring,
    &mt_cf::k_nscfstring,
    &mt_cf::k_nscfconstantstring,
    &mt_cf::k_nsdata,
    &mt_cf::k_nsmutabledata,
    &mt_cf::k_nscfdata,
    &mt_cf::k_nsvalue,
    &mt_cf::k_nsnumber,
    &mt_cf::k_nscfnumber,
    &mt_cf::k_nscfboolean,
    &mt_cf::k_nscftype,
    // Foundation (runtime/src/foundation/, #49, ADR 0035)
    &mt_ns::k_nsarray,
    &mt_ns::k_nsmutablearray,
    &mt_ns::k_nsdictionary,
    &mt_ns::k_nsmutabledictionary,
    &mt_ns::k_nsset,
    &mt_ns::k_nsnull,
    &mt_ns::k_nsbundle,
    &mt_ns::k_nslocale,
    &mt_ns::k_nsfilemanager,
    &mt_ns::k_nsfilehandle,
    &mt_ns::k_nsurl,
    &mt_ns::k_nserror,
    &mt_ns::k_nsthread,
#endif
    // the offline service stubs (#52, runtime/src/stubs/): GameKit, emulated offline
    &mt_stubs_impl::k_gk_player,
    &mt_stubs_impl::k_gk_local_player,
    &mt_stubs_impl::k_gk_score,
    &mt_stubs_impl::k_gk_achievement,
    &mt_stubs_impl::k_gk_leaderboard,
#ifdef MT_HLE_CORE_ONLY
    // a UIViewController: without UIKit (the core build) an NSObject; else listed after UIKit
    &mt_stubs_impl::k_gk_leaderboard_view_controller,
#endif
    // StoreKit: a store that sells nothing (ADR 0034)
    &mt_stubs_impl::k_sk_payment_queue,
    &mt_stubs_impl::k_sk_products_request,
    &mt_stubs_impl::k_sk_products_response,
    &mt_stubs_impl::k_sk_product,
    &mt_stubs_impl::k_sk_payment,
    &mt_stubs_impl::k_sk_payment_transaction,
#ifndef MT_HLE_CORE_ONLY
    // UIKit drawing (runtime/src/cg/, #51, ADR 0036)
    &mt_cg::k_uiimage,
    &mt_cg::k_uifont,
    &mt_cg::k_uicolor,
    &mt_cg::k_uibezierpath,
    // UIKit (runtime/src/uikit/, #54, ADR 0037)
    &mt_ui::k_uiresponder,
    &mt_ui::k_uiview,
    &mt_ui::k_uiwindow,
    &mt_ui::k_uiimageview,
    &mt_ui::k_uilabel,
    &mt_ui::k_uicontrol,
    &mt_ui::k_uibutton,
    &mt_ui::k_uipagecontrol,
    &mt_ui::k_uiscrollview,
    &mt_ui::k_uitextview,
    &mt_ui::k_uiactivityindicatorview,
    &mt_ui::k_calayer,
    &mt_ui::k_caeagllayer,
    &mt_ui::k_uiapplication,
    &mt_ui::k_uiviewcontroller,
    &mt_ui::k_uidevice,
    &mt_ui::k_uitouch,
    &mt_ui::k_uievent,
    &mt_ui::k_nscoder,
    &mt_ui::k_uinibdecoder,
    // GameKit's leaderboard controller (#52) is a UIViewController
    &mt_stubs_impl::k_gk_leaderboard_view_controller,
#endif
};

// Host categories: methods one issue adds to another issue's class, added after every class.
// The list ends with nullptr (it may otherwise be empty).
const mt_objc_host_category *const k_host_categories[] = {
#ifndef MT_HLE_CORE_ONLY
    // Foundation's selectors on NSObject (#47) and on CoreFoundation's bridged classes (#48,
    // ADR 0035)
    &mt_ns::k_nsobject_methods,
    &mt_ns::k_nsstring_methods,
    &mt_ns::k_nsmutablestring_methods,
    &mt_ns::k_nsdata_methods,
    &mt_ns::k_nsnumber_methods,
    // UIKit's NSString drawing selectors on Foundation's NSString (#51)
    &mt_cg::k_nsstring_drawing,
#endif
    nullptr,
};

// clang-format on

void return_nil(mt_cpu *cpu) {
    cpu->r[0] = 0;
    cpu->r[1] = 0;
}

struct Override {
    const char *cls;
    int class_method;
    const char *selector;
    mt_guest_fn fn;
    const char *reason;
};

// The only ways the port changes the game's own Objective-C behaviour (ADR 0026).
// +[GameCenterManager IsGameCenterAvailable] is not overridden: GameKit is emulated offline
// (#52, ADR 0029), so Game Center keeps working as the game expects. Nor is
// +[SKPaymentQueue canMakePayments]: StoreKit is a host class that answers YES and cancels
// every payment (ADR 0034), because with NO a purchase leaves "Please Wait..." up for ever.
const Override k_overrides[] = {
    {"Chartboost", 1, "sharedChartboost", return_nil,
     "ads off: every Chartboost send from the engine goes to nil (ADR 0026)"},
};

} // namespace

extern "C" const mt_objc_host_class *const *mt_objc_host_registry(std::uint32_t *count) noexcept {
    *count = static_cast<std::uint32_t>(sizeof k_host_classes / sizeof k_host_classes[0]);
    return k_host_classes;
}

extern "C" const mt_objc_host_category *const *
mt_objc_host_category_registry(std::uint32_t *count) noexcept {
    *count = static_cast<std::uint32_t>(sizeof k_host_categories / sizeof k_host_categories[0]) - 1;
    return k_host_categories;
}

extern "C" std::uint32_t mt_objc_install_overrides(mt_objc *rt) noexcept {
    std::uint32_t failed = 0;
    for (const Override &o : k_overrides) {
        if (mt_objc_override(rt, o.cls, o.class_method, o.selector, o.fn) != 0) {
            char line[256];
            std::snprintf(line, sizeof line, "objc: override %c[%s %s] (%s): no class %s",
                          o.class_method != 0 ? '+' : '-', o.cls, o.selector, o.reason, o.cls);
            log_line(rt, line);
            failed++;
        }
    }
    return failed;
}
