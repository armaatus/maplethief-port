// The frontend's settings file (#59, ADR 0043; the audio keys #64; hide_keypad #71; rumble and
// rumble_strength #72; gfx_backend #120; filter and hd_text #88, ADR 0066; gapless_music #125;
// perf_overlay #137, ADR 0071; pacing #80, ADR 0075; fullscreen, vsync, pause_unfocused and
// language #145, ADR 0077; texture_pack and texture_filter #153, ADR 0081; border and
// border_image #157, ADR 0085; autosave #150, docs/re/autosave.md; frame_rate #87,
// ADR 0091; ui_animations, turbo_attack and auto_potion #90, ADR 0092; minimap #176, ADR 0098)
// and the settings registry
// and presets (#144, ADR 0070): settings.ini in the data directory (MAPLETHIEF_DATA_DIR, else
// SDL_GetPrefPath("", "MapleThief")). Pure parsing and writing, no SDL; game.cpp reads and
// writes the file, main.cpp applies the command line over it.
//
// Every setting is one row of the registry (settings_registry(), the table in settings.cpp):
// its key, group, comment, value type bound to its Settings field, command-line flag, menu
// label and help, and its value in each preset. Parsing, writing, the command line, the presets
// and #89's settings menu all go through the rows. To add a setting: add its field to Settings
// and its row to the table (ADR 0070 has the recipe and the planned keys' preset columns).
//
// A new file, the defaults and a preset chosen (the order of the table):
//
//   # MapleThief settings (settings.ini). ...
//   # the preset: original | original_controller | enhanced | custom. ...
//   preset = original
//   # integer | fit | smooth | sharp
//   scale_mode = integer
//   # the bars around the image, #RRGGBB
//   border_color = #000000
//   # the renderbuffers' internal scale, 1 to 8, or auto (the largest that fits the frame ...)
//   render_scale = 1
//   # what draws the game's GL frames: software (the reference) | gpu (SDL3 GPU)
//   gfx_backend = software
//   # a shader filter over the scaled image: none | xbr | crt | lcd
//   filter = none
//   # draw the game's text textures at the render scale: on | off
//   hd_text = off
//   # volume of everything, 0 to 100 (percent)
//   master_volume = 100
//   # volume of the music, 0 to 100
//   music_volume = 100
//   # volume of the sound effects, 0 to 100
//   effects_volume = 100
//   # silence while the window is not focused: on | off
//   mute_unfocused = on
//   # loop the music without the silence at the start and end of its files: on | off (off: ...)
//   gapless_music = off
//   # both channels the same mix, for one ear or one speaker: on | off
//   audio_mono = off
//   # the sound's output device, by its name, or default (the system's; from the menu's list)
//   audio_device = default
//   # a directory of your own music and sound files that replace the game's, by name (...)
//   audio_pack =
//   # the right mouse button: a touch at screen point X,Y, or off
//   right_touch = off
//   # hide the on-screen key pad: auto (while a gamepad or the keyboard is used) | always | never
//   hide_keypad = never
//   # the gamepad rumbles on a vibration, a hit and a level-up: on | off
//   rumble = on
//   # how strongly, 0 to 100 (percent)
//   rumble_strength = 100
//   # the frame-time overlay (F3 shows or hides it while the game runs): on | off
//   perf_overlay = off
//   # Assist: the cash shop and the paid revives: off (as the original offline) | meso (sold
//   # for the hero's meso, dearer with the level, the falls and the purchases)
//   cash_shop = off
//   # frame pacing: display (each game tick lasts whole display refreshes when the refresh
//   # rate allows) | wall (the wall clock, as before #80)
//   pacing = display
//   # the folder for the game's save files, e.g. a cloud-synced one (an absolute path; ...)
//   save_dir =
//   # backups kept of each character, taken after each save, 0 to 100 (0: none)
//   save_backups = 10
//   # the window: off | on (the whole display; F11 or Alt+Enter toggles it)
//   fullscreen = off
//   # how macOS shows fullscreen: desktop (its own Space, as the green button) | borderless ...
//   fullscreen_mode = desktop
//   # wait for the display's refresh to present a frame (no tearing): on | off
//   vsync = on
//   # pause the game while the window is not focused, as when it is minimised: on | off
//   pause_unfocused = off
//   # the game's language: en | ko (MAPLETHIEF_LANGUAGE overrides it; from the next start)
//   language = en
//   # a directory of HD textures made from your own --dump-textures (python3 -m ...); empty: none
//   texture_pack =
//   # how textures are sampled at render scale 2 and up: game (smooth, the device's look) | ...
//   texture_filter = game
//   # the touch key pad: original, or for each group moved (dpad, actions, slots)
//   # GROUP=SIZE,DX,DY,OPACITY (percent, screen points, percent), ';' between groups
//   keypad_layout = original
//   # what fills the window around the picture: color (border_color) | blur (the game, blurred) |
//   # image (border_image)
//   border = color
//   # the PNG of border = image, scaled to cover the window; empty: none
//   border_image =
//   # Assist: the game's tick length: game (the game's own Speed option) | 65 | 50 | 40 | 33
//   # | 25 (ms; 33 and 25 are faster than any the game offers)
//   game_speed = game
//   # Assist: holding the fast-forward key (Tab, the right stick's click) runs the game: off
//   # | x2 | x3 (times as fast)
//   fast_forward = off
//   # save the game when it quits or goes to the background, where the game's own Save ...
//   autosave = off
//   # how often the window shows the game: original (each game tick's frame, 20 a second) | ...
//   frame_rate = original
//   # how fast the menus' UIKit animations run: normal | fast | instant
//   ui_animations = normal
//   # Assist: holding attack in the field attacks again and again: on | off
//   turbo_attack = off
//   # Assist: drink the HP potion of a quick slot under this share of max HP: off | 25 | 50
//   # | 75 (percent)
//   auto_potion = off
//   # scenes (the intro, chapter cards, boss entrances) run fast, each speech box tapped: on
//   # | off
//   skip_intros = off
//   # the port's minimap of the map in the field (platforms, ladders, the hero, portals and NPCs):
//   # off | border (beside the picture where there is room, else over it) | overlay
//   minimap = off
//   # Assist: the auto-run key (R, the left stick's click) keeps the hero walking: on | off
//   auto_run = off
//   # Assist: a press starts these held actions and the next one stops them: off | walk
//   # | fast_forward | all
//   hold_toggle = off
//   # Assist: the hero picks up drops near it without walking over them: on | off
//   auto_loot = off
//   # pause when the gamepad in use disconnects, until a pad connects or a key: on | off
//   pause_on_disconnect = off
//
// The format, in full:
//   - UTF-8 (a leading byte order mark is skipped); lines end in LF or CR LF.
//   - Blank lines and lines whose first non-blank character is '#' are comments. There are no
//     comments after a value ('#' starts a colour), no sections, no quoting, no escapes.
//   - Every other line is `key = value`: blanks (space, tab) around both are dropped. Keys are
//     ASCII letters, digits and '_', matched case-insensitively, as are the mode names.
//   - Unknown keys, malformed lines, bad values and a `[section]` line are reported (the caller
//     logs them) and ignored; the setting keeps its earlier value. A key given twice: the last
//     one counts. Never fatal.
//   - A file over kMaxSettingsBytes is ignored as a whole.
//   - On/off values: on, off, true, false, yes, no, 1, 0 (any case); written as on or off.
//
// The presets (#144, ADR 0070):
//   - `preset = original | original_controller | enhanced | custom`. A preset other than
//     custom sets every key it controls (the rows' preset columns) and wins over those keys'
//     lines in the file, wherever the preset line is: a line that disagrees is reported and
//     ignored. `custom` keeps every key's own line. Keys outside the presets (the volumes,
//     right_touch, ...) are the file's in every preset.
//   - Changing a key the presets control (apply_setting: the command line, the settings menu)
//     moves a preset to custom when the new value differs from the preset's.
//   - No `preset` line (no file, or a file from before #144): no preset (Settings::preset
//     empty), every key as the file or the defaults say, exactly as before. The frontend then
//     shows the first-launch picker in an interactive run (preset_picker.h).
#ifndef MAPLETHIEF_PLATFORM_SETTINGS_H
#define MAPLETHIEF_PLATFORM_SETTINGS_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "keypad_layout.h"
#include "scaling.h"

