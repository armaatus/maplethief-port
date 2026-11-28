// Restored content (#389, ADR 0069's #379 addendum, docs/re/original-bugs.md "Unreachable
// content"): what the game ships but never reaches, made reachable under the setting
// restored_content (off in every preset). Each restore is its own follow-up: shops 0, 1 and 16
// from three merchants (#426, maplethief/hooks.h's mt_hooks_install_restored_shops); Monster
// Carnival from a Spiegelman NPC in three towns (#427, mt_hooks_install_restored_carnival); the
// unreached maps 4.5 and 4.8 from a portal on 4.3 (#428, mt_hooks_install_restored_maps).
#ifndef MAPLETHIEF_PLATFORM_RESTORED_CONTENT_H
#define MAPLETHIEF_PLATFORM_RESTORED_CONTENT_H

namespace maplethief::frontend {

// Registers the restored shops' hooks, on or off, with the box's texts in the menu language
// (port_text.def, restored_shop_*). Before the guest runs: game.cpp and the tests' boot.
void install_restored_shops(bool on);

// Registers Monster Carnival's hook, on or off, and with it on shows the Spiegelman scripts'
// literal texts in the game's language (mt_cf_set_literal_key_prefixes). Before the guest runs:
// game.cpp and the tests' boot.
void install_restored_carnival(bool on);

// Registers the unreached maps' wrapper, on or off. Before the guest runs: game.cpp and the
// tests' boot.
void install_restored_maps(bool on);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_RESTORED_CONTENT_H
