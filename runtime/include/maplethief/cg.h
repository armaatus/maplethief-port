/* CoreGraphics' host options (#88, ADR 0066): the game's text at the output resolution.
 *
 * The game draws its engine text with UIKit into a bitmap context over guest memory
 * (TextCache_DrawText 0x9031: a 512x32 or 32x32 buffer, -drawAtPoint:withFont:) and uploads
 * that buffer with glTexImage2D. At render scale N the text texture is still 1x, so its glyphs
 * are magnified N times. With a text scale k > 1:
 *
 *   - Every -drawAtPoint:withFont: into a bitmap context over guest memory (32-bit pixels, rows
 *     of exactly width * 4 bytes) is drawn a second time into the context's shadow: a k-times
 *     larger image in the same pixel layout, with the CTM scaled by k. The glyphs are rasterized
 *     at k times the size, not magnified.
 *   - Before that draw, the shadow is checked against the 1x pixels. It is still their k-times
 *     image only if the 1x pixels' hash equals the one recorded after the shadow's last draw.
 *     Otherwise something else drew there (the game clears the buffer between texts), so the
 *     shadow is rebuilt from the 1x pixels, each pixel copied to k x k pixels. After the draw
 *     the shadow records the 1x pixels' new hash: mt_gfx_upload_hash of the buffer as a
 *     GL_RGBA / GL_UNSIGNED_BYTE upload.
 *   - mt_cg_text_replace, an mt_gfx_replace_fn for mt_gfx_set_replace_hook, replaces a
 *     GL_RGBA / GL_UNSIGNED_BYTE upload whose size and hash are a shadow's with that shadow's
 *     pixels. If the buffer was changed after the text was drawn, its hash no longer matches, and
 *     the upload stays as it is. So a replacement is always the upload's own content, drawn at
 *     k times the resolution.
 *   - The last 4 shadows drawn stay findable after their context is released, because
 *     TextCache_DrawText releases the context before it uploads the buffer.
 *   - Not shadowed (logged once, counted): a text draw through a clip, or into a context of
 *     another layout. Host-memory contexts (UIGraphicsBeginImageContext, the compositor's
 *     views) are never shadowed: their images don't become textures directly, and the
 *     compositor already draws the shell's text at the composite's resolution.
 *
 * k is the frontend's render scale when settings.ini says hd_text = on. Default 1: off, and
 * nothing changes. Kept across runtimes; the shadows are forgotten when the ObjC runtime
 * changes.
 *
 * Single guest thread (ADR 0033). C17, also C++20. */
#ifndef MAPLETHIEF_CG_H
#define MAPLETHIEF_CG_H

#include <stddef.h>
#include <stdint.h>

#include "maplethief/gfx.h"
#include "maplethief/recomp/recomp.h"

MT_EXTERN_C_BEGIN

/* The text scale: 1 (off, the default) to MT_GFX_MAX_SCALE; other values are clamped. */
void mt_cg_set_text_scale(uint32_t k) MT_NOEXCEPT;
uint32_t mt_cg_text_scale(void) MT_NOEXCEPT;

/* The replacement hook (see above). `user` is unused. Install it with
 * mt_gfx_add_replace_hook(MT_GFX_REPLACE_ORDER_TEXT, mt_cg_text_replace, NULL): it goes before a
 * texture pack's hook (#153, ADR 0081). */
int mt_cg_text_replace(void *user, const mt_gfx_upload *upload,
                       mt_gfx_replacement *out) MT_NOEXCEPT;

typedef struct mt_cg_text_stats {
    uint64_t shadowed_draws; /* text draws drawn into a shadow too */
    uint64_t rebuilds;       /* shadows rebuilt from the 1x pixels before a draw */
    uint64_t unshadowed;     /* text draws into guest memory that could not be shadowed */
    uint64_t replacements;   /* uploads the hook replaced */
} mt_cg_text_stats;

/* The counters since the text scale was last set. */
mt_cg_text_stats mt_cg_get_text_stats(void) MT_NOEXCEPT;

/* --- UIKit images by file name (#153, ADR 0081: texture packs' menu art) ------------------
 *
 * The shell's images (+[UIImage imageNamed:], +[UIImage imageWithContentsOfFile:]) never pass
 * through mt_gfx: the compositor draws them (ADR 0037). The image hook sees each such image
 * once per bundle file, when it is first decoded, and may hand back an image k times its size
 * (one integer k from 1 to MT_GFX_MAX_SCALE for both axes, at most MT_GFX_MAX_SIZE). The guest
 * keeps the original (its CGImage, its size); only the compositor draws the replacement into the
 * view's rectangle. A wrong size is rejected (logged, counted). */

typedef struct mt_cg_image_file {
    const char *file;       /* the bundle-relative path, '/'-separated ("MainMenuBG.png") */
    uint32_t width, height; /* the decoded image's size in pixels */
    const uint8_t *rgba;    /* its pixels, straight RGBA, top row first; valid during the call */
} mt_cg_image_file;

typedef struct mt_cg_image_replacement {
    const uint8_t *rgba; /* straight RGBA (as a PNG decodes), top row first, width * 4 per row */
    uint32_t width, height;
} mt_cg_image_replacement;

/* Returns 1 after filling *out, 0 to keep the image. The pixels are copied right after the hook
 * returns. Runs on the guest's thread, inside the UIImage call. */
typedef int (*mt_cg_image_replace_fn)(void *user, const mt_cg_image_file *image,
                                      mt_cg_image_replacement *out);

/* Installs the hook (NULL: none, the default); kept across runtimes. Images decoded before keep
 * what they got. */
void mt_cg_set_image_replace_hook(mt_cg_image_replace_fn hook, void *user) MT_NOEXCEPT;

typedef struct mt_cg_image_stats {
    uint64_t offered;  /* bundle images offered to the hook */
    uint64_t replaced; /* replaced */
    uint64_t rejected; /* replacements of the wrong size, ignored */
} mt_cg_image_stats;

/* The counters since the hook was last set. */
mt_cg_image_stats mt_cg_get_image_stats(void) MT_NOEXCEPT;

/* --- UIColor ------------------------------------------------------------------------------ */

/* The components of `color`, a UIColor (+colorWithRed:green:blue:alpha: and the others), each
 * 0 to 1, into rgba[4] (#194: the key pad's haptics read what -[TouchKeyPadView
 * colorAtPixel:onImage:] 0xB4DB5 returns). Returns 1, or 0 (rgba untouched) for nil and
 * anything that is not a UIColor. Guest thread. */
int mt_cg_color_rgba(mt_cpu *cpu, uint32_t color, float rgba[4]) MT_NOEXCEPT;

/* --- PNG files before stb_image (#322, ADR 0158) ---------------------------------------------
 *
 * The chunk walk that every PNG decode runs before the pinned stb_image reads the file (CG's
 * png_decode, the frontend's border image): NULL if the file starts with the PNG signature and
 * each chunk up to IEND lies inside it; otherwise why not. Refused besides: an empty IDAT before
 * the first one with data (stb_image copies its zero bytes through a null pointer). stb_image
 * grows its buffer to a chunk's stated length before it reads the chunk, so a 100-byte file
 * could claim 2 GiB; with this walk no chunk claims more than the file holds. */
const char *mt_cg_png_check(const uint8_t *data, size_t size) MT_NOEXCEPT;

MT_EXTERN_C_END

#endif /* MAPLETHIEF_CG_H */