namespace maplethief::frontend {

inline constexpr const char *kSettingsFileName = "settings.ini";
inline constexpr std::size_t kMaxSettingsBytes = 64 * 1024;

// right_touch (#65, ADR 0047): the right mouse button as a second touch at a fixed screen point.
struct RightTouch {
    bool on = false;
    std::uint32_t x = 0, y = 0; // screen points, x < 480, y < 320

    bool operator==(const RightTouch &) const = default;
};

// "off", or "X,Y" (whole screen points, x 0-479, y 0-319, blanks allowed around each) to the
// setting; false otherwise.
bool parse_right_touch(std::string_view text, RightTouch *out);

// hide_keypad (#71, ADR 0052): when the on-screen key pad is hidden (keypad_hide.h).
enum class KeypadMode { kAuto, kAlways, kNever };
// "auto", "always", "never".
const char *keypad_mode_name(KeypadMode mode);
// One of those names, in any case (no blanks).
bool parse_keypad_mode(std::string_view text, KeypadMode *out);

// gfx_backend (#120, ADR 0060): what executes mt_gfx's draws. kGpu installs the GPU backend
// (maplethief/gfx_gpu.h) and presents with SDL's GPU renderer on the same device; without a
// usable GPU the frontend logs it and stays on the software rasterizer.
enum class GfxBackend { kSoftware, kGpu };
// "software", "gpu".
const char *gfx_backend_name(GfxBackend backend);
// One of those names, in any case (no blanks).
bool parse_gfx_backend(std::string_view text, GfxBackend *out);

// filter (#88, ADR 0066): a fragment shader of the present stage (gfx_backend.h's PresentPass)
// that draws the screen image into the scale mode's rectangle. It needs SDL's GPU renderer for
// the window, which the frontend asks for whatever gfx_backend is; without one it logs that and
// draws with the scale mode's own filter.
//   none  the scale mode's filter (the default)
//   xbr   xBR-style edge smoothing (present_xbr.frag)
//   crt   scanlines and an aperture mask (present_crt.frag)
//   lcd   an LCD cell grid (present_lcd.frag)
enum class ShaderFilter { kNone, kXbr, kCrt, kLcd };
// "none", "xbr", "crt", "lcd".
const char *shader_filter_name(ShaderFilter filter);
// One of those names, in any case (no blanks).
bool parse_shader_filter(std::string_view text, ShaderFilter *out);

// cash_shop (#156, ADR 0072): the cash shop's items and the death popup's paid revives offline
// (maplethief/cashshop.h). An Assist: off in every preset.
//   off   as the original offline (ADR 0034): every purchase is cancelled
//   meso  the two SP items, the two EXP packs and the two revives are sold for the hero's meso
//         at prices that rise with the level and with the falls and purchases since the last
//         level-up; the meso packs are not sold
enum class CashShop { kOff, kMeso };
// texture_filter (#153, ADR 0081): how the game's textures are sampled at render scale N > 1.
//   game     the game's own filters (GL_LINEAR: the default, the device's look)
//   nearest  GL_NEAREST for every texture (mt_gfx_set_texture_filter): sharp texels, no bleed
//            from a texture's padding; at render scale 1 the game's filters stay
enum class TextureFilter { kGame, kNearest };
// "game", "nearest".
const char *texture_filter_name(TextureFilter filter);
// What mt_gfx_set_texture_filter gets at render scale N: MT_GFX_NEAREST for nearest at N > 1,
// else 0 (the game's filters). The frontend applies it at the start and at every change of N.
std::uint32_t texture_filter_override(TextureFilter filter, std::uint32_t render_scale);

// The names of each enum's values, in the enum's order (value 0 first): the registry's choice
// rows use them, and the *_name functions above return them.
inline constexpr const char *kScaleModeNames[] = {"integer", "fit", "smooth", "sharp"};
inline constexpr const char *kGfxBackendNames[] = {"software", "gpu"};
inline constexpr const char *kShaderFilterNames[] = {"none", "xbr", "crt", "lcd"};
inline constexpr const char *kKeypadModeNames[] = {"auto", "always", "never"};
// What the settings menu offers for border_color and right_touch (#89): black, greys, white; off,
// a touch on the attack key, on the jump key (docs/controls/keypad.md's key areas).
inline constexpr const char *kBorderColorChoices[] = {"#000000", "#202020", "#404040", "#808080",
                                                      "#FFFFFF"};
inline constexpr const char *kRightTouchChoices[] = {"off", "440,290", "370,290"};
inline constexpr const char *kCashShopNames[] = {"off", "meso"};

// fullscreen (#145, ADR 0077; #179, ADR 0099): `fullscreen = off | on`, the window or the whole
// display (SDL's desktop fullscreen: borderless at the display's own mode). F11 and Alt+Enter
// toggle it (desktop.h). `fullscreen_mode = desktop | borderless` is how macOS shows it: desktop
// in its own Space (the system's fullscreen, the green button's), borderless over the desktop
// without a Space or its animation; it takes effect at the next start (SDL reads it at video
// init). Elsewhere both are the same borderless window. iOS is always fullscreen (ADR 0055) and
// ignores both.
enum class FullscreenMode { kDesktop, kBorderless };
inline constexpr const char *kFullscreenModeNames[] = {"desktop", "borderless"};
// language (#145): the bundle's localisation the game plays in (mt_cf_config.language, #75);
// MAPLETHIEF_LANGUAGE overrides it.
enum class Language { kEn, kKo };
inline constexpr const char *kLanguageNames[] = {"en", "ko"};
inline constexpr const char *kTextureFilterNames[] = {"game", "nearest"};
// autosave (#150, autosave.h, docs/re/autosave.md): when the port saves the character in play
// through the game's own save pair, and only where the game's menu Save would be allowed.
//   off      never (the original: only the game's own saves)
//   on_quit  on a quit (the window's close button, Cmd-Q, --quit) and when the game goes to the
//            background (a minimise, iOS's home gesture, pause_unfocused's focus loss)
enum class Autosave { kOff, kOnQuit };
inline constexpr const char *kAutosaveNames[] = {"off", "on_quit"};
// widescreen (#86, ADR 0090, maplethief/widescreen.h): the field drawn wider than 3:2.
//   off   the original 480 x 320 picture everywhere
//   auto  a 568 x 320 GL layer: in the field (and the NPC chat) on a map wider than 480 points
//         the engine draws up to 568 points wide (as wide as the map), the touch key pad shown
//         or not (#168); menus, the title and narrow maps keep the 3:2 picture, centred
enum class Widescreen { kOff, kAuto };
inline constexpr const char *kWidescreenNames[] = {"off", "auto"};
// widescreen_hud (#168, ADR 0090's addendum): where the touch key pad and its GL icons go when
// the field is wider than 3:2. The bar is drawn in the middle either way.
//   edges   the key pad's groups move to the wide view's edges, their icons with them: the
//           d-pad to the left edge, jump / attack and the quick slots to the right
//   narrow  the whole HUD and the key pad stay at their 3:2 places (the middle 480 points)
enum class WidescreenHud { kEdges, kNarrow };
inline constexpr const char *kWidescreenHudNames[] = {"edges", "narrow"};
// frame_rate (#87, ADR 0091, frame_interp.h): how often the window shows the game's picture.
//   original  each game tick's frame as it was drawn, 20 a second at the default speed
//   smooth    every display refresh, the frame between the last two ticks interpolated (the
//             game's draws replayed at blended positions); costs most of a tick of latency
enum class FrameRate { kOriginal, kSmooth };
inline constexpr const char *kFrameRateNames[] = {"original", "smooth"};
// "original", "smooth".
const char *frame_rate_name(FrameRate rate);

// preset (#144, ADR 0070): the whole experience in one setting. The first three are the columns
// of the registry; custom is whatever the player changed.
enum class Preset { kOriginal, kOriginalController, kEnhanced, kCustom };
inline constexpr Preset kPresets[] = {Preset::kOriginal, Preset::kOriginalController,
                                      Preset::kEnhanced, Preset::kCustom};
inline constexpr const char *kPresetNames[] = {"original", "original_controller", "enhanced",
                                               "custom"};
// "original", "original_controller", "enhanced", "custom" (the file's and --preset's values).
const char *preset_name(Preset preset);
// One of those names, in any case (no blanks).
bool parse_preset(std::string_view text, Preset *out);
// "Original", "Original + controller", "Enhanced", "Custom" (the picker's and the menu's).
const char *preset_title(Preset preset);
// What the preset is, in one line of at most 64 characters (the picker).
const char *preset_summary(Preset preset);

// pacing (#80, ADR 0075): what the real-time guest clock follows (pacing.h).
//   display  each game tick lasts a whole number of display refreshes when the refresh rate is
//            a multiple of the tick rate (60, 120 Hz at 50 ms), so no frame is held a refresh
//            more or less than the others; otherwise the wall clock (the default)
//   wall     the wall clock, as before #80 (the run loop wakes about every millisecond)
// The fixed-step mode (--fixed-step) has its own virtual clock and ignores it.
enum class PacingMode { kDisplay, kWall };
inline constexpr const char *kPacingNames[] = {"display", "wall"};
// "display", "wall".
const char *pacing_mode_name(PacingMode mode);

// border (#157, ADR 0085): what fills the drawable around the 3:2 picture (border.h).
//   color  the border colour (border_color), as before #157 (the default)
//   blur   the current frame stretched to cover the window, blurred and dimmed
//   image  the PNG of border_image, scaled to cover the window (over the border colour where it
//          is transparent); without one, or one that cannot be read, the border colour (logged)
enum class BorderMode { kColor, kBlur, kImage };
inline constexpr const char *kBorderModeNames[] = {"color", "blur", "image"};
// "color", "blur", "image".
const char *border_mode_name(BorderMode mode);
// game_speed (#151, ADR 0087, game_speed.h): the game's tick length. An Assist: `game` in every
// preset.
//   game            the game's own Speed option decides (65, 50 or 40 ms; 50 by default), as
//                   the original
//   65, 50, 40      the game's own three lengths, whatever its Speed option says
//   33, 25          two faster ones the game does not have (30 and 40 ticks a second)
// Any value but `game` writes its length into all three entries of the game's table: the
// game's Speed option then changes nothing, and the game's config file keeps the option's own
// value (never a port-only one).
enum class GameSpeed { kGame, k65, k50, k40, k33, k25 };
inline constexpr const char *kGameSpeedNames[] = {"game", "65", "50", "40", "33", "25"};
// "game", "65", ...
const char *game_speed_name(GameSpeed speed);
// fast_forward (#151, ADR 0087): what holding the fast_forward action (bindings.h; Tab and the
// right stick's click by default) does: nothing (off, every preset) or the real-time guest
// clock at 2 or 3 times the wall clock's speed while it is held.
enum class FastForward { kOff, kX2, kX3 };
inline constexpr const char *kFastForwardNames[] = {"off", "x2", "x3"};
// "off", "x2", "x3".
const char *fast_forward_name(FastForward ff);
// colorblind_filter (#158, ADR 0093, color_filter.h): the picture's colours daltonised for a
// dichromacy, so that colours the player confuses become ones they can tell apart. Off by
// default; not in the presets (the player's own need).
enum class ColorblindFilter { kOff, kProtanopia, kDeuteranopia, kTritanopia };
inline constexpr const char *kColorblindFilterNames[] = {"off", "protanopia", "deuteranopia",
                                                         "tritanopia"};
// "off", "protanopia", "deuteranopia", "tritanopia".
const char *colorblind_filter_name(ColorblindFilter filter);
// screen_shake's largest value (#158): the game's own shake, in percent.
inline constexpr std::uint32_t kMaxScreenShake = 100;
// flashes' largest value (#186): the game's own full-screen flashes, in percent of their alpha.
inline constexpr std::uint32_t kMaxFlashes = 100;
// The picture adjustments (#187, ADR 0093's addendum, color_filter.h): brightness, contrast and
// saturation in percent, gamma in hundredths (the file writes it as 0.80 ... 1.40). Video; not in
// the presets (the player's screen, not the experience).
inline constexpr std::uint32_t kPictureMin = 50, kPictureMax = 150, kPictureStep = 5;
inline constexpr std::uint32_t kPictureDefault = 100;
inline constexpr std::uint32_t kGammaMin = 80, kGammaMax = 140, kGammaStep = 5;
inline constexpr std::uint32_t kGammaDefault = 100;

// ui_animations (#90, ADR 0092): the speed of the game's UIKit animations (the title menu's
// fades and slides, the character select's), through mt_uikit_set_animation_scale. Only the
// presentation: no game code waits on them (docs/re/qol.md).
//   normal   as the game asked (the default, every preset)
//   fast     four times as fast (delays and durations a quarter)
//   instant  none: each change shows at once
enum class UiAnimations { kNormal, kFast, kInstant };
inline constexpr const char *kUiAnimationsNames[] = {"normal", "fast", "instant"};
const char *ui_animations_name(UiAnimations a);
// The UIKit animation scale of `a`: 1, 0.25, 0.
double ui_animation_scale(UiAnimations a);
// auto_potion (#90, ADR 0092, assists.h): an Assist. Under that percentage of its max HP, in
// the field, the hero drinks the HP potion of its quick slots, through the slot's own key.
enum class AutoPotion { kOff, k25, k50, k75 };
inline constexpr const char *kAutoPotionNames[] = {"off", "25", "50", "75"};
const char *auto_potion_name(AutoPotion a);
// The percentage of `a` (0: off).
std::uint32_t auto_potion_percent(AutoPotion a);
// minimap (#176, ADR 0098, minimap.h): the port's minimap of the current map (its .att grid,
// the hero, portals and NPCs), drawn into the window only, in the field.
//   off      none (the default, every preset)
//   border   in the bar beside the picture when there is room, else as overlay
//   overlay  over the picture's top edge, right of the compass; with widescreen and
//            widescreen_hud = edges in the visible view's top left corner, the compass moved
//            right of it (minimap_anchor)
enum class Minimap { kOff, kBorder, kOverlay };
inline constexpr const char *kMinimapNames[] = {"off", "border", "overlay"};
// hold_toggle (#172, ADR 0094, assists.h): an Assist. The held actions that a press toggles
// instead (the game holds only walking; the port's fast-forward is held too).
enum class HoldToggle { kOff, kWalk, kFastForward, kAll };
inline constexpr const char *kHoldToggleNames[] = {"off", "walk", "fast_forward", "all"};
const char *hold_toggle_name(HoldToggle h);
// Whether `h` toggles walking (left / right) and fast-forward.
bool hold_toggle_walk(HoldToggle h);
bool hold_toggle_fast_forward(HoldToggle h);

struct Settings {
    // The preset (#144): empty when nothing chose one yet (no file, or one without the key).
    std::optional<Preset> preset;
    ScaleMode scale_mode = ScaleMode::kInteger; // scale_mode, --scale-mode
    Color border_color;                         // border_color, --border-color (black)
    // render_scale, --render-scale: 1 to 8, or kRenderScaleAuto (`auto`, #152, ADR 0080: the
    // largest N that fits the frame budget and the window, render_scale.h).
    std::uint32_t render_scale = 1;
    GfxBackend gfx_backend = GfxBackend::kSoftware; // gfx_backend, --gfx-backend
    ShaderFilter filter = ShaderFilter::kNone;      // filter, --filter
    // hd_text, --hd-text (on | off; #88): the text the game draws into its text textures is
    // drawn a second time at render_scale times the size, and the textures sample that copy
    // (maplethief/cg.h). No effect at render scale 1.
    bool hd_text = false;
    // Audio (#64, ADR 0045's buses): percent, 0 to 100; the bus gain is volume / 100.
    std::uint32_t master_volume = 100;  // master_volume, --master-volume
    std::uint32_t music_volume = 100;   // music_volume, --music-volume
    std::uint32_t effects_volume = 100; // effects_volume, --effects-volume
    bool mute_unfocused = true;         // mute_unfocused, --mute-unfocused (on | off)
    // ADR 0062: the music's loops without its files' leading and trailing digital silence.
    bool gapless_music = false;                 // gapless_music, --gapless-music (on | off)
    RightTouch right_touch;                     // right_touch, --right-touch (off | X,Y)
    KeypadMode hide_keypad = KeypadMode::kAuto; // hide_keypad, --hide-keypad
    // #195 (ADR 0045's addendum): the mix as (L + R) / 2 in both channels (mt_audio_set_mono),
    // and the playback device by SDL's name for it (empty: `default`, the system's; a device
    // that is not there plays on the default and the name is kept for when it is back).
    bool audio_mono = false;  // audio_mono, --audio-mono (on | off)
    std::string audio_device; // audio_device, --audio-device (default | NAME)
    // #188 (ADR 0106, maplethief/audio.h): audio_pack, --audio-pack DIR (empty: none), a folder
    // of the player's own music and sound files that replace the game's by file name.
    std::string audio_pack;
    // Rumble (#72, rumble.h): the active gamepad on a vibration, a hit and a level-up.
    bool rumble = true;                  // rumble, --rumble (on | off)
    std::uint32_t rumble_strength = 100; // rumble_strength, --rumble-strength (0 to 100)
    // #184 (disconnect.h): the active pad going away pauses the game until a pad or a key.
    bool pause_on_disconnect = false; // pause_on_disconnect, --pause-on-disconnect (on | off)
    // The frame-time overlay (#137, perf_overlay.h): shown from the start; F3 toggles it.
    bool perf_overlay = false; // perf_overlay, --perf-overlay (on | off)
    // The offline cash shop (#156, ADR 0072): an Assist.
    CashShop cash_shop = CashShop::kOff;      // cash_shop, --cash-shop (off | meso)
    PacingMode pacing = PacingMode::kDisplay; // pacing, --pacing (display | wall)
    // Saves (#148, ADR 0078, save_tools.h). save_dir: the folder the game's Documents/ (its save
    // files) maps to, an absolute path (a cloud-synced folder); empty: <data dir>/Documents.
    std::string save_dir;            // save_dir, --save-dir
    std::uint32_t save_backups = 10; // save_backups, --save-backups (0 to 100)
    // Desktop conveniences (#145, desktop.h, ADR 0077).
    // fullscreen, --fullscreen (off | on; desktop and borderless read as on in that mode, the
    // file's value before #179 and the flag's shorthand).
    bool fullscreen = false;
    FullscreenMode fullscreen_mode = FullscreenMode::kDesktop; // fullscreen_mode, --fullscreen-mode
    bool vsync = true; // vsync, --vsync (on | off): SDL_RenderPresent waits for the display
    // pause_unfocused, --pause-unfocused (on | off): the window losing focus pauses the game as
    // a minimise does (lifecycle.h).
    bool pause_unfocused = false;
    Language language = Language::kEn; // language, --language (en | ko)
    // fix_original_bugs, --fix-original-bugs (on | off; #155, docs/re/original-bugs.md): the
    // original game's crashes and dead ends fixed through guest function hooks
    // (maplethief/hooks.h): the NGB portal items' exit(1), Play before the engine's main menu
    // (#122). Never balance: a fix that changes gameplay is an Assist.
    bool fix_original_bugs = false;
    // Texture packs (#153, ADR 0081, maplethief/texpack.h): texture_pack, --texture-pack DIR
    // (empty: none), a directory of replacements made from the player's own dump; and
    // texture_filter, --texture-filter (game | nearest).
    std::string texture_pack;
    TextureFilter texture_filter = TextureFilter::kGame;
    // Offline polish (#154, ADR 0084, maplethief/offline.h): the title menu without its dead
    // online buttons (the banner, More Games, About's links; the Game Center button too while
    // local_game_center is off), and Game Center on this computer: the leaderboard screen and
    // local achievements with a toast.
    bool hide_online_ui = false;    // hide_online_ui, --hide-online-ui (on | off)
    bool local_game_center = false; // local_game_center, --local-game-center (on | off)
    // Menus (#147, menu_input.h): held directions repeat on the key pad's menu panels; a popup
    // that just opened ignores confirm and back for a moment.
    bool menu_repeat = false;   // menu_repeat, --menu-repeat (on | off)
    bool confirm_guard = false; // confirm_guard, --confirm-guard (on | off)
    // Controller hints (#146, prompts.h): the focus highlight and the port's selection markers,
    // and the button prompts next to the game's buttons (both with a gamepad or the keyboard).
    bool focus_highlight = true; // focus_highlight, --focus-highlight (on | off)
    bool button_prompts = false; // button_prompts, --button-prompts (on | off)
    // The field key pad's button labels (ADR 0101, hud_labels.h): each key's control beside it.
    bool button_labels = true; // button_labels, --button-labels (on | off)
    // The touch key pad's groups resized, moved and faded (#159, keypad_layout.h): original.
    KeypadLayout keypad_layout; // keypad_layout, --keypad-layout
    // The border (#157, ADR 0085): border, --border (color | blur | image); border_image,
    // --border-image FILE (a PNG; empty: none; a relative path is from the working directory).
    BorderMode border = BorderMode::kColor;
    std::string border_image;
    // Assists (#151, ADR 0087): the game's tick length, and the fast-forward key's speed.
    GameSpeed game_speed = GameSpeed::kGame;      // game_speed, --game-speed
    FastForward fast_forward = FastForward::kOff; // fast_forward, --fast-forward (off | x2 | x3)
    Autosave autosave = Autosave::kOff;           // autosave, --autosave (off | on_quit; #150)
    // The wider field (#86, ADR 0090): widescreen, --widescreen (off | auto).
    Widescreen widescreen = Widescreen::kOff;
    // Where the HUD and the key pad go in the wide view (#168): widescreen_hud, --widescreen-hud.
    WidescreenHud widescreen_hud = WidescreenHud::kEdges;
    // Accessibility (#158, ADR 0093): the colour-blind filter (the present stage's first step),
    // and the scenes' camera shake in percent of the game's (maplethief/hooks.h's
    // mt_hooks_install_screen_shake; 100: the original).
    ColorblindFilter colorblind_filter = ColorblindFilter::kOff; // --colorblind-filter
    std::uint32_t screen_shake = kMaxScreenShake;                // --screen-shake (0 to 100)
    // The full-screen flashes' alpha in percent (#186; mt_hooks_install_flashes; 100: original).
    std::uint32_t flashes = kMaxFlashes; // --flashes (0 to 100)
    // The picture adjustments (#187): the present stage's colour step with the filter.
    std::uint32_t brightness = kPictureDefault; // --brightness (50 to 150, percent)
    std::uint32_t contrast = kPictureDefault;   // --contrast (50 to 150, percent)
    std::uint32_t saturation = kPictureDefault; // --saturation (50 to 150, percent)
    std::uint32_t gamma = kGammaDefault;        // --gamma (0.80 to 1.40, in hundredths)
    // Frame interpolation (#87, ADR 0091): frame_rate, --frame-rate (original | smooth).
    FrameRate frame_rate = FrameRate::kOriginal;
    // Quality of life (#90, ADR 0092, docs/re/qol.md): the UIKit animations' speed (Video), and
    // two Assists: attack repeating while held (turbo_attack) and the HP potion drunk under a
    // percentage of max HP (auto_potion).
    UiAnimations ui_animations = UiAnimations::kNormal; // ui_animations, --ui-animations
    bool turbo_attack = false;                          // turbo_attack, --turbo-attack (on | off)
    AutoPotion auto_potion = AutoPotion::kOff;          // auto_potion, --auto-potion
    // The scene skip (#174, ADR 0096, scene_skip.h): scenes run at kSceneSkipRate times the
    // game's speed with each speech box tapped; every frame and command still runs.
    bool skip_intros = false; // skip_intros, --skip-intros (on | off)
    // The port's minimap (#176, ADR 0098): minimap, --minimap (off | border | overlay).
    Minimap minimap = Minimap::kOff;
    // Controller extras (#172, ADR 0094, assists.h): two Assists, the auto-run action latching
    // the walk (auto_run) and presses toggling held actions (hold_toggle).
    bool auto_run = false;                     // auto_run, --auto-run (on | off)
    HoldToggle hold_toggle = HoldToggle::kOff; // hold_toggle, --hold-toggle
    // Auto-loot (#175, assists.h): the game's own pick-up of the drops near the hero.
    bool auto_loot = false; // auto_loot, --auto-loot (on | off)

