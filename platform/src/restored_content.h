// Restored content (#389, ADR 0069's #379 addendum, docs/re/original-bugs.md "Unreachable
// content"): what the game ships but never reaches, made reachable under the setting
// restored_content (off in every preset). Each restore is its own follow-up: shops 0, 1 and 16
// from three merchants (#426, maplethief/hooks.h's mt_hooks_install_restored_shops).
#ifndef MAPLETHIEF_PLATFORM_RESTORED_CONTENT_H
#define MAPLETHIEF_PLATFORM_RESTORED_CONTENT_H

namespace maplethief::frontend {

// Registers the restored shops' hooks, on or off, with the box's texts in the menu language
// (port_text.def, restored_shop_*). Before the guest runs: game.cpp and the tests' boot.
void install_restored_shops(bool on);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_RESTORED_CONTENT_H
