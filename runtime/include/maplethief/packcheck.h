/* Pack checks and the text export (#371, ADR 0205): the tools a player runs on a pack from the
 * game itself, so that what the check says is what the game does.
 *
 * Each kind's check reads a pack folder with its loader's own rules (maplethief/texpack.h,
 * audio.h, textpack.h): the same names, limits and header checks, then the same decode, but
 * into a report instead of the log, and without touching the pack the game has open. Then the
 * manifest, pack.strings (ADR 0149): format, kind, id, name, the lengths, language,
 * game_version and port_min. A pack without pack.strings is fine (a note); a broken one is
 * refused whole, as ADR 0149 says the game refuses it.
 *
 *   texture  every <hash>_<W>x<H>.png / .raw.png: PNG signature, at most 80 MiB, k times the
 *            name's size (k 1 to MT_GFX_MAX_SCALE, at most MT_GFX_MAX_SIZE), decodes; every
 *            ui/ file: a safe path, PNG, decodes, and k times the game's own image when the
 *            game's bundle is given (else its size is not checked: a note); symbolic links and
 *            names past MT_TEXPACK_MAX_FILES are skipped (ignored); other names are ignored.
 *   audio    every .wav / .mp3 / .flac: at most MT_AUDIO_PACK_MAX_FILE bytes, the format, rate,
 *            channels and length, decoded as the game decodes it; an effect's limit when the
 *            game's file of that stem is a WAV, a track's when it is an MP3 (the bundle given),
 *            else a track's (a note); a name the game has no sound for is ignored; every .loop:
 *            "start end".
 *   text     the files the game reads (pack.strings or textpack.strings, Scripts, ScriptExtra,
 *            Tables, Port): size, strict UTF-8 and syntax; Tables keys that name no text
 *            column (ignored) and values longer than their field (cut: a note); Port keys the
 *            port lacks or whose places differ (ignored; mt_packcheck_set_port_text); the
 *            manifest's format, name and language.
 *
 * Lines go to `fn` (may be NULL) as they are found, each with its level. Pack files are
 * untrusted data: parsed within the limits, never executed. Thread-safe with the game's own
 * pack (nothing global is read or written besides the log hook, which is not used). C17, also
 * C++20. */
#ifndef MAPLETHIEF_PACKCHECK_H
#define MAPLETHIEF_PACKCHECK_H

#include <stdint.h>

#include "maplethief/recomp/recomp.h"

MT_EXTERN_C_BEGIN

typedef enum mt_packcheck_level {
    MT_PACKCHECK_NOTE = 0,     /* good to know: nothing is lost */
    MT_PACKCHECK_IGNORED = 1,  /* a name or entry the game passes over */
    MT_PACKCHECK_REJECTED = 2, /* a file the game rejects (the game's own kept), or the pack */
} mt_packcheck_level;

typedef void (*mt_packcheck_fn)(void *user, int level, const char *line);

typedef struct mt_packcheck_report {
    int opens;         /* 1: the game opens the pack; 0: it refuses it whole */
    const char *kind;  /* "texture", "audio", "text"; "" if none could be told */
    uint32_t good;     /* files (text: entries) the game uses */
    uint32_t rejected; /* files the game rejects, the pack refused counted once */
    uint32_t ignored;  /* names and entries it passes over */
    uint32_t notes;
} mt_packcheck_report;

/* The port's own text, for a text pack's Port.strings: the English of `key`, or NULL when the
 * port has no such text. The frontend sets it once (NULL: Port.strings keys are not checked). A
 * key the port lacks is ignored, and so is a translation whose places ({0}, {1}, ...) are not
 * the English's (the port keeps its English then). */
typedef const char *(*mt_packcheck_english_fn)(void *user, const char *key);
void mt_packcheck_set_port_text(mt_packcheck_english_fn fn, void *user) MT_NOEXCEPT;

/* `bundle`: the game's MapleThief.app folder (UTF-8), for the sizes of its images and the kinds
 * of its sounds; NULL or "" when it is not at hand. */
mt_packcheck_report mt_texpack_check(const char *dir, const char *bundle, mt_packcheck_fn fn,
                                     void *user) MT_NOEXCEPT;
mt_packcheck_report mt_audio_pack_check(const char *dir, const char *bundle, mt_packcheck_fn fn,
                                        void *user) MT_NOEXCEPT;
mt_packcheck_report mt_textpack_check(const char *dir, mt_packcheck_fn fn, void *user) MT_NOEXCEPT;

/* The kind of the pack in `dir`: pack.strings' "kind" if it names one, else "text" with a
 * textpack.strings, "texture" with a texture file name or ui/, "audio" with a sound file; ""
 * if none. Then that kind's check (`kind` "" picks one; a report with kind "" and opens 0 when
 * there is nothing to tell). */
const char *mt_pack_kind(const char *dir) MT_NOEXCEPT;
mt_packcheck_report mt_pack_check(const char *dir, const char *kind, const char *bundle,
                                  mt_packcheck_fn fn, void *user) MT_NOEXCEPT;

/* Writes the game's text to translate into `dir` (made if needed): pack.strings (kind text, the
 * language as `language`, game_version 1.5, content derived), Scripts.strings and
 * ScriptExtra.strings (the bundle's <lang>.lproj tables), Tables.strings (the data tables' text
 * columns, each with its room). `language`: "en" or "ko" (English/ or Korean/ data). The
 * frontend adds Port.strings. Returns the entries written, or -1 with the reason to `fn` at
 * MT_PACKCHECK_REJECTED (no bundle, a file that does not parse, a file that cannot be written).
 * The bundle's files are untrusted: property lists and tables are parsed, nothing else. */
int mt_textpack_export(const char *bundle, const char *language, const char *dir,
                       mt_packcheck_fn fn, void *user) MT_NOEXCEPT;

/* A .strings literal of UTF-8 `s` that the parser reads back as `s` (quotes included), into
 * `out` of `size` bytes; returns the length it needs (without the NUL), as snprintf. */
uint32_t mt_textpack_quote(const char *s, char *out, uint32_t size) MT_NOEXCEPT;

MT_EXTERN_C_END

#endif /* MAPLETHIEF_PACKCHECK_H */
