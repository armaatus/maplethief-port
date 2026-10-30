// The save files on the host (#78, ADR 0074, maplethief/saves.h): the Documents directory,
// atomic replacement, the read-time sanitiser and the checks (#148, ADR 0078). Plain C++ (no
// exceptions, no standard containers, no function-local statics) like the stubs: the
// recompiler's C test harness links it with them (tests/recomp_harness.py).
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "maplethief/saves.h"
#include "maplethief/stubs.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <io.h> // _commit, _fileno
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

constexpr std::size_t k_path_max = 1024;

struct State {
    char documents_dir[k_path_max] = {}; // mt_saves_set_documents_dir, "" for the default
    char answer[k_path_max] = {};        // what mt_saves_documents_dir returned last
    mt_saves_fault fault = MT_SAVES_FAULT_NONE;
};
State g;

bool is_separator(char c) {
#if defined(_WIN32)
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

#if defined(_WIN32)
// UTF-8 -> UTF-16 into out[0..size); false if it does not fit or is not UTF-8.
bool widen(const char *utf8, wchar_t *out, int size) {
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, out, size) > 0;
}
#endif

// Creates directory `dir` (UTF-8); true if it exists afterwards.
bool make_dir(const char *dir) {
#if defined(_WIN32)
    wchar_t w[k_path_max];
    if (!widen(dir, w, static_cast<int>(k_path_max))) {
        return false;
    }
    if (CreateDirectoryW(w, nullptr) != 0) {
        return true;
    }
    const DWORD attr = GetFileAttributesW(w);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st;
    return mkdir(dir, 0755) == 0 || (stat(dir, &st) == 0 && S_ISDIR(st.st_mode));
#endif
}

// The directory part of `path` into out (no trailing separator); false if there is none.
bool parent_of(const char *path, char *out) {
    std::size_t len = std::strlen(path);
    while (len > 0 && !is_separator(path[len - 1])) {
        len--;
    }
    while (len > 1 && is_separator(path[len - 1])) {
        len--;
    }
    if (len == 0 || len >= k_path_max) {
        return false;
    }
    std::memcpy(out, path, len);
    out[len] = '\0';
    return true;
}

// Creates the parent directories of `path`, like mkdir -p of its directory.
bool make_parent_dirs(const char *path) {
    char dir[k_path_max];
    if (!parent_of(path, dir)) {
        return true; // a bare file name: the current directory
    }
    // Each prefix that ends before a separator, then the whole directory.
    for (std::size_t i = 1; dir[i] != '\0'; i++) {
        if (is_separator(dir[i]) && !is_separator(dir[i - 1])) {
            const char saved = dir[i];
            dir[i] = '\0';
#if defined(_WIN32)
            const bool drive = i == 2 && dir[1] == ':'; // "C:"
#else
            const bool drive = false;
#endif
            if (!drive) {
                make_dir(dir);
            }
            dir[i] = saved;
        }
    }
    return make_dir(dir);
}

std::FILE *open_write(const char *path) {
#if defined(_WIN32)
    wchar_t w[k_path_max];
    std::FILE *f = nullptr;
    return widen(path, w, static_cast<int>(k_path_max)) && _wfopen_s(&f, w, L"wb") == 0 ? f
                                                                                        : nullptr;
#else
    return std::fopen(path, "wb");
#endif
}

// The bytes of `f` reach the disk (not just the OS's cache).
bool sync_file(std::FILE *f) {
    if (std::fflush(f) != 0) {
        return false;
    }
#if defined(_WIN32)
    return _commit(_fileno(f)) == 0;
#elif defined(__APPLE__)
    // fsync only hands the bytes to the drive, which may keep them in its cache; F_FULLFSYNC
    // flushes that too. Not every file system has it: fsync then.
    const int fd = fileno(f);
    return fcntl(fd, F_FULLFSYNC) == 0 || fsync(fd) == 0;
#else
    return fsync(fileno(f)) == 0;
#endif
}

void remove_file(const char *path) {
#if defined(_WIN32)
    wchar_t w[k_path_max];
    if (widen(path, w, static_cast<int>(k_path_max))) {
        DeleteFileW(w);
    }
#else
    std::remove(path);
#endif
}

