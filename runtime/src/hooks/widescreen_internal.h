// Widescreen (#86, #169, ADR 0090): what widescreen.cpp and widescreen_menus.cpp share. Guest
// thread only.
#ifndef MAPLETHIEF_HOOKS_WIDESCREEN_INTERNAL_H
#define MAPLETHIEF_HOOKS_WIDESCREEN_INTERNAL_H

namespace mt_widescreen_detail {

// The menus' hooks (screen paints, the fill), on or off; by mt_widescreen_configure.
void menus_configure(bool on) noexcept;
// Unregisters them and zeroes their numbers (mt_widescreen_reset).
void menus_reset() noexcept;
// Whether a screen's paint is running with the engine's width back at 480: the camera and
// backdrop hooks then leave the width and the host transform alone.
bool menu_active() noexcept;

} // namespace mt_widescreen_detail

#endif
