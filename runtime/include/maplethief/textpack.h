/* Text packs (#189, ADR 0108, docs/formats/text-pack.md): a translation of the game's text, and
 * of the port's own, into another language, from a folder of the player's UTF-8 files, without
 * touching the game's files.
 *
 *   DIR/textpack.strings    the manifest (required): format "1", name, language (a tag such as
 *                           "de" or "pt-BR"); version, game_version, author optional
 *   DIR/Scripts.strings     the game's dialogue and quest text, by the keys of the bundle's
 *   DIR/ScriptExtra.strings   <language>.lproj files (-[NSBundle localizedStringForKey:...])
 *   DIR/Tables.strings      the data tables' text: "<table>.<row>.<column>" (item_etc.0.name),
 *                           written into the table's bytes as the game reads the file
 *   DIR/Port.strings        the port's own text (the settings menu, the prompts, ...: the
 *                           frontend asks mt_textpack_lookup("Port", key))
 *
 * Every file is Apple's text .strings syntax ("key" = "value";, comments) in strict UTF-8.
 * Files are untrusted data: at most MT_TEXTPACK_MAX_FILE bytes, keys and values within their
 * limits; a file that breaks a rule is refused whole with one log line naming the line and the
 * reason ("textpack: DIR/Scripts.strings: line 12: invalid UTF-8 (byte 0xC3 ...): refused"),
 * and the game's text is used for everything it would have changed. A key the pack does not
 * have falls back to the game's (or the port's) own text.
 *
 * While a pack is open:
 *   - the game's Korean text encodings (EUC-KR, Mac Korean) read bytes that are valid UTF-8 as
 *     UTF-8 and write UTF-8, so the pack's UTF-8 reaches the screen through the game's
 *     conversions (its table text and its drawing try EUC-KR first; "ö" is C3 B6, a Hangul
 *     syllable in EUC-KR);
 *   - the game's text wrap steps through characters by their UTF-8 length (its own steps 3
 *     bytes for every byte >= 0x80, Hangul's length): two guest hooks, Text_CharCount 0x22989
 *     and Text_SubChars 0x22A1D;
 *   - a table value longer than its field is cut at a whole character and logged, and a text
 *     wider than the texture the game draws it into is logged ("overflow").
 * Without a pack nothing changes.
 *
 * Opened by the frontend before the boot (text_pack = DIR); kept across processes. Single guest
 * thread. C17, also C++20. */
#ifndef MAPLETHIEF_TEXTPACK_H
#define MAPLETHIEF_TEXTPACK_H

#include <stdint.h>

#include "maplethief/recomp/recomp.h"

MT_EXTERN_C_BEGIN

#define MT_TEXTPACK_MAX_FILE (4u << 20) /* bytes of one .strings file */
#define MT_TEXTPACK_MAX_KEY 256u        /* bytes of a key (UTF-8) */
#define MT_TEXTPACK_MAX_VALUE 4096u     /* bytes of a value (UTF-8) */
#define MT_TEXTPACK_MAX_ENTRIES 65536u  /* entries of one file */

typedef struct mt_textpack_stats {
    uint32_t files;     /* .strings files loaded (the manifest included) */
    uint32_t refused;   /* files refused (logged) */
    uint32_t entries;   /* entries loaded from the text files (not the manifest's) */
    uint32_t unknown;   /* Tables.strings keys that name no table text column (logged) */
    uint64_t lookups;   /* strings the game asked the pack for */
    uint64_t replaced;  /* of them, answered from the pack */
    uint32_t fields;    /* table fields written from the pack */
    uint32_t overflows; /* table values cut to their field, texts wider than their texture */
} mt_textpack_stats;

/* Opens the pack in the directory `dir` (UTF-8): reads and checks its files. Returns the entries
 * loaded, or -1 (logged) if `dir` is no directory or its manifest is missing or refused. A pack
 * open before is closed first. */
int mt_textpack_open(const char *dir) MT_NOEXCEPT;
void mt_textpack_close(void) MT_NOEXCEPT;
int mt_textpack_is_open(void) MT_NOEXCEPT;

/* The pack's text for `key` in `table` ("Scripts", "ScriptExtra", "Port"; UTF-8, valid until the
 * pack closes), or NULL: no pack, no such file or key. Counts a lookup. */
const char *mt_textpack_lookup(const char *table, const char *key) MT_NOEXCEPT;

/* A manifest value ("name", "language", "version", ...), or NULL. */
const char *mt_textpack_manifest(const char *key) MT_NOEXCEPT;

/* The counters since the pack opened (zeros without one). */
mt_textpack_stats mt_textpack_get_stats(void) MT_NOEXCEPT;

/* Calls `fn` with each text the game draws into a bitmap (-[NSString drawAtPoint:withFont:],
 * which TextCache_DrawText 0x9031 uses for every name, line and number), as UTF-8, while a pack
 * is open; NULL stops. On the game's thread. For tests: what a pack's text looks like where
 * the game draws it. */
typedef void (*mt_textpack_draw_fn)(const char *utf8, void *user);
void mt_textpack_set_draw_observer(mt_textpack_draw_fn fn, void *user) MT_NOEXCEPT;

MT_EXTERN_C_END

#endif /* MAPLETHIEF_TEXTPACK_H */
