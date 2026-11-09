// The player's crash report (#198, ADR 0030's addendum): where a guest trap, a hardware fault or
// a run that ends by itself ends up, and what the next start does about it.
//
//   the file    crashes/<time>.txt in the data folder: the version line, the crash report of
//               ADR 0030 (mt_crash_format: guest registers, the symbolized guest backtrace) and
//               the last 200 log lines (player_log.h). Written by crash_report_write through
//               open/write/close only, from a signal handler too (see below).
//   the dialog  a guest trap: run_game asks for it after the run (request_crash_dialog). A
//               signal or an unhandled exception: the process is dying and may not use SDL, so
//               it starts a second copy of itself, `MapleThief --crash-dialog FILE`, which
//               shows the dialog and exits. Both show SDL_ShowMessageBox with the file's path
//               and an "Open folder" button; on iOS the next start's offer names the file.
//   the marker  crashes/running.marker is set at the start of a game and cleared when it ends
//               without a report. Found at the next start: the last run crashed, and an
//               interactive run offers a safe start (safe_start_settings, never written to the
//               settings file: set_settings_write_blocked).
//
// Async-signal-safety of the path a crash takes: mt_crash_format (snprintf, dladdr, the guest
// memory's lock-trying description) is not safe in the strict sense (ADR 0030); everything of
// crash_report_write is: open/CreateFile, write/WriteFile, close, time, plain arithmetic.
// posix_spawn of the dialog is not on POSIX's safe list, and is a kernel call on macOS and a
// CLONE_VFORK path on Linux. On Windows the exception filter runs on the faulting thread and
// CreateProcess is the same kind of risk. The process is dying either way; the file is written
// before any of that.
//
// Test seams: set_crash_dialog_for_test catches the dialog request, MAPLETHIEF_CRASH_DIALOG_LOG=
// FILE makes it (and the --crash-dialog helper) append "dialog: <path>" to FILE instead of
// showing anything, and crash_report_start's `install_handlers` leaves a test process's signals
// alone.
#ifndef MAPLETHIEF_PLATFORM_CRASH_REPORT_H
#define MAPLETHIEF_PLATFORM_CRASH_REPORT_H

#include <functional>
#include <string>

#include "settings.h"

struct SDL_Window;

namespace maplethief::frontend {

inline constexpr const char *kCrashMarkerName = "running.marker";
inline constexpr const char *kCrashDialogLogEnv = "MAPLETHIEF_CRASH_DIALOG_LOG";

// Remembers `crashes_dir` (data_location().crashes; created), sets the runtime's crash sink and,
// with `install_handlers`, mt_crash_install. "" : no folder, so no file; the handlers still run.
void crash_report_start(const std::string &crashes_dir, bool install_handlers = true);
// The top of every report ("version: ...\n..."), copied. Call when the header is known.
void crash_report_set_header(const std::string &text);
// `show`: the default dialog shows the box (interactive runs); else it only logs.
void set_crash_ui(bool show);

// A report was written in this run.
bool crash_reported();
// The report's path ("" if none).
std::string last_crash_file();

// The guest ended by itself and not through a trap: an exit status, a stop reason (`why`).
void crash_report_guest_ended(const std::string &why);
// The trap and hardware paths come through the runtime's sink; the sink's body, exposed for
// the tests of a report's contents.
void crash_report_write(const char *report, std::size_t len, bool in_signal);

// Asks for the dialog for last_crash_file(), parented to `window` (may be null). No crash
// report: nothing. Honours the seams.
void request_crash_dialog(SDL_Window *window);
using CrashDialogFn = std::function<void(const std::string &path)>;
void set_crash_dialog_for_test(CrashDialogFn fn); // null: the default

// `MapleThief --crash-dialog FILE`: shows it and returns the process's exit status.
int run_crash_dialog_tool(const std::string &path);

// The marker. `begin` returns whether the last run left it (and its report's path: the newest
// .txt in `crashes_dir`, "" if none) and then sets it; `end` clears it. "" as the folder: a
// no-op that reports no crash.
struct LastRun {
    bool crashed = false;
    std::string crash_file;
};
LastRun crash_marker_begin(const std::string &crashes_dir);
void crash_marker_end(const std::string &crashes_dir);

// Safe start: the defaults, with the player's saves folder, backups, language and preset kept
// so that the game still finds their saves and does not ask again: no packs, the software
// renderer, no filter.
Settings safe_start_settings(const Settings &settings);
// The start-up question ("Start with default settings ...") after a crash.
enum class SafeStartChoice { kDefaults, kNormal };
using SafeStartPrompt = std::function<SafeStartChoice(const std::string &crash_file)>;
void set_safe_start_prompt_for_test(SafeStartPrompt prompt); // null: the default
// Whether this start is a safe one: `flag` (--safe-start), or the last run crashed, `interactive`
// and the player chose it.
bool decide_safe_start(bool flag, const LastRun &last, bool interactive);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_CRASH_REPORT_H
