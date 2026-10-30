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

#include <stdint.h>

#include "maplethief/gfx.h"

MT_EXTERN_C_BEGIN

/* The text scale: 1 (off, the default) to MT_GFX_MAX_SCALE; other values are clamped. */
void mt_cg_set_text_scale(uint32_t k) MT_NOEXCEPT;
uint32_t mt_cg_text_scale(void) MT_NOEXCEPT;

/* The replacement hook (see above). `user` is unused. Install it with
 * mt_gfx_set_replace_hook(mt_cg_text_replace, NULL). */
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

MT_EXTERN_C_END

#endif /* MAPLETHIEF_CG_H */
