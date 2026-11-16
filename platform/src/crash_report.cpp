// The player's crash report (crash_report.h, #198).
#include "crash_report.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>
#include <vector>

#include "file_dialog.h"
#include "maplethief/diag.h"
#include "player_log.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#if defined(__SWITCH__)
#include <switch.h>
#else
#include <spawn.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
extern char **environ;
#endif

namespace maplethief::frontend {

namespace fs = std::filesystem;

namespace {

fs::path to_path(const std::string &s) { return fs::path(std::u8string(s.begin(), s.end())); }

// Everything the crash path needs, ready before the crash.
constexpr std::size_t kPathMax = 1024;
char g_dir[kPathMax];       // the crashes folder (UTF-8; empty: none)
char g_path[kPathMax + 32]; // the report written
char g_header[4096];
std::size_t g_header_len = 0;
std::atomic<bool> g_reported{false};
bool g_spawn = false; // a hardware fault starts the helper
#if defined(_WIN32)
wchar_t g_wdir[kPathMax];
wchar_t g_wpath[kPathMax + 32];
wchar_t g_wexe[kPathMax];
wchar_t g_wcmd[kPathMax * 2 + 64];
#else
char g_exe[kPathMax];
#endif

bool g_show_ui = false;
CrashDialogFn g_dialog_for_test;
SafeStartPrompt g_prompt_for_test;

// --- the crash path: only what a signal handler may do ---------------------------------------

#if defined(_WIN32)
using File = HANDLE;
const File kNoFile = INVALID_HANDLE_VALUE;
void put(File f, const char *s, std::size_t n) {
    DWORD done = 0;
    WriteFile(f, s, static_cast<DWORD>(n), &done, nullptr);
}
void close_file(File f) { CloseHandle(f); }
#else
using File = int;
const File kNoFile = -1;
void put(File f, const char *s, std::size_t n) {
    while (n > 0u) {
        const ssize_t w = ::write(f, s, n);
        if (w <= 0) {
            return;
        }
        s += w;
        n -= static_cast<std::size_t>(w);
    }
}
void close_file(File f) { ::close(f); }
#endif

void puts_file(File f, const char *s) { put(f, s, std::strlen(s)); }

// Opens crashes/<stamp>.txt for the current time (or the next free second); kNoFile if the
// folder cannot take it. The name lands in g_path.
File open_report() {
    std::time_t t = std::time(nullptr);
    for (int tries = 0; tries < 60; tries++, t++) {
        char stamp[16];
        log_stamp(t, stamp);
#if defined(_WIN32)
        std::size_t n = 0;
        for (; g_wdir[n] != L'\0' && n < kPathMax - 1u; n++) {
            g_wpath[n] = g_wdir[n];
        }
        g_wpath[n++] = L'\\';
        for (const char *p = stamp; *p != '\0'; p++) {
            g_wpath[n++] = static_cast<wchar_t>(*p);
        }
        for (const wchar_t *p = L".txt"; *p != L'\0'; p++) {
            g_wpath[n++] = *p;
        }
        g_wpath[n] = L'\0';
        const HANDLE h = CreateFileW(g_wpath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            // The narrow path too, for the report and the log.
            const int m = WideCharToMultiByte(CP_UTF8, 0, g_wpath, -1, g_path,
                                              static_cast<int>(sizeof g_path), nullptr, nullptr);
            if (m <= 0) {
                g_path[0] = '\0';
            }
            return h;
        }
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) {
            return kNoFile;
        }
#else
        std::size_t n = std::strlen(g_dir);
        // NOLINTNEXTLINE(bugprone-not-null-terminated-result): ".txt" below ends the path
        std::memcpy(g_path, g_dir, n);
        g_path[n++] = '/';
        std::memcpy(g_path + n, stamp, 15);
        n += 15;
        // NOLINTNEXTLINE(bugprone-not-null-terminated-result): the 5 bytes include ".txt"'s NUL
        std::memcpy(g_path + n, ".txt", 5);
        const int fd = ::open(g_path, O_WRONLY | O_CREAT | O_EXCL | O_TRUNC, 0644);
        if (fd >= 0) {
            return fd;
        }
#endif
    }
    return kNoFile;
}

// Starts `<this program> --crash-dialog <file>`. Not on iOS.
void spawn_dialog_helper() {
#if defined(_WIN32)
    std::size_t n = 0;
    const auto add = [&n](const wchar_t *s) {
        for (; *s != L'\0' && n + 1u < sizeof g_wcmd / sizeof g_wcmd[0]; s++) {
            g_wcmd[n++] = *s;
        }
    };
    add(L"\"");
    add(g_wexe);
    add(L"\" --crash-dialog \"");
    add(g_wpath);
    add(L"\"");
    g_wcmd[n] = L'\0';
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(g_wexe, g_wcmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
#elif defined(__SWITCH__)
    // Horizon starts no second process, and libnx's exception handler runs in ordinary thread
    // context (runtime/src/diag/crash_horizon.cpp): the system's error applet shows the file's
    // path from this process, and the handler then exits to the Homebrew Menu.
    char message[kPathMax + 96];
    std::snprintf(message, sizeof message, "MapleThief crashed. The report is %s", g_path);
    ErrorApplicationConfig c;
    if (R_SUCCEEDED(errorApplicationCreate(&c, message, nullptr))) {
        errorApplicationShow(&c);
    }
#elif !defined(SDL_PLATFORM_IOS)
    char *argv[] = {g_exe, const_cast<char *>("--crash-dialog"), g_path, nullptr};
    pid_t pid = 0;
    posix_spawn(&pid, g_exe, nullptr, nullptr, argv, environ);
#endif
}

void find_own_path() {
#if defined(_WIN32)
    GetModuleFileNameW(nullptr, g_wexe, static_cast<DWORD>(sizeof g_wexe / sizeof g_wexe[0]));
#elif defined(__APPLE__)
    std::uint32_t size = sizeof g_exe;
    if (_NSGetExecutablePath(g_exe, &size) != 0) {
        g_exe[0] = '\0';
    }
#elif defined(__SWITCH__)
    g_exe[0] = '\0'; // no helper process on Horizon (spawn_dialog_helper shows it in-process)
#else
    const ssize_t n = ::readlink("/proc/self/exe", g_exe, sizeof g_exe - 1u);
    g_exe[n > 0 ? n : 0] = '\0';
#endif
}

} // namespace

void crash_report_write(const char *report, std::size_t len, bool in_signal) {
    File f = kNoFile;
    if (g_dir[0] != '\0') {
        f = open_report();
    }
    if (f != kNoFile) {
        puts_file(f, "MapleThief crash report\n");
        put(f, g_header, g_header_len);
        puts_file(f, "\n");
        put(f, report, len);
        puts_file(f, "\n--- the last log lines ---\n");
        const char *lines[kLogTailLines];
        const std::size_t n = player_log_tail(lines, kLogTailLines);
        for (std::size_t i = 0; i < n; i++) {
            puts_file(f, lines[i]);
            puts_file(f, "\n");
        }
        close_file(f);
    }
    g_reported.store(true);
    if (in_signal && f != kNoFile && g_spawn) {
        spawn_dialog_helper();
    }
}

namespace {

void crash_sink(const char *report, std::size_t len, int in_signal, void *) {
    crash_report_write(report, len, in_signal != 0);
}

} // namespace

// --- set-up -----------------------------------------------------------------------------------

void crash_report_start(const std::string &crashes_dir, bool install_handlers) {
    std::error_code ec;
    g_dir[0] = '\0';
    g_reported.store(false);
    g_path[0] = '\0';
    if (!crashes_dir.empty() && crashes_dir.size() < kPathMax) {
        fs::create_directories(to_path(crashes_dir), ec);
        std::memcpy(g_dir, crashes_dir.data(), crashes_dir.size());
        g_dir[crashes_dir.size()] = '\0';
#if defined(_WIN32)
        if (MultiByteToWideChar(CP_UTF8, 0, g_dir, -1, g_wdir,
                                static_cast<int>(sizeof g_wdir / sizeof g_wdir[0])) <= 0) {
            g_dir[0] = '\0';
        }
#endif
    }
    find_own_path();
    g_spawn = install_handlers;
    mt_crash_set_sink(crash_sink, nullptr);
    if (install_handlers) {
        mt_crash_install();
    }
}

void crash_report_set_header(const std::string &text) {
    g_header_len = std::min(text.size(), sizeof g_header - 1u);
    std::memcpy(g_header, text.data(), g_header_len);
}

void set_crash_ui(bool show) { g_show_ui = show; }

bool crash_reported() { return g_reported.load(); }

std::string last_crash_file() { return crash_reported() ? std::string(g_path) : std::string(); }

void crash_report_guest_ended(const std::string &why) {
    const std::string report = "=== the game ended by itself: " + why + " ===\n";
    crash_report_write(report.data(), report.size(), false);
}

// --- the dialog -------------------------------------------------------------------------------

namespace {

void show_dialog(const std::string &path, SDL_Window *window) {
    const fs::path folder = to_path(path).parent_path();
    const std::string text = "MapleThief crashed.\n\nA report was written to:\n" + path +
                             "\n\nPlease attach it and the newest file in logs/ to a bug report.";
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Open folder"},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Close"},
    };
    SDL_MessageBoxData box{};
    box.flags = SDL_MESSAGEBOX_ERROR;
    box.window = window;
    box.title = "MapleThief crashed";
    box.message = text.c_str();
    box.numbuttons = 2;
    box.buttons = buttons;
    int pressed = 0;
    if (!SDL_ShowMessageBox(&box, &pressed)) {
        SDL_Log("crash: cannot show the dialog: %s", SDL_GetError());
        return;
    }
    if (pressed == 1) {
        const auto u8 = folder.u8string();
        std::string why;
        if (!FileDialogs::sdl_url_opener(file_url(std::string(u8.begin(), u8.end())), &why)) {
            SDL_Log("crash: cannot open the folder: %s", why.c_str());
        }
    }
}

