// The run file, .mtrun (#405, ADR 0156, docs/formats/mtrun.md): a run played in real time, as
// its replay needs it. Our own data, deflated; no SDL and no guest in it.
//
//   bound to      the armv7 slice's SHA-256, the port's version, the format version, the
//                 settings at the start and every change at its turn, and the game's own files
//                 the guest reads at boot (its three save slots and its options)
//   holds         the run tape (maplethief/runtape.h: the clocks' values run-length coded, the
//                 host's calls at the guest's boundary at their turn), the splits, a checkpoint
//                 (guest memory digest and state line) every 1,000 game ticks and at the end, and
//                 why the recording ended
//
// The file is untrusted input: read_run_file checks every length against what is left and a
// limit before it reads or allocates, and refuses a malformed file with the byte where it goes
// wrong (in the file, or in the inflated body).
#ifndef MAPLETHIEF_PLATFORM_RUN_FILE_H
#define MAPLETHIEF_PLATFORM_RUN_FILE_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "maplethief/runtape.h"

namespace maplethief::frontend {

// The format version a writer writes and a reader accepts.
inline constexpr std::uint32_t kRunFormat = 1;
// The file's first 8 bytes.
inline constexpr char kRunMagic[8] = {'M', 'T', 'R', 'U', 'N', '\0', '\r', '\n'};
// Limits a reader holds a file to (untrusted input).
inline constexpr std::uint64_t kRunMaxBody = 512ull << 20; // inflated body
inline constexpr std::uint32_t kRunMaxFiles = 16;
inline constexpr std::uint32_t kRunMaxFileBytes = 4u << 20;
inline constexpr std::uint32_t kRunMaxText = 1u << 20; // a string (the settings text)

// A setting changed during the run, at the turn the recorder saw it.
struct RunSettingChange {
    std::uint64_t turn = 0;
    std::string key;
    std::string value;
    bool operator==(const RunSettingChange &) const = default;
};

// A file the guest reads at boot, by its name in the saves folder.
struct RunBootFile {
    std::string name;
    std::vector<std::uint8_t> bytes;
    bool operator==(const RunBootFile &) const = default;
};

// A checkpoint at the end of a turn.
struct RunCheckpoint {
    std::uint64_t turn = 0;
    std::uint64_t tick = 0;   // the game's ticks so far
    std::uint64_t digest = 0; // mt_snapshot_memory_digest
    std::string state;        // the state line (run_state.h)
    bool operator==(const RunCheckpoint &) const = default;
};

// A split (#406 fills them): its name and both times.
struct RunFileSplit {
    std::string name;
    std::uint64_t turn = 0;
    std::uint64_t rta_us = 0; // real time from the run's start
    std::uint64_t ticks = 0;  // game ticks from the run's start
    bool operator==(const RunFileSplit &) const = default;
};

// Why a recording ended.
enum class RunEnd : std::uint32_t {
    kQuit = 0,    // the player quit (or the run ended: --seconds, the window closed)
    kRefused = 1, // something the recorder cannot record became active (detail: its name)
    kFailed = 2,  // the run failed (a guest crash)
};
const char *run_end_name(RunEnd end);

struct RunFile {
    std::array<std::uint8_t, 32> binary_sha256{};
    std::string port_version;
    std::string settings; // serialise_settings at the start
    std::vector<RunSettingChange> changes;
    std::vector<RunBootFile> files;
    std::array<std::vector<mt_runtape_run>, MT_RUNTAPE_STREAMS> streams;
    std::vector<mt_runtape_event> events;
    std::vector<RunFileSplit> splits;
    std::vector<RunCheckpoint> checkpoints;
    RunEnd end = RunEnd::kQuit;
    std::string end_detail;
    std::uint64_t turns = 0; // the turns recorded
    bool operator==(const RunFile &) const;
};

// The file's bytes; empty if deflate failed.
std::vector<std::uint8_t> write_run_file(const RunFile &run);
// Reads `bytes` into *out. False with `why` ("byte 12: ...", "body byte 3,401: ...") for a
// malformed file; *out is then unspecified.
bool read_run_file(const std::uint8_t *bytes, std::size_t size, RunFile *out, std::string *why);

// The run's file name: "<UTC YYYYMMDD-HHMMSS>.mtrun" for Unix seconds `t`.
std::string run_file_name(std::int64_t t);

// The time the guest's clock tape covers, in microseconds (its first to its last value).
std::uint64_t run_tape_span_us(const RunFile &run);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_RUN_FILE_H
