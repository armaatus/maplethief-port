// The player's log file (player_log.h, #198).
#include "player_log.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string_view>
#include <system_error>

#include "ipa_import.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/utsname.h>
#endif

#ifndef MT_COMMIT
#define MT_COMMIT "unknown"
#endif

namespace maplethief::frontend {

namespace fs = std::filesystem;

namespace {

fs::path to_path(const std::string &s) { return fs::path(std::u8string(s.begin(), s.end())); }

// --- the process's log ----------------------------------------------------------------------

struct Tail {
    char lines[kLogTailLines][kLogTailLineBytes];
    std::atomic<std::uint32_t> next{0};
};
Tail g_tail;

std::mutex g_mutex; // the file, the pending lines, the redactions
std::ofstream g_file;
std::string g_path;
std::vector<std::string> g_pending; // until the header is written
bool g_header_written = false;
Redactions g_redactions;
SDL_LogOutputFunction g_previous = nullptr;
void *g_previous_user = nullptr;

// The caller holds g_mutex.
std::string redacted(std::string_view message) {
    return redact_log_text(std::string(message), g_redactions);
}

void store_tail(const std::string &line) {
    const std::uint32_t n = g_tail.next.load(std::memory_order_relaxed);
    char *slot = g_tail.lines[n % kLogTailLines];
    const std::size_t len = std::min(line.size(), kLogTailLineBytes - 1u);
    std::memcpy(slot, line.data(), len);
    slot[len] = '\0';
    g_tail.next.store(n + 1u, std::memory_order_release);
}

void SDLCALL tee(void *, int category, SDL_LogPriority priority, const char *message) {
    if (g_previous != nullptr) {
        g_previous(g_previous_user, category, priority, message);
    }
    player_log_line(message);
}

} // namespace

// --- pure functions --------------------------------------------------------------------------

std::string format_log_header(const LogHeader &h) {
    std::string out;
    out += "MapleThief " + h.version + "\n";
    out += "commit: " + h.commit + "\n";
    out += "os: " + h.os + "\n";
    out += "gpu: " + h.gpu + "\n";
    out += "settings: " + h.settings + "\n";
    out += "ipa check: " + h.ipa_check + "\n\n";
    return out;
}

void log_stamp(std::time_t t, char *out) {
    // UTC days to a civil date (H. Hinnant's days_from_civil, inverted).
    std::int64_t secs = static_cast<std::int64_t>(t);
    std::int64_t days = secs / 86400;
    std::int64_t rem = secs % 86400;
    if (rem < 0) {
        rem += 86400;
        days -= 1;
    }
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const auto doe = static_cast<std::uint32_t>(days - era * 146097);
    const std::uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    std::int64_t year = static_cast<std::int64_t>(yoe) + era * 400;
    const std::uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::uint32_t mp = (5 * doy + 2) / 153;
    const std::uint32_t day = doy - (153 * mp + 2) / 5 + 1;
    const std::uint32_t month = mp < 10 ? mp + 3 : mp - 9;
    if (month <= 2) {
        year += 1;
    }
    const auto put2 = [](char *p, std::uint32_t v) {
        p[0] = static_cast<char>('0' + v / 10);
        p[1] = static_cast<char>('0' + v % 10);
    };
    put2(out, static_cast<std::uint32_t>(year / 100));
    put2(out + 2, static_cast<std::uint32_t>(year % 100));
    put2(out + 4, month);
    put2(out + 6, day);
    out[8] = '-';
    const auto s = static_cast<std::uint32_t>(rem);
    put2(out + 9, s / 3600);
    put2(out + 11, s / 60 % 60);
    put2(out + 13, s % 60);
    out[15] = '\0';
}

std::size_t rotate_logs(const fs::path &dir, std::size_t keep) {
    std::vector<fs::path> files;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const auto u8 = it->path().filename().u8string();
        const std::string name(u8.begin(), u8.end());
        if (name.starts_with(kLogFilePrefix) && name.ends_with(kLogFileSuffix)) {
            files.push_back(it->path());
        }
    }
    std::sort(files.begin(), files.end());
    std::size_t removed = 0;
    while (files.size() > keep) {
        std::error_code remove_ec;
        if (fs::remove(files.front(), remove_ec)) {
            removed++;
        }
        files.erase(files.begin());
    }
    return removed;
}

std::string redact_log_text(std::string text, const Redactions &redactions) {
    for (const auto &[from, to] : redactions) {
        if (from.empty()) {
            continue;
        }
        std::size_t at = 0;
        while ((at = text.find(from, at)) != std::string::npos) {
            text.replace(at, from.size(), to);
            at += to.size();
        }
    }
    return text;
}

std::string log_settings_text(const Settings &settings) {
    std::string out =
        std::string("preset=") + (settings.preset ? preset_name(*settings.preset) : "none");
    for (const SettingInfo &row : settings_registry()) {
        static constexpr std::string_view kPrivate[] = {
            "save_dir", "audio_device", "audio_pack", "texture_pack", "text_pack", "border_image"};
        const std::string_view key = row.key;
        std::string value = setting_text(settings, row);
        if (!value.empty() && value != "default" &&
            std::find(std::begin(kPrivate), std::end(kPrivate), key) != std::end(kPrivate)) {
            value = "<set>";
        }
        out += (out.empty() ? "" : ", ") + std::string(key) + "=" + value;
    }
    return out;
}