    bool operator==(const Settings &) const = default;
};

// save_dir's longest value (the runtime's path buffer, maplethief/saves.h, less room for a name).
inline constexpr std::size_t kMaxSaveDirBytes = 900;
inline constexpr std::uint32_t kMaxSaveBackups = 100;

inline constexpr std::uint32_t kRenderScaleAuto = 0; // render_scale = auto
inline constexpr std::uint32_t kMaxVolume = 100;
inline constexpr std::uint32_t kMaxRumbleStrength = 100;

// --- The registry (#144, ADR 0070) ------------------------------------------------------------

// The settings menu's sections (#89). Enhancements (presentation and comfort) sit in Video,
// Audio, Controls, Gameplay and Accessibility; Assists hold whatever changes gameplay (game
// speed, turbo, auto-potion, cheats) and are never turned on by a preset.
enum class SettingGroup {
    kVideo,
    kAudio,
    kControls,
    kGameplay,
    kAccessibility,
    kAssists,
    kSaves,
    kOther
};
// "Video", "Audio", "Controls", "Gameplay", "Accessibility", "Assists", "Saves", "Other".
const char *setting_group_name(SettingGroup group);

// A value's type, which says how the menu edits it.
enum class ValueKind {
    kChoice, // one of a list of names (an enum whose values are 0, 1, ... in the names' order)
    kOnOff,  // on | off (also true/false, yes/no, 1/0 when read)
    kUint,   // a whole number from min to max (at most three digits)
    kColor,  // #RRGGBB
    kCustom, // its own parser and printer
};

struct SettingValue;
// Reads `text` (trimmed) into the row's field; false (the field unchanged) if it is not a value.
using SettingParse = bool (*)(const SettingValue &value, std::string_view text, Settings *s);
// The field's value as the file writes it.
using SettingPrint = std::string (*)(const SettingValue &value, const Settings &s);

// A row's value: its type and the field it is bound to (through parse and print). Made by
// choice<&Settings::f>(names), on_off<&Settings::f>(), uint_range<&Settings::f>(min, max, step,
// zero), color<&Settings::f>() or custom_value(parse, print, expect).
struct SettingValue {
    ValueKind kind = ValueKind::kCustom;
    SettingParse parse = nullptr;
    SettingPrint print = nullptr;
    // kChoice: the names, value 0 first. kColor, kCustom: the values the settings menu offers
    // (#89), as the file writes them; empty: none (the menu shows the value only).
    std::span<const char *const> names = {};
    std::uint32_t min = 0, max = 0; // kUint: the range
    std::uint32_t step = 1;         // kUint: the menu's step
    // kCustom: the values the menu offers when they are only known while the game runs (the
    // audio devices, #195), instead of `names`; nullptr: none.
    std::vector<std::string> (*offer)() = nullptr;
    // kUint: a name for the value 0, below the range (render_scale's "auto"), or nullptr. It
    // reads and writes as that name and comes first in the menu's choices.
    const char *zero = nullptr;
    // kCustom: what a good value is, after "is not" in the message about a bad one.
    const char *expect = nullptr;
};

// A row's value in each preset: the text the file would have (exactly as the row's printer
// writes it). All three null: kNotInPresets, the setting keeps its own value in every preset.
struct PresetColumns {
    const char *original = nullptr;
    const char *original_controller = nullptr;
    const char *enhanced = nullptr;