// The seams, then the box if this run may show one.
void dialog_for(const std::string &path, SDL_Window *window) {
    if (g_dialog_for_test) {
        g_dialog_for_test(path);
        return;
    }
    if (const char *log = SDL_getenv(kCrashDialogLogEnv); log != nullptr && log[0] != '\0') {
        std::ofstream out(to_path(log), std::ios::app | std::ios::binary);
        out << "dialog: " << path << '\n';
        return;
    }
    if (g_show_ui) {
        show_dialog(path, window);
        return;
    }
    SDL_Log("crash: report written to %s (no dialog in a scripted run)", path.c_str());
}

} // namespace

void request_crash_dialog(SDL_Window *window) {
    if (const std::string path = last_crash_file(); !path.empty()) {
        dialog_for(path, window);
    }
}

void set_crash_dialog_for_test(CrashDialogFn fn) { g_dialog_for_test = std::move(fn); }

int run_crash_dialog_tool(const std::string &path) {
    g_show_ui = true;
    dialog_for(path, nullptr);
    return 0;
}

// --- the marker and the safe start -------------------------------------------------------------

LastRun crash_marker_begin(const std::string &crashes_dir) {
    LastRun last;
    if (crashes_dir.empty()) {
        return last;
    }
    const fs::path dir = to_path(crashes_dir);
    std::error_code ec;
    last.crashed = fs::exists(dir / kCrashMarkerName, ec);
    if (last.crashed) {
        fs::path newest;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->path().extension() == ".txt" &&
                (newest.empty() || newest.filename() < it->path().filename())) {
                newest = it->path();
            }
        }
        const auto u8 = newest.u8string();
        last.crash_file.assign(u8.begin(), u8.end());
    }
    fs::create_directories(dir, ec);
    std::ofstream(dir / kCrashMarkerName, std::ios::binary | std::ios::trunc)
        << "a MapleThief run is going (or crashed). This file is removed on a clean exit.\n";
    return last;
}

