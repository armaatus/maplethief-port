/* Texture packs and texture dumps (#153, ADR 0081): HD textures and sprites that a player makes
 * from their own copy of the game.
 *
 * A dump (--dump-textures DIR) writes every texture the game uploads, once:
 *
 *   DIR/<hash>_<W>x<H>.png       the upload's texels, straight-alpha RGBA (the game's
 *                                premultiplied texels un-premultiplied: c * 255 / a, rounded,
 *                                which ADR 0041's round(c * a / 255) maps back exactly)
 *   DIR/<hash>_<W>x<H>.raw.png   an upload with a colour above its alpha (not premultiplied):
 *                                its texels as stored
 *   DIR/ui/<bundle path>         a UIKit image the shell loads by file name (straight RGBA)
 *   DIR/manifest.jsonl           one JSON object a line per file: hash, file, width, height, GL
 *                                format and type, alpha ("straight" or "raw"); ui, file, width,
 *                                height for a UIKit image
 *
 * <hash> is mt_gfx_upload_hash in 16 lower-case hex digits, W x H the upload's (power-of-two)
 * size. A pack (texture_pack = DIR) has files of the same names, each image k times the size its
 * name says (one integer k from 1 to MT_GFX_MAX_SCALE for both axes, at most MT_GFX_MAX_SIZE),
 * straight alpha (premultiplied at load as the game's loader does it, ADR 0041) or, for .raw.png,
 * as mt_gfx should store it. A pack is indexed when it opens (its file names only) and each file
 * is read when its upload comes. Pack files are untrusted data: only PNGs (the signature is
 * checked) whose header has the right size are decoded (stb_image, PNG only); anything else is
 * rejected with one log line, and the game's texture is kept.
 *
 * Several packs (#421, ADR 0149): mt_texpack_add appends a pack to an ordered list of at most
 * MT_PACKS_MAX. For each upload (by its file name) and each ui/ file the first pack with a file
 * that passes the checks wins; a rejected file falls through to the next pack, then the game.
 * Packs of different k mix: each file is checked against its own upload. Each pack's counts
 * (replaced, rejected, hidden by an earlier pack): mt_texpack_pack_use
 * (maplethief/pack_manifest.h).
 *
 * The hooks: the dump at MT_GFX_REPLACE_ORDER_DUMP (it sees every upload and replaces none), one
 * hook for every pack at MT_GFX_REPLACE_ORDER_PACK (after hd_text's text, maplethief/cg.h), and
 * one UIKit image hook for both (mt_cg_set_image_replace_hook; the dump first, then the packs'
 * ui/ files).
 *
 * Single guest thread (ADR 0033). C17, also C++20. */
#ifndef MAPLETHIEF_TEXPACK_H
#define MAPLETHIEF_TEXPACK_H

#include <stdint.h>

#include "maplethief/cg.h"
#include "maplethief/gfx.h"
#include "maplethief/pack_manifest.h"
#include "maplethief/packcheck.h"

MT_EXTERN_C_BEGIN

typedef struct mt_texpack_stats {
    /* The packs, all together. */
    uint32_t files;       /* GL texture files indexed */
    uint32_t ui_files;    /* ui/ files indexed */
    uint64_t uploads;     /* uploads the packs were asked about (hd_text's are not) */
    uint64_t replaced;    /* replaced by a pack file */
    uint64_t missing;     /* with no file in any pack */
    uint64_t rejected;    /* with files that were all rejected: the game's texture kept */
    uint64_t ui_images;   /* UIKit images the packs were asked about */
    uint64_t ui_replaced; /* replaced by a ui/ file */
    uint64_t ui_rejected; /* with ui/ files that were all rejected */
    /* The dump. */
    uint64_t dumped;      /* texture files written */
    uint64_t dumped_ui;   /* ui/ files written */
    uint64_t dump_errors; /* files that could not be written (logged) */
} mt_texpack_stats;

/* The files a pack indexes at most (GL and ui/ together); the rest are skipped, logged (#413). */
#define MT_TEXPACK_MAX_FILES 65536u

/* Adds the pack in the directory `dir` (UTF-8) after the packs open now: indexes its file names
 * and, for the first pack, installs the hooks. Returns the files indexed (GL and ui/), or -1
 * (logged) if `dir` is no directory or MT_PACKS_MAX packs are open. */
int mt_texpack_add(const char *dir) MT_NOEXCEPT;
/* Closes every pack, then adds `dir`: the pack alone. */
int mt_texpack_open(const char *dir) MT_NOEXCEPT;
/* Closes every pack. */
void mt_texpack_close(void) MT_NOEXCEPT;
/* The packs open now, and what pack `index` (0: the first) did: 0, or -1 if there is none. */
uint32_t mt_texpack_pack_count(void) MT_NOEXCEPT;
int mt_texpack_pack_use(uint32_t index, mt_pack_use *out) MT_NOEXCEPT;

/* Starts a dump into `dir` (UTF-8, made if needed; files already there are not written again)
 * and installs its hooks. Returns 0, or -1 (logged) if the directory or its manifest cannot be
 * made. */
int mt_texpack_dump_open(const char *dir) MT_NOEXCEPT;
void mt_texpack_dump_close(void) MT_NOEXCEPT;
/* Whether a dump is open (the menu turns it on and off, #371). */
int mt_texpack_dump_is_open(void) MT_NOEXCEPT;

/* The counters since the first pack (for the packs') or the dump (for the dump's) opened. */
mt_texpack_stats mt_texpack_get_stats(void) MT_NOEXCEPT;

/* The file name a dump writes and a pack reads for an upload: "<hash>_<W>x<H>.png" (raw 0) or
 * ".raw.png" (raw 1), into `out` (at least MT_TEXPACK_NAME_MAX bytes). */
#define MT_TEXPACK_NAME_MAX 48
void mt_texpack_file_name(uint64_t hash, uint32_t width, uint32_t height, int raw,
                          char *out) MT_NOEXCEPT;

/* Builds a pack from a dump (#371, ADR 0205; what python3 -m mtport.texpack build does, in the
 * game): every file of DUMP/manifest.jsonl, k (1 to MT_GFX_MAX_SCALE) times larger, under its
 * name into `pack` (made if needed; another folder than the dump). The scaler: Scale2x and
 * Scale3x composed for 4x, 6x and 8x (nearest for 5x and 7x), or nearest. `ui` 0 leaves the
 * ui/ images out. Manifest lines it cannot trust (a bad name, a size that disagrees with the
 * name, a ui/ path with "..") are skipped, each a line to `fn` (maplethief/packcheck.h).
 * Returns the files written, or -1 (the reason to `fn`) if it cannot start. */
#define MT_TEXPACK_SCALER_SCALE 0
#define MT_TEXPACK_SCALER_NEAREST 1
int mt_texpack_build(const char *dump, const char *pack, unsigned k, int scaler, int ui,
                     mt_packcheck_fn fn, void *user) MT_NOEXCEPT;

MT_EXTERN_C_END

#endif /* MAPLETHIEF_TEXPACK_H */