// Renames `from` over `to`, replacing it in one step.
bool rename_over(const char *from, const char *to) {
#if defined(_WIN32)
    wchar_t wf[k_path_max], wt[k_path_max];
    return widen(from, wf, static_cast<int>(k_path_max)) &&
           widen(to, wt, static_cast<int>(k_path_max)) &&
           MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    if (std::rename(from, to) != 0) {
        return false;
    }
    // The rename itself reaches the disk with the directory's entry.
    char dir[k_path_max];
    if (parent_of(to, dir)) {
        const int fd = open(dir, O_RDONLY);
        if (fd >= 0) {
            fsync(fd);
            close(fd);
        }
    }
    return true;
#endif
}

} // namespace

extern "C" const char *mt_saves_documents_dir(void) noexcept {
    if (g.documents_dir[0] != '\0') {
        std::snprintf(g.answer, k_path_max, "%s", g.documents_dir);
        return g.answer;
    }
    const char *data = mt_stubs_data_dir();
    const std::size_t len = std::strlen(data);
    if (len == 0) {
        g.answer[0] = '\0';
        return g.answer;
    }
#if defined(_WIN32)
    const char *sep = is_separator(data[len - 1]) ? "" : "\\";
#else
    const char *sep = is_separator(data[len - 1]) ? "" : "/";
#endif
    const int n = std::snprintf(g.answer, k_path_max, "%s%sDocuments", data, sep);
    if (n < 0 || static_cast<std::size_t>(n) >= k_path_max) {
        g.answer[0] = '\0';
    }
    return g.answer;
}

extern "C" int mt_saves_set_documents_dir(const char *dir) noexcept {
    if (dir == nullptr || dir[0] == '\0') {
        g.documents_dir[0] = '\0';
        return 0;
    }
    if (std::strlen(dir) >= k_path_max - 64) {
        return -1;
    }
    std::snprintf(g.documents_dir, k_path_max, "%s", dir);
    // No trailing separator: <dir>/<name> is then the one spelling of a save's path.
    std::size_t len = std::strlen(g.documents_dir);
    while (len > 1 && is_separator(g.documents_dir[len - 1])) {
        g.documents_dir[--len] = '\0';
    }
    return 0;
}

extern "C" int mt_saves_replace_file(const char *path, const void *bytes, size_t size) noexcept {
    char tmp[k_path_max];
    if (path == nullptr || path[0] == '\0' || (bytes == nullptr && size != 0)) {
        return -1;
    }
    const int n = std::snprintf(tmp, k_path_max, "%s%s", path, MT_SAVES_TMP_SUFFIX);
    if (n < 0 || static_cast<std::size_t>(n) >= k_path_max) {
        return -1;
    }
    if (!make_parent_dirs(path)) {
        return -1;
    }
    std::FILE *f = open_write(tmp);
    if (f == nullptr) {
        return -1;
    }
    const mt_saves_fault fault = g.fault;
    if (fault == MT_SAVES_FAULT_OPENED) {
        std::fclose(f); // the process is gone: the empty file stays
        return -1;
    }
    if (fault == MT_SAVES_FAULT_HALF_WRITTEN) {
        std::fwrite(bytes, 1, size / 2, f);
        std::fclose(f);
        return -1;
    }
    bool ok = size == 0 || std::fwrite(bytes, 1, size, f) == size;
    ok = sync_file(f) && ok;
    ok = std::fclose(f) == 0 && ok;
    if (fault == MT_SAVES_FAULT_WRITTEN) {
        return -1;
    }
    if (!ok || !rename_over(tmp, path)) {
        remove_file(tmp);
        return -1;
    }
    return 0;
}

extern "C" uint32_t mt_saves_sanitise(const char *name, uint8_t *bytes, size_t size) noexcept {
    if (name == nullptr || bytes == nullptr || std::strcmp(name, "config") != 0) {
        return 0;
    }
    // config: byte 1 indexes ApplyGameSpeed's tick lengths {65, 50, 40} (0x108C58), byte 5
    // the table {0, 1, 3} (0x108C4C), both unchecked (docs/formats/save.md).
    const size_t k_indexes[] = {1, 5};
    uint32_t changed = 0;
    for (const size_t at : k_indexes) {
        if (at < size && bytes[at] > 2) {
            bytes[at] = 2;
            changed++;
        }
    }
    return changed;
}

