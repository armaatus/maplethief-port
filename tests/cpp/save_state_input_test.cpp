// The frontend's side of a save state's restore (#203, save_state_input.h): a held key is
// released, a scene skip's tap is let go and the skip starts over, the menu's repeat is
// forgotten, a null layer is skipped. The runtime's side (its input, part of the state) is
// objc.snapshot's and play.save_state's.
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "actions.h"
#include "input.h"
#include "keyboard.h"
#include "maplethief/input.h"
#include "menu_input.h"
#include "save_state_input.h"
#include "scene_skip.h"

namespace {

namespace fe = maplethief::frontend;
using u32 = std::uint32_t;

std::vector<std::pair<u32, bool>> g_sent;
void record(void *, u32 action, bool down) { g_sent.emplace_back(action, down); }
std::int32_t field_panel(void *) { return 0; }

SDL_Event key_event(SDL_Keycode key, bool down) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.key = key;
    e.key.down = down;
    return e;
}

} // namespace

TEST_CASE("save_state_input: a held key is released before the restore") {
    g_sent.clear();
    fe::Input input;
    fe::Actions actions{&input};
    fe::Keyboard keyboard{&actions};
    actions.set_sink(record, nullptr);
    actions.set_panel_source(field_panel, nullptr);
    const SDL_Event down = key_event(SDLK_LEFT, true);
    input.handle(down);
    keyboard.handle(down);
    REQUIRE(g_sent.size() == 1);
    CHECK(g_sent[0] == std::pair<u32, bool>{MT_ACTION_LEFT, true});
    fe::RestoreInput layers;
    layers.input = &input;
    layers.actions = &actions;
    fe::release_for_restore(layers, 1.0);
    REQUIRE(g_sent.size() == 2);
    CHECK(g_sent[1] == std::pair<u32, bool>{MT_ACTION_LEFT, false});
    // The key's own release later goes nowhere: it was let go already.
    const SDL_Event up = key_event(SDLK_LEFT, false);
    input.handle(up);
    keyboard.handle(up);
    CHECK(g_sent.size() == 2);
}

TEST_CASE("save_state_input: a scene skip's tap is let go and the skip starts over") {
    std::vector<std::pair<u32, bool>> presses;
    fe::SceneSkip skip([&presses](u32 action, bool down) { presses.emplace_back(action, down); });
    const auto read = [](u32 addr, u32 *out) {
        const std::map<u32, u32> words = {
            {fe::kAutosaveCanvas, 0x01000000},         {0x01000000 + 0x0C, 0x01001000},
            {0x01001000, fe::kAutosaveGameVptr},       {fe::kAutosaveGameMode, 9},
            {fe::kAutosavePopup, 0xFFFFFFFFu},         {0x01001000 + fe::kSceneBoxWord, 0x101u},
            {0x01001000 + fe::kSceneList, 0x01002000}, {0x01002000 + fe::kSceneListStep, 12},
        };
        const auto it = words.find(addr);
        if (it == words.end()) {
            return false;
        }
        *out = it->second;
        return true;
    };
    skip.turn(read, true, fe::kScenePanel, 100, true, 1.0);
    REQUIRE(presses.size() == 1);
    CHECK(skip.skipping());
    fe::RestoreInput layers;
    layers.scene_skip = &skip;
    fe::release_for_restore(layers, 1.1);
    REQUIRE(presses.size() == 2);
    CHECK(presses[1] == std::pair<u32, bool>{MT_ACTION_OK, false});
    CHECK_FALSE(skip.skipping());
    CHECK(skip.rate() == 1);
    // No second release later: the tap is forgotten.
    fe::release_for_restore(layers, 1.2);
    CHECK(presses.size() == 2);
}

TEST_CASE("save_state_input: no layer is a no-op; the menu's state is forgotten") {
    fe::release_for_restore(fe::RestoreInput{}, 0.0);
    fe::MenuInput menu;
    fe::RestoreInput layers;
    layers.menu = &menu;
    fe::release_for_restore(layers, 0.0);
    CHECK(menu.popup() == -1);
    CHECK_FALSE(menu.guarding(0.0));
}
