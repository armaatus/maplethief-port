/* The pack manifest (#370, ADR 0149): what a texture, audio or text pack says about itself, and
 * whether the port takes it, read before the pack opens and by the settings menu's packs screen.
 *
 *   DIR/pack.strings      the manifest, in a text pack's .strings syntax (ADR 0108: strict
 *                         UTF-8, the same limits, refused whole on a broken rule):
 *     format        required: "1"; a larger number needs a newer port
 *     kind          required: texture | audio | text; a pack read as another kind is refused
 *     id            required: 1 to 64 of [a-z0-9._-], starting with a letter or digit
 *     name          required: at most 64 characters
 *     version       at most 32 characters; author at most 128; description at most 1,024
 *     language      a text pack's: a tag such as "de" or "pt-BR" (required there)
 *     game_version  "1.5" if given: the game the port runs
 *     port_min      X.Y.Z if given: the oldest port it works with
 *     content       own | derived (shown; the runtime does not act on it); license: as is
 *   Unknown keys are ignored.
 *
 * Without pack.strings: a texture or audio pack takes its folder's name as its name and id; a
 * text pack reads textpack.strings (format, name, language; version, author, game_version) with
 * kind text and its folder's name as its id. With both, pack.strings counts.
 *
 * Packs are untrusted data: the manifest is parsed, never executed; a symbolic link is not
 * followed. C17, also C++20. */
#ifndef MAPLETHIEF_PACK_MANIFEST_H
#define MAPLETHIEF_PACK_MANIFEST_H

#include <stddef.h>
#include <stdint.h>

#include "maplethief/recomp/recomp.h"

MT_EXTERN_C_BEGIN

#define MT_PACK_MANIFEST_FORMAT 1     /* the newest manifest format this port reads */
#define MT_PACK_MANIFEST_GAME "1.5"   /* the game version the port runs */
#define MT_PACK_MAX_NAME 64u          /* characters */
#define MT_PACK_MAX_VERSION 32u       /* characters */
#define MT_PACK_MAX_AUTHOR 128u       /* characters */
#define MT_PACK_MAX_DESCRIPTION 1024u /* characters */
#define MT_PACK_MAX_ID 64u            /* bytes of [a-z0-9._-] */

/* Each field is NUL-terminated UTF-8, "" when the manifest has none. */
typedef struct mt_pack_manifest {
    char kind[8];                              /* "texture", "audio" or "text" */
    char id[MT_PACK_MAX_ID + 1];               /* the pack's identity across versions */
    char name[MT_PACK_MAX_NAME * 4 + 1];       /* what the packs screen shows */
    char version[MT_PACK_MAX_VERSION * 4 + 1]; /* the pack's own version, as written */
    char author[MT_PACK_MAX_AUTHOR * 4 + 1];   /* as written */
    char description[MT_PACK_MAX_DESCRIPTION * 4 + 1];
    char language[36];     /* a text pack's language tag */
    char game_version[33]; /* as written */
    char port_min[33];     /* as written */
    char content[16];      /* "own", "derived" or as written */
    char license[65];      /* as written */
    char source[24];       /* the file read: "pack.strings", "textpack.strings", or "" (none) */
    char refused[512];     /* why the port does not take the pack; "" when it does */
} mt_pack_manifest;

/* Reads the manifest of the pack in directory `dir` (UTF-8) as a pack of `kind` ("texture",
 * "audio" or "text") for the port `port_version` (X.Y.Z or X.Y.Z-rc.N; NULL or "": port_min is
 * not checked). Fills `out` and returns 1 if the port takes the pack, 0 if it refuses it
 * (out->refused says why, in a short English sentence such as "pack format 2 needs a newer
 * port" or "pack.strings: line 3: invalid UTF-8 ..."). The name and id fall back to the
 * folder's name whenever the manifest gives none, so a refused pack still has a name to show. */
int mt_pack_manifest_read(const char *dir, const char *kind, const char *port_version,
                          mt_pack_manifest *out) MT_NOEXCEPT;

/* The kind the pack.strings in `bytes` (`n` bytes) names (#422: a folder or zip added to the
 * packs screen goes where its kind's packs go): the bytes through the same strict parser, then
 * its "kind" copied into `kind` ("texture", "audio" or "text"). Returns 1, or 0 with `kind` ""
 * when the bytes are refused or name none of the three. */
int mt_pack_manifest_kind(const uint8_t *bytes, size_t n, char kind[8]) MT_NOEXCEPT;

/* Compares two versions X.Y.Z[-rc.N]: negative, zero or positive as a is older than, as old as or
 * newer than b (a release candidate is older than its release). If either is not a version: 0,
 * and *ok = 0 (when `ok` is not NULL; else *ok = 1). */
int mt_pack_compare_versions(const char *a, const char *b, int *ok) MT_NOEXCEPT;

/* Several packs of a kind at once (#421, ADR 0149): each kind's runtime (maplethief/texpack.h,
 * audio.h, textpack.h) keeps an ordered list of at most MT_PACKS_MAX packs; the first pack that
 * has an entry, with a file that passes the kind's checks, wins it. What each pack did since it
 * opened, read with mt_texpack_pack_use, mt_audio_pack_use and mt_textpack_pack_use:
 *   replaced   entries it answered (uploads or UIKit images; sounds; strings and table fields)
 *   rejected   entries it had a file or text for that was rejected (the next pack was asked)
 *   hidden     entries it had that an earlier pack answered, and by which: hidden_by[i] counts
 *              those pack i answered (hidden is their sum) */
#define MT_PACKS_MAX 16u

typedef struct mt_pack_use {
    const char *dir; /* the pack's folder as opened (UTF-8; valid until its kind's packs close) */
    uint64_t replaced;
    uint64_t rejected;
    uint64_t hidden;
    uint64_t hidden_by[MT_PACKS_MAX];
} mt_pack_use;

MT_EXTERN_C_END

#endif /* MAPLETHIEF_PACK_MANIFEST_H */
