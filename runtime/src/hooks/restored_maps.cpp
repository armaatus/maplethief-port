// The unreached maps 4.5 and 4.8 (#428, restored_content; docs/re/original-bugs.md "Unreachable
// content"); see maplethief/hooks.h.
//
// The game ships a third road from 4.3 to 4.10 beside 4.4-4.7 and 4.6-4.9: 4.5 and 4.8, whose
// portal scripts already lead out to maps the game reaches (portal451 to 4.3's portal 2, portal481
// to 4.10's portal 0) and to each other (portal450, portal480), but no reached map has a portal
// into them. A wrapper on CMMap::LoadObj 0xD259 runs the loader, then puts one portal object
// on 4.3, named portal480 (the shipped script that warps to 4.5's portal 0), at the upper middle
// platform whose portal statue has no portal.
//
// It goes first in the list, with kind 2 (a drawn portal; 0-2 are 4.3's own). The warp's
// arrival (0x868D5) puts the hero on the last object whose kind is the target portal, so the
// warps to 4.3's portal 2 (portal460 from 4.6, portal451 from 4.5) still land on 4.3's own. 4.3
// has no NPC and no map script, so nothing refers to its objects by index.
#include <atomic>
#include <cstdint>

#include "map_objects.h"
#include "maplethief/diag.h"
#include "maplethief/hle/abi.h"
#include "maplethief/hooks.h"

namespace {

using u32 = std::uint32_t;

// 4.3 is 38 tiles wide; tile (18, 7) stands on the platform's floor (43.att row 16).
constexpr mt_restored_portal k_portals[] = {
    {4, 3, 7 * 38 + 18, 2, "portal480", 4, 5, 0},
};

std::atomic<std::uint64_t> g_portals{0};

int load_obj(mt_cpu *cpu, const mt_hook_call *hook) {
    const u32 map = cpu->r[0];
    const auto area = static_cast<std::int32_t>(cpu->r[1]);
    const auto number = static_cast<std::int32_t>(cpu->r[2]);
    hook->original(cpu);
    const mt_restored_portal *p = mt_restored_portal_on(area, number);
    if (p == nullptr || mt_map_objects_detail::count(cpu, map) == 0u) {
        return MT_HOOK_HANDLED; // void: nothing to return; each load builds the list afresh
    }
    if (mt_map_objects_detail::insert(cpu, map, 0u, static_cast<std::uint16_t>(p->cell),
                                      static_cast<std::uint16_t>(p->kind), p->script,
                                      MT_HOOK_FN_MAP_LOAD_OBJ)) {
        g_portals.fetch_add(1, std::memory_order_relaxed);
        mt_tracef("hooks", "CMMap::LoadObj(%d, %d): %s (portal %d) at cell %d, to %d.%d", area,
                  number, p->script, p->kind, p->cell, p->to_area, p->to_map);
    }
    return MT_HOOK_HANDLED;
}

} // namespace

extern "C" const mt_restored_portal *mt_restored_portal_on(std::int32_t area,
                                                           std::int32_t map) noexcept {
    for (const mt_restored_portal &p : k_portals) {
        if (p.area == area && p.map == map) {
            return &p;
        }
    }
    return nullptr;
}

extern "C" void mt_hooks_install_restored_maps(int enabled) noexcept {
    mt_hook_wrap(MT_HOOK_FN_MAP_LOAD_OBJ, load_obj, nullptr, enabled);
}

extern "C" mt_restored_maps_stats mt_hooks_restored_maps_stats(void) noexcept {
    return {g_portals.load(std::memory_order_relaxed)};
}
