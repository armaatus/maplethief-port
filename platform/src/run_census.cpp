// The census of the port's ways into the guest (#405); see run_census.h.
#include "run_census.h"

namespace maplethief::frontend {

namespace {

constexpr CensusKind R = CensusKind::kRecorded;
constexpr CensusKind P = CensusKind::kPure;
constexpr CensusKind X = CensusKind::kRefused;

constexpr CensusSite kSites[] = {
    // Input: every action, touch and lifecycle message, whoever sends it (the devices, the
    // focus engine, menu repeat, the Assists' latches, skip_intros' taps, the scripted taps).
    {"actions.cpp", "mt_input_press", R, nullptr, "the bound actions"},
    {"actions.cpp", "mt_input_release", R, nullptr, "the bound actions"},
    {"assists.cpp", "mt_input_press", R, nullptr, "turbo_attack, auto_potion, auto_run"},
    {"assists.cpp", "mt_input_release", R, nullptr, "turbo_attack, auto_potion, auto_run"},
    {"focus.cpp", "mt_input_press", R, nullptr, "the controller focus on UIKit screens"},
    {"focus.cpp", "mt_input_release", R, nullptr, "the controller focus on UIKit screens"},
    {"game.cpp", "mt_input_press", R, nullptr, "the gamepads' actions"},
    {"game.cpp", "mt_input_release", R, nullptr, "the gamepads' actions"},
    {"menu_input.cpp", "mt_input_press", R, nullptr, "menu repeat and the confirm guard"},
    {"menu_input.cpp", "mt_input_release", R, nullptr, "menu repeat and the confirm guard"},
    {"scene_skip.cpp", "mt_input_press", R, nullptr, "skip_intros' taps"},
    {"scene_skip.cpp", "mt_input_release", R, nullptr, "skip_intros' taps"},
    {"input.cpp", "mt_uikit_touch", R, nullptr, "every touch (mouse, fingers, the pointer)"},
    {"lifecycle.cpp", "mt_uikit_lifecycle_event", R, nullptr, "resign, become active, terminate"},
    {"game.cpp", "mt_runloop_set_fire_tolerance", R, nullptr,
     "the 1 ms grid (ADR 0131) changes when the run loop's timer fires"},

    // Settings that reach the guest: the file holds them, with every change at its turn.
    {"game.cpp", "mt_hooks_install_original_bug_fixes", P, nullptr, "fix_original_bugs"},
    {"game.cpp", "mt_hooks_install_quest_page_fix", P, nullptr, "always on"},
    {"game.cpp", "mt_hooks_install_text_wrap_guard", P, nullptr, "always on"},
    {"restored_content.cpp", "mt_hooks_install_restored_shops", P, nullptr, "restored_content"},
    {"game.cpp", "mt_hooks_install_screen_shake", P, nullptr, "screen_shake"},
    {"game.cpp", "mt_hooks_install_flashes", P, nullptr, "flashes"},
    {"game.cpp", "mt_hooks_install_native_script_vm", P, nullptr,
     "--native-script-vm: the same opcode loop on the host (ADR 0226), a developer switch"},
    {"live_settings.cpp", "mt_hooks_install_screen_shake", P, nullptr, "screen_shake, live"},
    {"live_settings.cpp", "mt_hooks_install_flashes", P, nullptr, "flashes, live"},
    {"game.cpp", "mt_uikit_set_animation_scale", P, nullptr, "ui_animations"},
    {"game.cpp", "mt_widescreen_turn", P, nullptr, "widescreen: the view's width every turn"},
    {"game_speed.cpp", "guest_memory", P, nullptr,
     "game_speed: the speed table and CMTimer's interval, at the boot and at its change"},
    {"assists.cpp", "mt_hook_register", P, nullptr, "auto_loot widens a drop's reach"},
    {"assists.cpp", "mt_hook_unregister", P, nullptr, "auto_loot"},
    {"assists.cpp", "guest_memory", P, nullptr, "auto_loot's argument, inside the guest's call"},
    {"list_keys.cpp", "mt_hook_register", P, nullptr, "list keys in shops and the popup menu"},
    {"list_keys.cpp", "mt_hook_unregister", P, nullptr, "list keys"},
    {"list_keys.cpp", "mt_call_guest", P, nullptr, "list keys, inside the guest's key handler"},
    {"list_keys.cpp", "guest_memory", P, nullptr, "list keys, inside the guest's key handler"},
    {"prompts.cpp", "mt_hook_wrap", P, nullptr, "the popup menu's list scroll"},
    {"prompts.cpp", "mt_hook_unwrap", P, nullptr, "the popup menu's list scroll"},
    {"prompts.cpp", "guest_memory", P, nullptr, "the list scroll, inside the guest's handler"},
    {"keypad_layout.cpp", "mt_hook_register", P, nullptr, "keypad_layout: the keys' hit areas"},
    {"keypad_layout.cpp", "mt_hook_unregister", P, nullptr, "keypad_layout"},
    {"keypad_layout.cpp", "mt_hook_set_enabled", P, nullptr, "keypad_layout"},
    {"keypad_layout.cpp", "mt_uikit_set_host_transform", P, nullptr,
     "keypad_layout and widescreen: where the pad's views take touches"},
    {"keypad_layout.cpp", "mt_uikit_set_host_opacity", P, nullptr, "keypad_layout (drawing only)"},
    {"keypad_layout.cpp", "mt_uikit_set_host_reveal", P, nullptr, "keypad_layout"},
    {"keypad_layout.cpp", "mt_uikit_set_host_touch_area", P, nullptr,
     "widescreen: touches that begin in the wings"},
    {"minimap.cpp", "mt_uikit_set_host_transform", P, nullptr,
     "minimap: the compass beside the box, from the panel and the setting"},
    {"keypad_hide.cpp", "mt_uikit_set_host_opacity", P, nullptr,
     "hide_keypad's fade: drawing only, a touch's hit test does not read it"},
    {"keypad_haptics.cpp", "mt_hook_wrap", P, nullptr, "keypad_haptics: observes, passes through"},
    {"keypad_haptics.cpp", "mt_hook_unwrap", P, nullptr, "keypad_haptics"},
    {"recovery.cpp", "mt_hook_wrap", P, nullptr, "crash_recovery counts the game's saves"},
    {"save_tools.cpp", "mt_hook_register", P, nullptr,
     "backups: the game's save goes through, a copy beside it"},
    {"autosave.cpp", "mt_hook_register", P, nullptr, "autosave notices the exit to the menu"},
    {"autosave.cpp", "mt_hook_unregister", P, nullptr, "autosave"},
    {"quick_state.cpp", "mt_saves_replace_file", P, nullptr,
     "a save state's own file, which the guest never reads"},
    {"pack_tools.cpp", "mt_saves_replace_file", P, nullptr,
     "the text export's Port.strings, in a pack folder the guest never reads"},

    // What a replay cannot do again: the recording ends with the turn before.
    {"autosave.cpp", "mt_call_guest", X, "autosave",
     "the game's save, called by the port when the window closes or hides"},
    {"quick_state.cpp", "mt_snapshot_restore", X, "a save state's load",
     "the whole guest replaced by a state file"},
    {"save_tools.cpp", "mt_saves_replace_file", X, "a save import or backup restore",
     "the game's own files replaced while it runs"},
    {"data_location.cpp", "mt_saves_replace_file", X, "a data folder switch",
     "the saves moved to another folder"},
};

} // namespace

std::span<const CensusSite> run_census() { return kSites; }

const char *census_kind_name(CensusKind kind) {
    switch (kind) {
    case CensusKind::kRecorded:
        return "recorded";
    case CensusKind::kPure:
        return "pure";
    case CensusKind::kRefused:
        return "refused";
    }
    return "?";
}

} // namespace maplethief::frontend
