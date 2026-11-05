// The registry of host state serializers and module sections (#202, ADR 0105). Each entry lives
// next to its state type, in the type's own file; this list only names them. A new kind of host
// state adds its line here (snapshot_internal.h has the recipe); the census
// (mt_snapshot_missing, play.save_state_census) names every live type that has none.
#include <cstring>

#include "snapshot/snapshot_internal.h"

namespace mt_cf {
extern const mt_snap::StateOps k_snap_string, k_snap_number, k_snap_data, k_snap_url,
    k_snap_bundle, k_snap_timer, k_snap_runloop;
extern const mt_snap::Module k_snap_cf, k_snap_cf_bundle, k_snap_cf_runloop;
} // namespace mt_cf

namespace mt_ns {
extern const mt_snap::StateOps k_snap_array, k_snap_set, k_snap_dict, k_snap_nsurl, k_snap_error;
extern const mt_snap::Module k_snap_ns, k_snap_ns_files;
} // namespace mt_ns

namespace mt_snap {

namespace {

// The value host states (#202). #203 adds the graphics, audio and UIKit ones.
const StateOps *const k_ops[] = {
    &mt_cf::k_snap_string, &mt_cf::k_snap_number, &mt_cf::k_snap_data,   &mt_cf::k_snap_url,
    &mt_cf::k_snap_bundle, &mt_cf::k_snap_timer,  &mt_cf::k_snap_runloop, &mt_ns::k_snap_array,
    &mt_ns::k_snap_set,    &mt_ns::k_snap_dict,   &mt_ns::k_snap_nsurl,  &mt_ns::k_snap_error,
};

const Module *const k_modules[] = {
    &mt_cf::k_snap_cf, &mt_cf::k_snap_cf_bundle, &mt_cf::k_snap_cf_runloop,
    &mt_ns::k_snap_ns, &mt_ns::k_snap_ns_files,
};

} // namespace

const StateOps *ops_for(const mt_objc_state_type *type) {
    for (const StateOps *o : k_ops) {
        if (o->type == type) {
            return o;
        }
    }
    return nullptr;
}

const StateOps *ops_named(const char *name) {
    for (const StateOps *o : k_ops) {
        if (std::strcmp(o->name, name) == 0) {
            return o;
        }
    }
    return nullptr;
}

const StateOps *const *all_ops(std::size_t *count) {
    *count = sizeof k_ops / sizeof k_ops[0];
    return k_ops;
}

const Module *const *all_modules(std::size_t *count) {
    *count = sizeof k_modules / sizeof k_modules[0];
    return k_modules;
}

} // namespace mt_snap
