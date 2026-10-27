// The runtime's host classes and host overrides (ADR 0026, ADR 0029). mt_objc_start adds
// these classes in this order (a superclass before its subclasses), then placeholders for
// the imported classes no one implements yet, then loads the image and installs the
// overrides. Framework issues (#49 Foundation, #51 UIImage/UIFont, #52 stubs, #54 UIKit) add
// one line per class here, like runtime/src/hle/registry.cpp for functions.
#include <cstdint>
#include <cstdio>

#include "objc/objc_internal.h"
#ifndef MT_HLE_CORE_ONLY
#include "cf/cf_hle.h" // the bridged CF/Foundation classes (#48)
#endif
#include "stubs/stubs_internal.h" // the offline service stubs (#52)

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
#endif
    // the offline service stubs (#52, runtime/src/stubs/): GameKit, emulated offline
    &mt_stubs_impl::k_gk_player,
    &mt_stubs_impl::k_gk_local_player,
    &mt_stubs_impl::k_gk_score,
    &mt_stubs_impl::k_gk_achievement,
    &mt_stubs_impl::k_gk_leaderboard,
    &mt_stubs_impl::k_gk_leaderboard_view_controller,
    // StoreKit: a store that sells nothing (ADR 0034)
    &mt_stubs_impl::k_sk_payment_queue,
    &mt_stubs_impl::k_sk_products_request,
    &mt_stubs_impl::k_sk_products_response,
    &mt_stubs_impl::k_sk_product,
    &mt_stubs_impl::k_sk_payment,
    &mt_stubs_impl::k_sk_payment_transaction,
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
