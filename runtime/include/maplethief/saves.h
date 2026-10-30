/* The save files on the host (#78, ADR 0074): where the game's Documents/ directory lives, how
 * a file there is replaced so that a crash or a kill never leaves half a file, and the checks a
 * save file's bytes get when the game reads them. Foundation's NSFileManager and NSFileHandle
 * (runtime/src/foundation/files.cpp) and GameKit's gamecenter.json go through here; so should
 * every other writer of the player's data (export, import and backups: #148).
 *
 * The game's own files are docs/formats/save.md: theif0-2, quest0-2, config, cupon, CTerms and
 * the receipt lists, raw bytes with no version or checksum, written at offset 0 without
 * truncating. The port keeps those bytes exactly; only the way they reach the disk changes.
 *
 * Plain C++ like the stubs that call it (no exceptions, no standard containers, no
 * function-local statics), so the recompiler's C test harness links it too. C17 that also
 * compiles as C++20. Not thread-safe: the guest's main thread calls it (ADR 0014). */
#ifndef MAPLETHIEF_SAVES_H
#define MAPLETHIEF_SAVES_H

#include <stddef.h>
#include <stdint.h>

#include "maplethief/recomp/recomp.h"

MT_EXTERN_C_BEGIN

/* --- where the saves are ------------------------------------------------------------------ */

/* The host directory the guest's <home>/Documents maps to: mt_saves_set_documents_dir, else
 * <mt_stubs_data_dir()>/Documents:
 *
 *   macOS    ~/Library/Application Support/MapleThief/Documents
 *   Windows  %APPDATA%\MapleThief\Documents
 *   Linux    $XDG_DATA_HOME/MapleThief/Documents (SDL's per-user directory; the stubs' own
 *            default without the frontend is .../maplethief/Documents)
 *   iOS      the app's own Documents/ (the frontend sets it; shared with the Files app)
 *
 * MAPLETHIEF_DATA_DIR moves all of them. Never NULL; "" if there is no data directory. The
 * string stays valid until the next call. */
const char *mt_saves_documents_dir(void) MT_NOEXCEPT;

/* Sets the directory Documents/ maps to (copied); NULL or "" goes back to the default above.
 * Returns 0, or -1 if the path is too long. */
int mt_saves_set_documents_dir(const char *dir) MT_NOEXCEPT;

/* --- writing ------------------------------------------------------------------------------ */

/* Replaces the file at `path` (UTF-8) with `size` bytes, atomically: the bytes go to the
 * temporary file "<path>.mt-tmp" in the same directory, which is flushed to the disk
 * (F_FULLFSYNC on Apple, fsync elsewhere, _commit on Windows) and then renamed over `path`
 * (rename(2); MoveFileExW with MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH on Windows).
 * Whenever the process stops, `path` holds either all of its old bytes or all of the new ones.
 * Creates the parent directories. Returns 0, or -1 with `path` unchanged and the temporary file
 * removed. A "<path>.mt-tmp" left by a killed process is harmless (never read) and is
 * overwritten by the next replacement of `path`. */
int mt_saves_replace_file(const char *path, const void *bytes, size_t size) MT_NOEXCEPT;

/* The suffix of the temporary files: tools that list or copy the saves skip these. */
#define MT_SAVES_TMP_SUFFIX ".mt-tmp"

/* --- reading ------------------------------------------------------------------------------- */

/* Makes the bytes of the save file `name` (a file name in Documents/, e.g. "config") safe for
 * the game to read, in place, before the game sees them. Only values the game cannot handle
 * change; a file the game wrote itself comes through as it is. Returns the number of bytes
 * changed (0 for every other file).
 *
 *   config  byte 1 (game speed) and byte 5 above 2 become 2: the game indexes 3-entry tables
 *           with them unchecked (ApplyGameSpeed 0x37C41: table 0x108C58, the tick length;
 *           0x37B85 and CStateGame::LoadGame 0x86C1D: table 0x108C4C), so 3 gave a tick
 *           length of -1 and larger values read whatever follows the table. */
uint32_t mt_saves_sanitise(const char *name, uint8_t *bytes, size_t size) MT_NOEXCEPT;

/* --- tests ---------------------------------------------------------------------------------- */

/* Where mt_saves_replace_file stops when a fault is set, as a process killed at that moment
 * would: nothing more is written, nothing is cleaned up, and it returns -1. */
typedef enum mt_saves_fault {
    MT_SAVES_FAULT_NONE = 0,
    MT_SAVES_FAULT_OPENED = 1,       /* the temporary file was created, nothing written */
    MT_SAVES_FAULT_HALF_WRITTEN = 2, /* half the bytes are in the temporary file */
    MT_SAVES_FAULT_WRITTEN = 3       /* all of them, flushed, but not yet renamed */
} mt_saves_fault;

/* Sets the fault every later mt_saves_replace_file meets until it is set back to NONE. A test
 * seam: nothing else sets it. */
void mt_saves_set_fault(mt_saves_fault fault) MT_NOEXCEPT;

MT_EXTERN_C_END

#endif /* MAPLETHIEF_SAVES_H */