void crash_marker_end(const std::string &crashes_dir) {
    if (!crashes_dir.empty()) {
        std::error_code ec;
        fs::remove(to_path(crashes_dir) / kCrashMarkerName, ec);
    }
}

Settings safe_start_settings(const Settings &settings) {
    Settings safe;
    safe.preset = settings.preset;
    safe.save_dir = settings.save_dir;
    safe.save_backups = settings.save_backups;
    safe.language = settings.language;
    return safe;
}

namespace {

SafeStartChoice ask_safe_start(const std::string &crash_file) {
    std::string text = "MapleThief did not close cleanly last time.";
    if (!crash_file.empty()) {
        text += "\n\nThe report is at:\n" + crash_file;
    }
    text += "\n\nStart with default settings (no texture or audio packs, software renderer)?\nYour "
            "settings file is not changed.";
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Start with default settings"},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Start normally"},
    };
    SDL_MessageBoxData box{};
    box.flags = SDL_MESSAGEBOX_WARNING;
    box.title = "MapleThief";
    box.message = text.c_str();
    box.numbuttons = 2;
    box.buttons = buttons;
    int pressed = 0;
    if (!SDL_ShowMessageBox(&box, &pressed)) {
        SDL_Log("safe start: cannot ask: %s", SDL_GetError());
        return SafeStartChoice::kNormal;
    }
    return pressed == 1 ? SafeStartChoice::kDefaults : SafeStartChoice::kNormal;
}

} // namespace

void set_safe_start_prompt_for_test(SafeStartPrompt prompt) {
    g_prompt_for_test = std::move(prompt);
}

bool decide_safe_start(bool flag, const LastRun &last, bool interactive) {
    if (flag) {
        return true;
    }
    if (!last.crashed || (!interactive && !g_prompt_for_test)) {
        return false;
    }
    const SafeStartChoice choice =
        g_prompt_for_test ? g_prompt_for_test(last.crash_file) : ask_safe_start(last.crash_file);
    return choice == SafeStartChoice::kDefaults;
}

} // namespace maplethief::frontend
