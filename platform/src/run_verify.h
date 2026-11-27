// Verifying a shared run file (#408, ADR 0156): MapleThief --verify-run FILE. No SDL and no guest
// in this file: main.cpp and game.cpp gather what the replay did into a RunVerification, and this
// file decides and words the verdict.
//
// A file is refused before it plays when it does not read (run_file.h: a flipped byte fails the
// packed body's CRC-32), when it was recorded on another binary or port version
// (run_binding_mismatch) or when a checkpoint is missing (run_file_problem: one every
// kRunCheckpointTicks game ticks and one at every split). Then it replays (RunRecorder), and it
// verifies when every recorded turn played, every checkpoint is the same, the guest did not crash
// and the replay's live timer reached the file's splits at the same turns with the same times. The
// times are recomputed from the tape: the timer reads the guest's clock as the tape gives it. The
// clean-run status is the replay's own timer's (RunRecord), from the recorded settings and every
// change: never read from the file.
//
// A verified file proves that this input on this binary and port version gives this run with these
// times. It does not prove a human played it in real time: a hand-made file verifies too.
#ifndef MAPLETHIEF_PLATFORM_RUN_VERIFY_H
#define MAPLETHIEF_PLATFORM_RUN_VERIFY_H

#include <cstdint>
#include <string>
#include <vector>

#include "run_file.h"
#include "run_timer.h"

namespace maplethief::frontend {

// The missing checkpoint (or checkpoints out of turn order) of a file that read: "" if none.
std::string run_file_problem(const RunFile &run);

struct RunVerification {
    std::string path;    // the file as given
    std::string refused; // why it was refused before playing; "" if it played
    // The file (when it read).
    std::string port_version;
    std::uint64_t turns = 0;
    std::uint64_t events = 0;
    std::uint64_t checkpoints = 0;
    std::vector<RunFileCategory> categories;
    std::vector<RunFileSplit> recorded;
    // The replay.
    bool reached = false; // every recorded turn played
    bool guest_ok = true; // the guest did not crash or stop
    std::uint64_t compared = 0;
    std::uint64_t missed = 0; // the file's checkpoints the replay did not reach
    std::string first_difference;
    std::vector<RunFileSplit> replayed;
    std::vector<RunRecord> runs; // the replay's timer's finished and reset runs
};

// Whether the file verifies.
bool run_verified(const RunVerification &v);
// The first reason it does not ("" if it verifies).
std::string run_unverified_reason(const RunVerification &v);
// The report --verify-run prints (docs/play/speedrun.md#verifying-a-run): the file, each run's
// category, splits with both times recorded and replayed, clean or assisted with the reason, the
// run variables, the checkpoints and the verdict.
std::string run_verify_report(const RunVerification &v);
// "1:23.456" (minutes, seconds, milliseconds), "1:02:03.456" past an hour, from microseconds.
std::string format_run_time_us(std::uint64_t us);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_RUN_VERIFY_H