// --- checking (#148, ADR 0078) ------------------------------------------------------------------
//
// tools/mtport/savecheck.py has the same rules, in the same order, on savedata.py's parsed
// records; CTest saves.check_parity runs both on the same fixtures.

namespace {

constexpr std::size_t k_character_size = 0x464;
constexpr std::size_t k_quest_size = 0x12AD;
constexpr std::size_t k_config_size = 6;
constexpr std::size_t k_cupon_size = 0x174A;
constexpr std::size_t k_terms_size = 2;
constexpr std::uint8_t k_empty = 0xFF;

// The maps of each area: map/<area><map>.map exists for map 0 .. n-1 (docs/formats/map.md).
constexpr std::uint32_t k_maps_in_area[MT_SAVES_AREAS] = {6, 5, 12, 9, 19, 4, 4, 7, 7, 11, 6};

const char *const k_receipt_files[] = {"crestore", "riprestore1", "riprestore2", "totalrestore"};

enum class Kind { kNone, kCharacter, kQuest, kConfig, kCupon, kTerms, kReceipts };

Kind kind_of(const char *name) {
    if (name == nullptr) {
        return Kind::kNone;
    }
    const bool slot =
        (std::strncmp(name, "theif", 5) == 0 || std::strncmp(name, "quest", 5) == 0) &&
        name[5] >= '0' && name[5] <= '2' && name[6] == '\0';
    if (slot) {
        return name[0] == 't' ? Kind::kCharacter : Kind::kQuest;
    }
    if (std::strcmp(name, "config") == 0) {
        return Kind::kConfig;
    }
    if (std::strcmp(name, "cupon") == 0) {
        return Kind::kCupon;
    }
    if (std::strcmp(name, "CTerms") == 0) {
        return Kind::kTerms;
    }
    for (const char *r : k_receipt_files) {
        if (std::strcmp(name, r) == 0) {
            return Kind::kReceipts;
        }
    }
    return Kind::kNone;
}

std::int32_t le16(const std::uint8_t *p) {
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(p[0] | (p[1] << 8)));
}

// Writes the reason for a broken rule into why[0..size) and returns the verdict.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 4, 5)))
#endif
mt_saves_verdict broken(char *why, std::size_t size, mt_saves_verdict verdict, const char *format,
                        ...) {
    if (why != nullptr && size > 0) {
        va_list args;
        va_start(args, format);
        std::vsnprintf(why, size, format, args);
        va_end(args);
    }
    return verdict;
}

// Where the reason goes, passed down the checks.
struct Why {
    char *text;
    std::size_t size;
};
#define MT_BROKEN(w, verdict, ...) broken((w).text, (w).size, (verdict), __VA_ARGS__)

mt_saves_verdict check_size(const char *name, std::size_t size, std::size_t want, const char *what,
                            const Why &w) {
    if (size == want) {
        return MT_SAVES_OK;
    }
    return MT_BROKEN(w, MT_SAVES_BAD_SIZE, "%s has %zu bytes; %s has exactly %zu (%s)", name, size,
                     what, want, size < want ? "it is cut short" : "it is too long");
}

// An equipment record's id at p: empty or an item_equip row.
mt_saves_verdict check_equip(const char *name, const std::uint8_t *p, const char *where,
                             unsigned index, const Why &w) {
    if (p[0] != k_empty && p[0] >= MT_SAVES_EQUIP_IDS) {
        return MT_BROKEN(w, MT_SAVES_BAD_ITEM,
                         "%s: %s slot %u holds equipment %u; the game has equipment 0-%u", name,
                         where, index, p[0], MT_SAVES_EQUIP_IDS - 1);
    }
    return MT_SAVES_OK;
}

