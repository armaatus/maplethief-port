// The platform capability table (#241, ADR 0142): what each platform the frontend builds for
// has, as a pure description fixed at compile time. Rows of the settings registry
// (SettingInfo::needs, settings.h) and the settings menu's action rows (MenuActionInfo::needs,
// settings_menu.h) name the capabilities they need, and a platform without one of them does not
// show the row, so that no menu entry does nothing there. The frontend's other platform-only
// behaviour (the desktop's hotkeys, the controller's import list) asks the same table.
//
// The desktop (macOS, Windows, Linux) has every capability but the iOS-only ones, so on the
// desktop nothing is hidden. The Nintendo Switch (ADR 0138) has no window of its own (the window
// is the screen), one display, no cursor, no file dialog or file manager, no keyboard of its
// own, no other place for the data folder, one audio output, no HTTP fetcher; instead its pack
// rows offer the folders on the SD card (pack_folders.h), and the IPA is found on the SD card.
//
// Pure: no SDL, no I/O. Add a capability: one bit, its line in every row of kPlatformCaps, the
// rows that need it, and its line in ADR 0142's table.
#ifndef MAPLETHIEF_PLATFORM_PLATFORM_CAPS_H
#define MAPLETHIEF_PLATFORM_PLATFORM_CAPS_H

#include <cstdint>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace maplethief::frontend {

// The platform of a build (#238, ADR 0070's addendum): fixed at compile time, never decided at
// run time. kDesktop is macOS, Windows and Linux (the Steam Deck too).
enum class SettingsPlatform { kDesktop, kIos, kSwitch };
#if defined(__SWITCH__)
inline constexpr SettingsPlatform kSettingsPlatform = SettingsPlatform::kSwitch;
#elif defined(__APPLE__) && TARGET_OS_IPHONE
inline constexpr SettingsPlatform kSettingsPlatform = SettingsPlatform::kIos;
#else
inline constexpr SettingsPlatform kSettingsPlatform = SettingsPlatform::kDesktop;
#endif

// One capability, a bit of a mask. What depends on each:
enum Capability : std::uint32_t {
    // A window the player sizes, moves and puts fullscreen: the fullscreen and fullscreen_mode
    // rows, the fullscreen hotkeys (F11, Alt+Enter) and the window size keys (Ctrl+1 ... 5).
    kCapWindow = 1u << 0,
    // A choice of display to go fullscreen on: fullscreen_display (#196).
    kCapDisplays = 1u << 1,
    // A mouse with a cursor: right_touch (the right button) and the cursor's auto-hide.
    kCapCursor = 1u << 2,
    // A keyboard as the platform's own input: the screenshot key (F12) and, with kCapWindow,
    // the window's hotkeys. A USB keyboard on the Switch still plays the game (bindings.ini).
    kCapKeyboard = 1u << 3,
    // The system's open and save dialogs (#192): Export and Import ask through them.
    kCapFileDialogs = 1u << 4,
    // A file manager that shows a folder: Open data folder, Open logs folder (#192, #198).
    kCapOpenFolder = 1u << 5,
    // Values typed into settings.ini on the same machine: the rows that offer no values in the
    // menu and only show what the file says, save_dir (#148: any folder, a synced one) and
    // keypad_layout (#159: the editor row writes it). The Switch has no text editor for them.
    kCapTypedValues = 1u << 6,
    // The data folder has a second place to move to: the Data location row (#263, ADR 0127).
    kCapDataLocationChoice = 1u << 7,
    // A choice among audio outputs: audio_device (#195).
    kCapAudioDevices = 1u << 8,
    // The app quits itself (iOS apps do not): the menu's Quit row (#179).
    kCapQuit = 1u << 9,
    // An HTTP fetcher for the update notice (update_fetch.cpp): update_check and its row.
    kCapUpdateFetch = 1u << 10,
    // The iOS Files app shows the data folder (ADR 0074): an import takes the newest zip there.
    kCapFilesApp = 1u << 11,
    // The pack and border rows offer the folders on the SD card (pack_folders.h): the menu
    // has no other way to name them with a controller.
    kCapPackFolders = 1u << 12,
    // The player's IPA is a file on the SD card beside the game (#243): no dialog, no drop. The
    // import screen looks in <base> for MapleThief-1.5.ipa and then any other *.ipa, imports by
    // itself, and offers to delete the file afterwards (import_screen.h's SD-card route).
    kCapIpaOnSdCard = 1u << 13,
};

struct PlatformCaps {
    SettingsPlatform platform;
    const char *name;
    std::uint32_t caps;
};

inline constexpr std::uint32_t kDesktopCaps =
    kCapWindow | kCapDisplays | kCapCursor | kCapKeyboard | kCapFileDialogs | kCapOpenFolder |
    kCapTypedValues | kCapDataLocationChoice | kCapAudioDevices | kCapQuit | kCapUpdateFetch;

// The table. The iOS row says what the iOS app (ADR 0136) has: the window is always the whole
// screen (ADR 0055), an iPad's pointer clicks, a keyboard can be attached; the saves go through
// the Files app; the data folder is the app's.
inline constexpr PlatformCaps kPlatformCaps[] = {
    {SettingsPlatform::kDesktop, "desktop", kDesktopCaps},
    {SettingsPlatform::kIos, "ios",
     kCapCursor | kCapKeyboard | kCapTypedValues | kCapAudioDevices | kCapUpdateFetch |
         kCapFilesApp},
    // The Homebrew Menu takes the player back after a quit.
    {SettingsPlatform::kSwitch, "switch", kCapQuit | kCapPackFolders | kCapIpaOnSdCard},
};

// The capabilities of `platform`.
constexpr std::uint32_t platform_caps(SettingsPlatform platform = kSettingsPlatform) {
    for (const PlatformCaps &row : kPlatformCaps) {
        if (row.platform == platform) {
            return row.caps;
        }
    }
    return 0;
}

// Whether `platform` has every capability of `needs` (0: always).
constexpr bool platform_has(std::uint32_t needs, SettingsPlatform platform = kSettingsPlatform) {
    return (platform_caps(platform) & needs) == needs;
}

// "desktop", "ios", "switch".
constexpr const char *platform_name(SettingsPlatform platform) {
    for (const PlatformCaps &row : kPlatformCaps) {
        if (row.platform == platform) {
            return row.name;
        }
    }
    return "?";
}

static_assert(platform_has(kDesktopCaps, SettingsPlatform::kDesktop));
static_assert(!platform_has(kCapWindow, SettingsPlatform::kSwitch));

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_PLATFORM_CAPS_H
