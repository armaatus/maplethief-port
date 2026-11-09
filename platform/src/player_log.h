// The player's log file (#198, ADR 0030's addendum): every SDL_Log line, teed by the log output
// function into <data dir>/logs/MapleThief-<time>.log (UTF-8) as well as to stderr, so that a
// player who started the game from the Finder has something to attach to an issue.
//
//   player_log_open     opens the file, keeps the newest kLogFilesKept of them. Lines are held
//                       in memory until the header is known.
//   player_log_header   writes the header (version, commit, OS, GPU backend, settings, IPA hash
//                       check), then the lines so far; from then on each line is flushed as it
//                       is written, so a crash loses nothing.
//   player_log_tail     the last kLogTailLines lines, for the crash report (crash_report.h);
//                       lock-free, callable from a signal handler.
//
// What the file may contain: players post it in public issues. Before a line is stored its
// paths are redacted (the data folder, the game folder, the home folder, a named audio device:
// set_log_redactions, add_log_redaction), and the settings' values that are paths or device
// names print as "<set>" (log_settings_text). Nothing from the save files is ever logged.
#ifndef MAPLETHIEF_PLATFORM_PLAYER_LOG_H
#define MAPLETHIEF_PLATFORM_PLAYER_LOG_H

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "settings.h"

namespace maplethief::frontend {

inline constexpr std::size_t kLogFilesKept = 5;
inline constexpr std::size_t kLogTailLines = 200;
inline constexpr std::size_t kLogTailLineBytes = 480; // a longer line is cut in the tail only
inline constexpr const char *kLogFilePrefix = "MapleThief-";
inline constexpr const char *kLogFileSuffix = ".log";

struct LogHeader {
    std::string version;   // MT_VERSION
    std::string commit;    // MT_COMMIT
    std::string os;        // host_os_text()
    std::string gpu;       // "software (renderer metal)", ...
    std::string settings;  // log_settings_text(...)
    std::string ipa_check; // bundle_hash_check(...)
};
// The header's text, one "key: value" a line, ending in a blank line.
std::string format_log_header(const LogHeader &header);

// "20261109-143012" of `t`, in UTC (the same on every machine, so the files sort by time).
// `out` holds 16 bytes. Async-signal-safe: plain arithmetic, for the crash report.
void log_stamp(std::time_t t, char *out);

// Deletes the oldest log files in `dir` so that `keep` remain, the newest by name. Touches only
// files named MapleThief-*.log. Returns the number deleted.
std::size_t rotate_logs(const std::filesystem::path &dir, std::size_t keep);

// Literal text replaced in every line and in the header, longest first.
using Redactions = std::vector<std::pair<std::string, std::string>>;
void set_log_redactions(Redactions redactions);
// One more (an audio device's name). Thread-safe.
void add_log_redaction(std::string text, std::string replacement);
// `text` with the redactions applied (tests).
std::string redact_log_text(std::string text, const Redactions &redactions);

// The preset, then every setting as "key=value" in registry order, ", " between; the paths and the
// audio device as <set> when they are not empty.
std::string log_settings_text(const Settings &settings);

// "macOS (Darwin 25.6.0 arm64)", "Windows 10.0.26100 x64", "Linux (6.8.0 x86_64)": no host name.
std::string host_os_text();

// MT_COMMIT (the short commit of the configure step, "unknown" outside a git checkout).
std::string player_log_commit();

// "ok" or a sentence: the bundle's executable (<bundle>/MapleThief) against the 1.5 IPA's
// SHA-256 (ipa_import.h), or "not checked (--slice)".
std::string bundle_hash_check(const std::string &bundle_dir, bool custom_slice);

// Installs the log output function (chaining to the previous one) and opens a new file in
// `logs_dir`, which is created ("" or a failure: no file, the tail still works). Returns the
// file's path, "" if none.
std::string player_log_open(const std::string &logs_dir);
// See the top of this file. Only the first call writes.
void player_log_header(const LogHeader &header);
// The tee's other half: one line into the tail and the file. The log output function calls it.
void player_log_line(const char *message);
// Writes a header if none was (a run that ended before one was known), flushes and closes the
// file, removes the log output function.
void player_log_close();
std::string player_log_path();

// Fills `lines` with the tail's lines, oldest first; returns how many (at most kLogTailLines).
// Each is NUL-terminated, without its newline. Lock-free and async-signal-safe (a line being
// written at that moment may be torn, not unterminated).
std::size_t player_log_tail(const char **lines, std::size_t max);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_PLAYER_LOG_H
