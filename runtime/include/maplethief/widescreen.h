/* Widescreen (#86, ADR 0090, docs/re/widescreen.md): the field drawn wider than 480 points.
 *
 * The engine takes its screen size from Screen_Init(480, 320) 0x9909 once: the screen surface
 * 0x11AA88, the global 0x11AA8C and SkyCanvas +0x88 / +0x90 (width, half width). The camera's
 * clamp 0xC069 and follow 0xC175, the sprite and object culling and the full-screen clip read it
 * at draw time, and the sprite draw 0x7CCD clips every quad on the CPU to the surface's width and
 * the clip rect, so nothing past column 480 is ever drawn unless the engine itself believes the
 * screen is wider. No update function reads the width or the camera (docs/re/widescreen.md).
 *
 * The port therefore:
 *   - gives the GL renderbuffer a wider storage (`width`, 568: the iPhone 5's 568 x 320 points)
 *     when -[EAGLContext renderbufferStorage:fromDrawable:] sees the 480-point layer, so that
 *     -[EAGLView setupView] sets glViewport and glOrthox to 568 x 320;
 *   - sets the engine's view width E each turn and in the camera hooks: in the field (the current
 *     state a CStateGame, game mode 0x11AA00 0 or 4, a map loaded) while the host allows it, E is
 *     the map's width clamped to 480 ... `width`; everywhere else 480. The original clamp then
 *     keeps the camera inside the map, and the culling widens with E;
 *   - stretches the field's backdrop (CStateGame +0x454, a fixed 480 x 320 image drawn at (0, 0)
 *     through 0x24DC9) to E columns with mt_gles_set_host_x_transform;
 *   - moves the field HUD (HUD_Paint 0x5A465: the bar, the quick slots and the attack key's
 *     icon, drawn at fixed 480-point places) right by (E - 480) / 2 points, into the 3:2
 *     picture's place in the middle of the view, over the key pad's keys (#168).
 *
 * The GL frame then holds the picture in its first E columns; the host shows them centred
 * (platform/src/present.h). Everything here runs on the guest thread. */
#ifndef MAPLETHIEF_WIDESCREEN_H
#define MAPLETHIEF_WIDESCREEN_H

#include <stdint.h>

#include "maplethief/recomp/recomp.h"

MT_EXTERN_C_BEGIN

#define MT_WIDESCREEN_NARROW 480u /* the original width, points */
#define MT_WIDESCREEN_WIDE 568u   /* the wide renderbuffer's width (16:9 at 320 high, iPhone 5) */
#define MT_WIDESCREEN_HEIGHT 320u /* the height, points: never changes */

/* Before the boot: the renderbuffer's width (MT_WIDESCREEN_WIDE), or 0 / 480 for the original.
 * Registers the four hooks (camera clamp, follow, the backdrop's draw, the HUD), on when wide. */
void mt_widescreen_configure(uint32_t width) MT_NOEXCEPT;
/* The configured width: 480 when off. */
uint32_t mt_widescreen_width(void) MT_NOEXCEPT;

/* The view width the engine should draw at now: 480, or in the field (see above) with `allow`
 * the map's width clamped to 480 ... `width`. Reads guest memory only; 480 without the game's
 * globals. */
uint32_t mt_widescreen_decide(const mt_cpu *cpu, uint32_t width, int allow) MT_NOEXCEPT;

/* Once a run-loop turn, before the guest's frame: whether the host allows the wide view now
 * (the setting), then decides and applies E (as the camera hooks do too).
 * Returns E. With widescreen off it does nothing and returns 480. */
uint32_t mt_widescreen_turn(mt_cpu *cpu, int allow) MT_NOEXCEPT;

/* The view width in effect: the last one applied (480 before any). */
uint32_t mt_widescreen_view(void) MT_NOEXCEPT;

/* Writes view width `e` into the engine: the screen surface (width, bytes a row), 0x11AA8C,
 * SkyCanvas +0x88 / +0x90, and the canvas's clip rect when it is the full screen of the old
 * width. Nothing without the game's globals. */
void mt_widescreen_apply(mt_cpu *cpu, uint32_t e) MT_NOEXCEPT;

typedef struct mt_widescreen_stats {
    uint64_t turns;        /* mt_widescreen_turn calls that decided */
    uint64_t wide_turns;   /* of them with E > 480 */
    uint64_t changes;      /* times E changed */
    uint64_t camera_calls; /* camera hook calls (clamp, follow) */
    uint64_t backdrops;    /* backdrops stretched */
    uint64_t huds;         /* field HUDs moved to the middle of a wide view (#168) */
} mt_widescreen_stats;

mt_widescreen_stats mt_widescreen_get_stats(void) MT_NOEXCEPT;
/* Back to off, E = 480, the numbers zeroed, the hooks unregistered
 * (tests). */
void mt_widescreen_reset(void) MT_NOEXCEPT;

MT_EXTERN_C_END

#endif