std::string host_os_text() {
#if defined(_WIN32)
    using RtlGetVersionFn = LONG(WINAPI *)(OSVERSIONINFOW *);
    OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof v;
    if (const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"); ntdll != nullptr) {
        auto *get = reinterpret_cast<RtlGetVersionFn>(
            reinterpret_cast<void *>(GetProcAddress(ntdll, "RtlGetVersion")));
        if (get != nullptr) {
            get(&v);
        }
    }
    char text[64];
    std::snprintf(text, sizeof text, "Windows %lu.%lu.%lu x64", v.dwMajorVersion, v.dwMinorVersion,
                  v.dwBuildNumber);
    return text;
#else
    utsname u{};
    if (uname(&u) != 0) {
        return SDL_GetPlatform();
    }
    return std::string(SDL_GetPlatform()) + " (" + u.sysname + " " + u.release + " " + u.machine +
           ")";
#endif
}

std::string player_log_commit() { return MT_COMMIT; }

std::string bundle_hash_check(const std::string &bundle_dir, bool custom_slice) {
    if (custom_slice) {
        return "not checked (--slice)";
    }
    std::error_code ec;
    const fs::path exe = to_path(bundle_dir) / "MapleThief";
    const std::uintmax_t size = fs::file_size(exe, ec);
    if (ec || size > kMaxIpaEntryBytes) {
        return "not checked (the executable cannot be read)";
    }
    std::ifstream in(exe, std::ios::binary);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!in) {
        return "not checked (the executable cannot be read)";
    }
    const std::string hex = sha256_hex(sha256_of(bytes));
    return hex == kExecutableSha256 ? "ok (the executable is 1.5's)"
                                    : "DIFFERENT: the executable's SHA-256 is " + hex;
}

// --- the process's log ----------------------------------------------------------------------

void set_log_redactions(Redactions redactions) {
    // Longest first, so that a folder inside another is replaced by its own text.
    std::stable_sort(redactions.begin(), redactions.end(),
                     [](const auto &a, const auto &b) { return a.first.size() > b.first.size(); });
    const std::lock_guard lock(g_mutex);
    g_redactions = std::move(redactions);
}

void add_log_redaction(std::string text, std::string replacement) {
    if (text.empty()) {
        return;
    }
    const std::lock_guard lock(g_mutex);
    g_redactions.emplace_back(std::move(text), std::move(replacement));
    std::stable_sort(g_redactions.begin(), g_redactions.end(),
                     [](const auto &a, const auto &b) { return a.first.size() > b.first.size(); });
}

std::string player_log_open(const std::string &logs_dir) {
    const std::lock_guard lock(g_mutex);
    if (g_previous == nullptr) {
        SDL_GetLogOutputFunction(&g_previous, &g_previous_user);
        SDL_SetLogOutputFunction(tee, nullptr);
    }
    g_path.clear();
    g_header_written = false;
    g_pending.clear();
    if (logs_dir.empty()) {
        return g_path;
    }
    const fs::path dir = to_path(logs_dir);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        return g_path;
    }
    // A name for this second; another run in the same second takes the next.
    char stamp[16];
    std::time_t t = std::time(nullptr);
    for (int tries = 0; tries < 60; tries++, t++) {
        log_stamp(t, stamp);
        const fs::path file = dir / (std::string(kLogFilePrefix) + stamp + kLogFileSuffix);
        if (fs::exists(file, ec)) {
            continue;
        }
        g_file.open(file, std::ios::binary | std::ios::trunc);
        if (g_file) {
            const auto u8 = file.u8string();
            g_path.assign(u8.begin(), u8.end());
            rotate_logs(dir, kLogFilesKept);
        }
        break;
    }
    return g_path;
}

void player_log_header(const LogHeader &header) {
    const std::lock_guard lock(g_mutex);
    if (g_header_written) {
        return;
    }
    g_header_written = true;
    if (!g_file.is_open()) {
        return;
    }
    g_file << redacted(format_log_header(header));
    for (const std::string &line : g_pending) {
        g_file << line << '\n';
    }
    g_pending.clear();
    g_file.flush();
}

void player_log_line(const char *message) {
    const std::lock_guard lock(g_mutex);
    std::string line = redacted(message != nullptr ? message : "");
    store_tail(line);
    if (!g_file.is_open()) {
        return;
    }
    if (!g_header_written) {
        g_pending.push_back(std::move(line));
        return;
    }
    g_file << line << '\n';
    g_file.flush();
}

void player_log_close() {
    bool needs_header = false;
    {
        const std::lock_guard lock(g_mutex);
        needs_header = g_file.is_open() && !g_header_written;
    }
    if (needs_header) {
        LogHeader h;
        h.version = MT_VERSION;
        h.commit = MT_COMMIT;
        h.os = host_os_text();
        h.gpu = "not known (the run ended before the window)";
        h.settings = "not known";
        h.ipa_check = "not checked";
        player_log_header(h);
    }
    SDL_LogOutputFunction previous = nullptr;
    void *previous_user = nullptr;
    {
        const std::lock_guard lock(g_mutex);
        if (g_file.is_open()) {
            g_file.close();
        }
        previous = g_previous;
        previous_user = g_previous_user;
        g_previous = nullptr;
        g_previous_user = nullptr;
    }
    SDL_SetLogOutputFunction(previous != nullptr ? previous : SDL_GetDefaultLogOutputFunction(),
                             previous_user);
}

std::string player_log_path() {
    const std::lock_guard lock(g_mutex);
    return g_path;
}

std::size_t player_log_tail(const char **lines, std::size_t max) {
    const std::uint32_t n = g_tail.next.load(std::memory_order_acquire);
    const std::size_t count = std::min<std::size_t>({n, kLogTailLines, max});
    for (std::size_t i = 0; i < count; i++) {
        lines[i] = g_tail.lines[(n - count + i) % kLogTailLines];
    }
    return count;
}

} // namespace maplethief::frontend