mt_saves_verdict check_character(const char *name, const std::uint8_t *b, const Why &w) {
    if (b[0] > MT_SAVES_MAX_BRANCH) {
        return MT_BROKEN(w, MT_SAVES_BAD_JOB, "%s: job branch %u; the game has 0-%u", name, b[0],
                         MT_SAVES_MAX_BRANCH);
    }
    if (b[1] > MT_SAVES_MAX_JOB_TIER) {
        return MT_BROKEN(w, MT_SAVES_BAD_JOB, "%s: job advancement %u; the game has 0-%u", name,
                         b[1], MT_SAVES_MAX_JOB_TIER);
    }
    if (b[2] < 1 || b[2] > MT_SAVES_MAX_LEVEL) {
        return MT_BROKEN(w, MT_SAVES_BAD_LEVEL, "%s: level %u; the game has levels 1-%u", name,
                         b[2], MT_SAVES_MAX_LEVEL);
    }
    for (unsigned i = 0; i < 48; i++) {
        const mt_saves_verdict v =
            check_equip(name, b + 0x015 + 16 * i, "equipment inventory", i, w);
        if (v != MT_SAVES_OK) {
            return v;
        }
    }
    for (unsigned i = 0; i < 48; i++) {
        const std::uint8_t id = b[0x325 + 3 * i];
        if (id != k_empty && id >= MT_SAVES_ETC_IDS) {
            return MT_BROKEN(w, MT_SAVES_BAD_ITEM,
                             "%s: item inventory slot %u holds item %u; the game has items 0-%u",
                             name, i, id, MT_SAVES_ETC_IDS - 1);
        }
    }
    for (unsigned i = 0; i < 13; i++) {
        if (b[0x3B5 + i] > MT_SAVES_MAX_SKILL_LEVEL) {
            return MT_BROKEN(w, MT_SAVES_BAD_SKILL,
                             "%s: skill %u is at level %u; no skill goes past %u", name, i,
                             b[0x3B5 + i], MT_SAVES_MAX_SKILL_LEVEL);
        }
    }
    for (unsigned i = 0; i < 5; i++) {
        const mt_saves_verdict v = check_equip(name, b + 0x3C2 + 16 * i, "worn", i, w);
        if (v != MT_SAVES_OK) {
            return v;
        }
    }
    const std::uint8_t area = b[0x41C];
    const std::uint8_t map = b[0x41D];
    if (area >= MT_SAVES_AREAS) {
        return MT_BROKEN(w, MT_SAVES_BAD_MAP, "%s: area %u; the game has areas 0-%u", name, area,
                         MT_SAVES_AREAS - 1);
    }
    if (map >= k_maps_in_area[area]) {
        return MT_BROKEN(w, MT_SAVES_BAD_MAP, "%s: map %u of area %u; that area has maps 0-%u",
                         name, map, area, k_maps_in_area[area] - 1);
    }
    return MT_SAVES_OK;
}

// The length of the well-formed UTF-8 character at p[0..left) that is not a control character
// (below 0x20, or 0x7F), or 0. Overlong forms, surrogates and code points past U+10FFFF are not
// well formed. The NUL that ends a key is not a continuation byte, so a character it cuts is 0.
std::size_t text_char(const std::uint8_t *p, std::size_t left) {
    const std::uint8_t b = p[0];
    if (b < 0x80) {
        return b >= 0x20 && b != 0x7F ? 1 : 0;
    }
    std::size_t n = 0;
    std::uint8_t lo = 0x80;
    std::uint8_t hi = 0xBF;
    if (b >= 0xC2 && b <= 0xDF) {
        n = 2;
    } else if (b >= 0xE0 && b <= 0xEF) {
        n = 3;
        lo = b == 0xE0 ? 0xA0 : 0x80;
        hi = b == 0xED ? 0x9F : 0xBF;
    } else if (b >= 0xF0 && b <= 0xF4) {
        n = 4;
        lo = b == 0xF0 ? 0x90 : 0x80;
        hi = b == 0xF4 ? 0x8F : 0xBF;
    } else {
        return 0;
    }
    if (n > left || p[1] < lo || p[1] > hi) {
        return 0;
    }
    for (std::size_t k = 2; k < n; k++) {
        if (p[k] < 0x80 || p[k] > 0xBF) {
            return 0;
        }
    }
    return n;
}

