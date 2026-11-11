// The Nintendo Switch's screen (#238, ADR 0138): the window is the whole screen, 1280 x 720 in
// handheld mode and 1920 x 1080 docked, and follows a dock or undock while the game runs. Pure
// arithmetic, no SDL or libnx, so that the desktop's tests check it: platform/switch/
// switch_app.cpp asks libnx which mode the console is in, SDL's Switch backend resizes the window
// (with the fix of cmake/patches/sdl3-nx-window-size.cmake), and the presenter places the
// 480 x 320 picture in it as on every target (scaling.h, ADR 0043; border.h, ADR 0085).
//
// What the scale modes give there (scaling_test.cpp's Switch case):
//   handheld  integer 2x, 960 x 640 at (160, 40); fit, smooth and sharp 1080 x 720 at (100, 0)
//   docked    integer 3x, 1440 x 960 at (240, 60); fit, smooth and sharp 1620 x 1080 at (150, 0)
// The Switch's default is sharp (settings.h's kPlatformDefaults): integer leaves a quarter of
// the handheld screen's height unused, fit's nearest scaling at 2.25 makes uneven texels.
#ifndef MAPLETHIEF_PLATFORM_SWITCH_SCREEN_H
#define MAPLETHIEF_PLATFORM_SWITCH_SCREEN_H

namespace maplethief::frontend {

struct SwitchScreen {
    int width = 0;
    int height = 0;
    constexpr bool operator==(const SwitchScreen &) const = default;
};

inline constexpr SwitchScreen kSwitchHandheld{1280, 720};
inline constexpr SwitchScreen kSwitchDocked{1920, 1080};

// The screen in the console's operation mode.
constexpr SwitchScreen switch_screen(bool docked) {
    return docked ? kSwitchDocked : kSwitchHandheld;
}

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_SWITCH_SCREEN_H
