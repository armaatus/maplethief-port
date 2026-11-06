// The registry of host state serializers and module sections (#202, ADR 0105). Each entry lives
// next to its state type, in the type's own file; this list only names them. A new kind of host
// state adds its line here (snapshot_internal.h has the recipe); the census
// (mt_snapshot_missing, play.save_state_census) names every live type that has none.
#include <cstring>

#include "snapshot/snapshot_internal.h"

namespace mt_cf {
extern const mt_snap::StateOps k_snap_string, k_snap_number, k_snap_data, k_snap_url, k_snap_bundle,
    k_snap_timer, k_snap_runloop;
extern const mt_snap::Module k_snap_cf, k_snap_cf_bundle, k_snap_cf_runloop;
} // namespace mt_cf

namespace mt_ns {
extern const mt_snap::StateOps k_snap_array, k_snap_set, k_snap_dict, k_snap_nsurl, k_snap_error;
extern const mt_snap::Module k_snap_ns, k_snap_ns_files;
} // namespace mt_ns

namespace mt_cg {
extern const mt_snap::StateOps k_snap_space, k_snap_color, k_snap_provider, k_snap_image,
    k_snap_gradient, k_snap_path, k_snap_uiimage, k_snap_uicolor, k_snap_bezier, k_snap_font;
extern const mt_snap::Module k_snap_cg_uikit;
} // namespace mt_cg

namespace mt_ui {
extern const mt_snap::StateOps k_snap_view, k_snap_layer, k_snap_controller;
extern const mt_snap::Module k_snap_uikit, k_snap_uikit_touch;
} // namespace mt_ui

namespace mt_gles {
extern const mt_snap::StateOps k_snap_eagl;
extern const mt_snap::Module k_snap_gfx, k_snap_gles;
} // namespace mt_gles

namespace mt_audio {
extern const mt_snap::StateOps k_snap_session, k_snap_player;
extern const mt_snap::Module k_snap_audio;
} // namespace mt_audio

namespace mt_stubs_impl {
extern const mt_snap::StateOps k_snap_gk_score, k_snap_gk_achievement, k_snap_gk_leaderboard,
    k_snap_gk_controller, k_snap_sk_request, k_snap_sk_response, k_snap_sk_product,
    k_snap_sk_payment, k_snap_sk_transaction, k_snap_socket, k_snap_socket_source;
extern const mt_snap::Module k_snap_gamekit, k_snap_storekit, k_snap_stubs;
} // namespace mt_stubs_impl

namespace mt_input_snap {
extern const mt_snap::Module k_snap_input;
} // namespace mt_input_snap

namespace mt_cash_snap {
extern const mt_snap::Module k_snap_cashshop;
} // namespace mt_cash_snap

extern const mt_snap::Module k_snap_libc_stdio, k_snap_libc_ntoa;

namespace mt_snap {

namespace {

// The value host states (#202), then the graphics, audio, UIKit and service ones (#203).
const StateOps *const k_ops[] = {
    &mt_cf::k_snap_string,
    &mt_cf::k_snap_number,
    &mt_cf::k_snap_data,
    &mt_cf::k_snap_url,
    &mt_cf::k_snap_bundle,
    &mt_cf::k_snap_timer,
    &mt_cf::k_snap_runloop,
    &mt_ns::k_snap_array,
    &mt_ns::k_snap_set,
    &mt_ns::k_snap_dict,
    &mt_ns::k_snap_nsurl,
    &mt_ns::k_snap_error,
    &mt_cg::k_snap_space,
    &mt_cg::k_snap_color,
    &mt_cg::k_snap_provider,
    &mt_cg::k_snap_image,
    &mt_cg::k_snap_gradient,
    &mt_cg::k_snap_path,
    &mt_cg::k_snap_uiimage,
    &mt_cg::k_snap_uicolor,
    &mt_cg::k_snap_bezier,
    &mt_cg::k_snap_font,
    &mt_ui::k_snap_view,
    &mt_ui::k_snap_layer,
    &mt_ui::k_snap_controller,
    &mt_gles::k_snap_eagl,
    &mt_audio::k_snap_session,
    &mt_audio::k_snap_player,
    &mt_stubs_impl::k_snap_gk_score,
    &mt_stubs_impl::k_snap_gk_achievement,
    &mt_stubs_impl::k_snap_gk_leaderboard,
    &mt_stubs_impl::k_snap_gk_controller,
    &mt_stubs_impl::k_snap_sk_request,
    &mt_stubs_impl::k_snap_sk_response,
    &mt_stubs_impl::k_snap_sk_product,
    &mt_stubs_impl::k_snap_sk_payment,
    &mt_stubs_impl::k_snap_sk_transaction,
    &mt_stubs_impl::k_snap_socket,
    &mt_stubs_impl::k_snap_socket_source,
};

// Applied in this order after guest memory and the states: the textures before the GL state
// that names them, UIKit before its touch tracking, the audio last but one (it re-opens the
// players the states name).
const Module *const k_modules[] = {
    &mt_cf::k_snap_cf,
    &mt_cf::k_snap_cf_bundle,
    &mt_cf::k_snap_cf_runloop,
    &mt_ns::k_snap_ns,
    &mt_ns::k_snap_ns_files,
    &k_snap_libc_stdio,
    &k_snap_libc_ntoa,
    &mt_cg::k_snap_cg_uikit,
    &mt_gles::k_snap_gfx,
    &mt_gles::k_snap_gles,
    &mt_ui::k_snap_uikit,
    &mt_ui::k_snap_uikit_touch,
    &mt_stubs_impl::k_snap_stubs,
    &mt_stubs_impl::k_snap_gamekit,
    &mt_stubs_impl::k_snap_storekit,
    &mt_cash_snap::k_snap_cashshop,
    &mt_audio::k_snap_audio,
    &mt_input_snap::k_snap_input,
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