// A quest record's text key at p (`width` bytes): UTF-8 text with no control characters, up to
// a NUL inside the field. Scripts.strings keys are ASCII but for one Korean key with Hangul.
mt_saves_verdict check_key(const char *name, const std::uint8_t *p, std::size_t width,
                           unsigned record, const char *what, const Why &w) {
    for (std::size_t i = 0; i < width;) {
        if (p[i] == 0) {
            return MT_SAVES_OK;
        }
        const std::size_t n = text_char(p + i, width - i);
        if (n == 0) {
            return MT_BROKEN(w, MT_SAVES_BAD_QUEST,
                             "%s: quest %u's %s is not a text key (byte 0x%02X at %zu)", name,
                             record, what, p[i], i);
        }
        i += n;
    }
    return MT_BROKEN(w, MT_SAVES_BAD_QUEST, "%s: quest %u's %s has no end within its %zu bytes",
                     name, record, what, width);
}

// A requirement or a reward at p: kind 0-2; for kind 0 (an item) table 0 (item_equip) or 1
// (item_etc) and an id in it.
mt_saves_verdict check_quest_item(const char *name, const std::uint8_t *p, unsigned record,
                                  const char *what, unsigned index, const Why &w) {
    if (p[0] > 2) {
        return MT_BROKEN(w, MT_SAVES_BAD_QUEST,
                         "%s: quest %u's %s %u is of kind %u; the game has 0-2", name, record, what,
                         index, p[0]);
    }
    if (p[0] != 0) {
        return MT_SAVES_OK;
    }
    if (p[1] > 1) {
        return MT_BROKEN(w, MT_SAVES_BAD_QUEST,
                         "%s: quest %u's %s %u names item table %u; the game has 0 (equipment) "
                         "and 1 (items)",
                         name, record, what, index, p[1]);
    }
    const std::uint32_t ids = p[1] == 0 ? MT_SAVES_EQUIP_IDS : MT_SAVES_ETC_IDS;
    if (p[2] >= ids) {
        return MT_BROKEN(w, MT_SAVES_BAD_ITEM, "%s: quest %u's %s %u is %s %u; the game has 0-%u",
                         name, record, what, index, p[1] == 0 ? "equipment" : "item", p[2],
                         ids - 1);
    }
    return MT_SAVES_OK;
}

mt_saves_verdict check_quest(const char *name, const std::uint8_t *b, const Why &w) {
    // QuestSave_Apply 0x83631 stops at the first empty record, and each list at kind 0xFF.
    for (unsigned r = 0; r < 10; r++) {
        const std::uint8_t *q = b + 0xC9 + 0x1C9 * r;
        if (q[0] == k_empty) {
            break;
        }
        mt_saves_verdict v = check_key(name, q + 0x003, 64, r, "title", w);
        if (v == MT_SAVES_OK) {
            v = check_key(name, q + 0x043, 256, r, "description", w);
        }
        for (unsigned k = 0; v == MT_SAVES_OK && k < 5 && q[0x143 + 14 * k] != k_empty; k++) {
            v = check_quest_item(name, q + 0x143 + 14 * k, r, "requirement", k, w);
        }
        for (unsigned k = 0; v == MT_SAVES_OK && k < 5 && q[0x189 + 13 * k] != k_empty; k++) {
            v = check_quest_item(name, q + 0x189 + 13 * k, r, "reward", k, w);
        }
        if (v != MT_SAVES_OK) {
            return v;
        }
    }
    return MT_SAVES_OK;
}

// config: the two bytes the game uses as table indexes (byte 0, sound, is bits: any value).
mt_saves_verdict check_config(const std::uint8_t *b, const Why &w) {
    if (b[1] > 2) {
        return MT_BROKEN(w, MT_SAVES_BAD_CONFIG,
                         "config: game speed is %u; the game has speeds 0-2 (3 or more reads past "
                         "its table)",
                         b[1]);
    }
    if (b[5] > 2) {
        return MT_BROKEN(w, MT_SAVES_BAD_CONFIG,
                         "config: byte 5 is %u; the game has 0-2 (3 or more reads past its table)",
                         b[5]);
    }
    return MT_SAVES_OK;
}

