// The save files on the host (#78, ADR 0074, maplethief/saves.h): the Documents directory,
// atomic replacement and the read-time sanitiser. Plain C++ (no exceptions, no standard
// containers, no function-local statics) like the stubs: the recompiler's C test harness links
// it with them (tests/recomp_harness.py).
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

extern "C" void mt_saves_set_fault(mt_saves_fault fault) noexcept { g.fault = fault; }