    constexpr bool in_presets() const { return original != nullptr; }
    // The column of a preset; nullptr for custom or outside the presets.
    constexpr const char *operator[](Preset p) const {
        return p == Preset::kOriginal             ? original
               : p == Preset::kOriginalController ? original_controller
               : p == Preset::kEnhanced           ? enhanced
                                                  : nullptr;
    }
};
inline constexpr PresetColumns kNotInPresets{};

// When the frontend applies a change of the setting while the game runs (#89, ADR 0076).
enum class Applies {
    kNextStart, // read at start-up only: a change counts from the next start
    // The frontend applies a change at once (live_settings.h's apply_live_setting); if it
    // cannot this time (a filter without SDL's GPU renderer), the menu says "at next start".
    kAtOnce,
};

// One setting. The table is in settings.cpp, written with designated initialisers.
struct SettingInfo {
    const char *key;     // lower-case letters, digits and '_'
    SettingGroup group;  // the menu's section
    const char *comment; // the line(s) above the key in a new file, each "# ...", '\n' between
    SettingValue value;  // the type, bound to the field
    PresetColumns presets = kNotInPresets;
    const char *flag = nullptr; // "--scale-mode": a command-line flag that sets it, or none
    bool game_only = false;     // the flag is a game option (not for the placeholder or tests)
    const char *label = "";     // the settings menu's label (#89), e.g. "Scale mode"
    const char *help = "";      // one line under it in the menu
    Applies applies = Applies::kNextStart;
    // The settings menu shows the frame-time cost next to it (FrameStats::recent(); #89).
    bool frame_cost = false;
    // While it is selected the settings menu shows its test pattern behind the rows (a grey ramp
    // and the game's own picture, undimmed: the picture adjustments, #187).
    bool test_pattern = false;
};

namespace settings_detail {
// The index of `text` in `names`, ASCII case-insensitive; -1 if none.
int find_name(std::span<const char *const> names, std::string_view text);
bool parse_on_off(std::string_view text, bool *out);
// Digits only, at most 3 of them.
bool parse_small_uint(std::string_view text, std::uint32_t *out);
std::string color_text(const Color &c);

template <auto Field> using FieldType = std::remove_cvref_t<decltype(Settings{}.*Field)>;

template <auto Field> bool parse_choice(const SettingValue &v, std::string_view t, Settings *s) {
    const int i = find_name(v.names, t);
    if (i < 0) {
        return false;
    }
    s->*Field = static_cast<FieldType<Field>>(i);
    return true;
}
template <auto Field> std::string print_choice(const SettingValue &v, const Settings &s) {
    const auto i = static_cast<std::size_t>(s.*Field);
    return i < v.names.size() ? v.names[i] : "?";
}
template <auto Field> bool parse_switch(const SettingValue &, std::string_view t, Settings *s) {
    return parse_on_off(t, &(s->*Field));
}
template <auto Field> std::string print_switch(const SettingValue &, const Settings &s) {
    return s.*Field ? "on" : "off";
}
template <auto Field> bool parse_uint(const SettingValue &v, std::string_view t, Settings *s) {
    if (v.zero != nullptr && find_name(std::span<const char *const>(&v.zero, 1), t) == 0) {
        s->*Field = 0;
        return true;
    }
    std::uint32_t n = 0;
    if (!parse_small_uint(t, &n) || n < v.min || n > v.max) {
        return false;
    }
    s->*Field = n;
    return true;
}
template <auto Field> std::string print_uint(const SettingValue &v, const Settings &s) {
    if (v.zero != nullptr && s.*Field == 0) {
        return v.zero;
    }
    return std::to_string(s.*Field);
}
template <auto Field> bool parse_rgb(const SettingValue &, std::string_view t, Settings *s) {
    return parse_color(t, &(s->*Field));
}
template <auto Field> std::string print_rgb(const SettingValue &, const Settings &s) {
    return color_text(s.*Field);
}
} // namespace settings_detail

// The value types, bound to a field of Settings.
template <auto Field> constexpr SettingValue choice(std::span<const char *const> names) {
    static_assert(std::is_enum_v<settings_detail::FieldType<Field>>);
    return {.kind = ValueKind::kChoice,
            .parse = &settings_detail::parse_choice<Field>,
            .print = &settings_detail::print_choice<Field>,
            .names = names};
}
template <auto Field> constexpr SettingValue on_off() {
    static_assert(std::is_same_v<settings_detail::FieldType<Field>, bool>);
    return {.kind = ValueKind::kOnOff,
            .parse = &settings_detail::parse_switch<Field>,
            .print = &settings_detail::print_switch<Field>};
}
template <auto Field>
constexpr SettingValue uint_range(std::uint32_t min, std::uint32_t max, std::uint32_t step = 1,
                                  const char *zero = nullptr) {
    static_assert(std::is_same_v<settings_detail::FieldType<Field>, std::uint32_t>);
    return {.kind = ValueKind::kUint,
            .parse = &settings_detail::parse_uint<Field>,
            .print = &settings_detail::print_uint<Field>,
            .min = min,
            .max = max,
            .step = step,
            .zero = zero};
}
// `offered`: the colours the settings menu steps through (#89), "#RRGGBB" as the file writes.
template <auto Field> constexpr SettingValue color(std::span<const char *const> offered = {}) {
    static_assert(std::is_same_v<settings_detail::FieldType<Field>, Color>);
    return {.kind = ValueKind::kColor,
            .parse = &settings_detail::parse_rgb<Field>,
            .print = &settings_detail::print_rgb<Field>,
            .names = offered};
}
// `offered`: the values the settings menu steps through (#89), as the printer writes them.
constexpr SettingValue custom_value(SettingParse parse, SettingPrint print, const char *expect,
                                    std::span<const char *const> offered = {}) {
    return {.kind = ValueKind::kCustom,
            .parse = parse,
            .print = print,
            .names = offered,
            .expect = expect};
}

// audio_device (#195): the playback devices' names the settings menu offers after `default`
// (audio_out.cpp sets them from SDL_GetAudioPlaybackDevices when it opens the device and when
// one comes or goes). Empty until then: the menu then offers no values.
void set_audio_device_choices(std::vector<std::string> names);
inline constexpr const char *kAudioDeviceDefault = "default";
inline constexpr std::size_t kMaxAudioDeviceBytes = 256;

// Every setting, in the order a new file lists them (after `preset`).
std::span<const SettingInfo> settings_registry();
// The row of a key (ASCII case-insensitive) or of a flag ("--hd-text"); nullptr if none. The
// preset is not a row: kPresetKey and kPresetFlag.
const SettingInfo *find_setting(std::string_view key);
const SettingInfo *find_setting_flag(std::string_view flag);
inline constexpr const char *kPresetKey = "preset";
inline constexpr const char *kPresetFlag = "--preset";

// A row's value in `settings`, as the file writes it.
std::string setting_text(const Settings &settings, const SettingInfo &info);
// What a good value of the row is: "integer, fit, smooth or sharp", "on or off", "an integer
// from 0 to 100" ("auto or an integer from 1 to 8" with a name for 0), "#RRGGBB", or the custom
// row's own words.
std::string setting_expects(const SettingInfo &info);
// The values the menu cycles through: a choice's names, "off" and "on", every step of a range
// (after the range's name for 0, if it has one), or the values a colour or a custom row offers
// (empty if it offers none).
std::vector<std::string> setting_choices(const SettingInfo &info);
// The menu's Left / Right (#89): the previous or next of setting_choices (wrapping for a choice,
// a switch or offered values, clamped for a range), through apply_setting (so a preset moves
// to custom). A value not among the offered ones steps to the first (+1) or last (-1). False
// if the row has no choices.
bool step_setting(Settings *settings, const SettingInfo &info, int delta);

// Checks a table of rows (the registry's own is checked by a static_assert and the tests):
// keys and flags unique and well formed (a flag is "--" and the key with '-' for '_'), comment
// lines starting with '#', every preset column a value the row reads back as the same text, and
// every Assists row in the presets with the same value in each column, the value a
// default-constructed Settings has (off / neutral). Returns one message per problem.
std::vector<std::string> check_registry(std::span<const SettingInfo> rows);

// --- Presets ----------------------------------------------------------------------------------

// Sets every row in the presets to the preset's column and settings->preset to the preset;
// custom changes only settings->preset. Keys outside the presets keep their values.
void apply_preset(Settings *settings, Preset preset);
// The rows in the presets whose value in `settings` is not the preset's column (custom: none).
std::vector<const SettingInfo *> preset_differences(const Settings &settings, Preset preset);
// The first of original, original_controller and enhanced whose column `settings` has in full;
// empty if none.
std::optional<Preset> matching_preset(const Settings &settings);

// --- The file ---------------------------------------------------------------------------------

// Sets one setting from its key and value (both already trimmed), as the command line and the
// settings menu change a setting. `preset` applies the preset (apply_preset). Another key in the
// presets moves a preset other than custom to custom when its new value is not the preset's.
// Returns false, with `why`, for an unknown key or a bad value, leaving `settings` unchanged.
bool apply_setting(Settings *settings, std::string_view key, std::string_view value,
                   std::string *why);

// A list of settings, "key = value; key = value" (blanks around each part dropped, empty parts
// skipped), applied in order with apply_setting. MAPLETHIEF_SETTINGS holds one (#90, ADR 0092:
// the game applies it after the file and --preset, before the other flags; the test driver
// game_driver.h too), so a whole test suite can run with a feature on. Returns false, with
// `why` naming the part, at the first bad part; the parts before it stay applied.
bool apply_setting_list(Settings *settings, std::string_view list, std::string *why);

// Parses a settings file's text over `settings`. Returns the problems found, one message per
// ignored line ("line 3: unknown key 'scael_mode'"), in order; empty if there were none. With a
// `preset` line (the last good one counts) other than custom, that preset's columns win over
// the file's lines for the keys it controls ("line 7: hd_text = off: preset = enhanced sets
// on, ignored"); without one, each line is apply_setting.
std::vector<std::string> parse_settings(std::string_view text, Settings *settings);

// The file's text for `settings`. With `old_text` (the file as it is) empty: `preset` (when
// set) and every key with its comment line(s), in the registry's order. Otherwise `old_text` is
// kept byte for byte (comments, unknown keys, malformed lines, a byte order mark, its line
// endings), except that the value of each line that sets a known key is replaced by the
// setting's value, and the keys it lacks are appended, each after its comment. `preset` counts
// as known only when set (a preset line in the old text with no preset in `settings` becomes a
// comment). parse_settings(serialise_settings(s, t)) gives s for any t; an old text over
// kMaxSettingsBytes (which parse_settings ignores) is replaced whole.
std::string serialise_settings(const Settings &settings, std::string_view old_text = {});

// Text for a log message: at most `max` bytes of `s`, bytes outside printable ASCII as \xNN,
// "..." if cut.
std::string printable(std::string_view s, std::size_t max = 48);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_SETTINGS_H