mt_saves_verdict check_receipts(const char *name, const std::uint8_t *b, std::size_t size,
                                const Why &w) {
    if (size > MT_SAVES_MAX_RECEIPTS_BYTES) {
        return MT_BROKEN(w, MT_SAVES_BAD_SIZE, "%s has %zu bytes; a receipt list has at most %u",
                         name, size, MT_SAVES_MAX_RECEIPTS_BYTES);
    }
    if (size < 2) {
        return MT_BROKEN(w, MT_SAVES_BAD_SIZE, "%s has %zu bytes; a receipt list has at least 2",
                         name, size);
    }
    const std::int32_t count = le16(b);
    if (count < 0) {
        return MT_BROKEN(w, MT_SAVES_BAD_RECEIPTS, "%s: %d receipts", name, count);
    }
    std::size_t at = 2;
    for (std::int32_t i = 0; i < count; i++) {
        if (at + 3 > size) {
            return MT_BROKEN(w, MT_SAVES_BAD_RECEIPTS, "%s: receipt %d of %d runs past the end",
                             name, i, count);
        }
        const std::int32_t length = le16(b + at + 1);
        if (length < 0 || at + 3 + static_cast<std::size_t>(length) > size) {
            return MT_BROKEN(w, MT_SAVES_BAD_RECEIPTS,
                             "%s: receipt %d of %d bytes runs past the end", name, i, length);
        }
        at += 3 + static_cast<std::size_t>(length);
    }
    return MT_SAVES_OK; // bytes after the last entry: a shorter rewrite's old tail (save.md)
}

#undef MT_BROKEN

} // namespace

extern "C" int mt_saves_is_game_file(const char *name) noexcept {
    return kind_of(name) != Kind::kNone ? 1 : 0;
}

extern "C" uint32_t mt_saves_maps_in_area(uint32_t area) noexcept {
    return area < MT_SAVES_AREAS ? k_maps_in_area[area] : 0u;
}

extern "C" mt_saves_verdict mt_saves_check(const char *name, const uint8_t *bytes, size_t size,
                                           char *why, size_t why_size) noexcept {
    const Why w{why, why_size};
    if (why != nullptr && why_size > 0) {
        why[0] = '\0';
    }
    const Kind kind = kind_of(name);
    if (kind == Kind::kNone) {
        return broken(why, why_size, MT_SAVES_BAD_NAME, "%s is not a file the game saves",
                      name != nullptr ? name : "(no name)");
    }
    if (bytes == nullptr && size != 0) {
        return broken(why, why_size, MT_SAVES_BAD_SIZE, "%s: no bytes", name);
    }
    mt_saves_verdict v = MT_SAVES_OK;
    switch (kind) {
    case Kind::kCharacter:
        v = check_size(name, size, k_character_size, "a character file", w);
        return v != MT_SAVES_OK ? v : check_character(name, bytes, w);
    case Kind::kQuest:
        v = check_size(name, size, k_quest_size, "a quest file", w);
        return v != MT_SAVES_OK ? v : check_quest(name, bytes, w);
    case Kind::kConfig:
        v = check_size(name, size, k_config_size, "config", w);
        return v != MT_SAVES_OK ? v : check_config(bytes, w);
    case Kind::kCupon:
        return check_size(name, size, k_cupon_size, "cupon", w);
    case Kind::kTerms:
        return check_size(name, size, k_terms_size, "CTerms", w);
    case Kind::kReceipts:
        return check_receipts(name, bytes, size, w);
    case Kind::kNone:
        break;
    }
    return MT_SAVES_BAD_NAME;
}

extern "C" const char *mt_saves_verdict_name(mt_saves_verdict verdict) noexcept {
    switch (verdict) {
    case MT_SAVES_OK:
        return "ok";
    case MT_SAVES_BAD_NAME:
        return "bad_name";
    case MT_SAVES_BAD_SIZE:
        return "bad_size";
    case MT_SAVES_BAD_JOB:
        return "bad_job";
    case MT_SAVES_BAD_LEVEL:
        return "bad_level";
    case MT_SAVES_BAD_ITEM:
        return "bad_item";
    case MT_SAVES_BAD_SKILL:
        return "bad_skill";
    case MT_SAVES_BAD_MAP:
        return "bad_map";
    case MT_SAVES_BAD_QUEST:
        return "bad_quest";
    case MT_SAVES_BAD_CONFIG:
        return "bad_config";
    case MT_SAVES_BAD_RECEIPTS:
        return "bad_receipts";
    }
    return "?";
}

extern "C" void mt_saves_set_fault(mt_saves_fault fault) noexcept { g.fault = fault; }
