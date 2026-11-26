// Crash recovery points (#335, ADR 0151): a rolling save state of the running game, taken at
// safe points, kept beside the game's saves, and offered at the next start after a crash.
//
//   setting   crash_recovery = off | on (Gameplay; off, off, on; off on the Switch, a platform
//             row). Read live by the turn hook.
//   when      in the turn hook, at the first turn where mt_snapshot_safe holds after a trigger:
//             a map load (a new area and map outside the loading modes 6 to 8, AppletWatch's
//             rule), a game save (the slot's pair written: a call of CDataMgr::SaveQuestData
//             0x38F55, seen by a hook wrapper) or kRecoveryIntervalSeconds of play on the guest
//             clock since the last point. Only with a character slot (0-2) in play, and never in
//             the turn of a quick-save or quick-load. A point due while the last one is still
//             being written is skipped; the next trigger takes it.
//   writing   the main thread takes the stored state (MT_SNAPSHOT_STORE, the one to three ticks
//             of ADR 0151's table); a worker thread deflates it (mt_snapshot_deflate: the same
//             file mt_snapshot_save makes), moves recovery-1 to recovery-2 and writes recovery-1
//             through mt_saves_replace_file (ADR 0074). The worker touches only its own buffer.
//   files     <data dir>/states/recovery-1.state (the newest) and recovery-2.state (the one
//             before it). A clean exit waits for the write and deletes both (clean_exit); a run
//             that ends in a trap waits for the write and keeps them (wait).
//   the start find_recovery_point, when #198's marker says the last run crashed: the newest file
//             that reads (the header and every CRC, the port version, a character slot) and is
//             not older than its slot's theif<N> or quest<N>; every other one is deleted. #198's
//             box then asks (crash_report.h: decide_start): Resume, Resume with default
//             settings, Start with default settings, Start normally.
//   resume    an armed point waits for the player to load its character as usual: at the first
//             safe turn in the field with that slot it is restored as quick_load does (the
//             frontend's input let go, mt_snapshot_restore, the clock re-based), its file then
//             deleted (the older one becomes the newest). Another character in the field first:
//             the point is dropped, with a notice. A point of another copy of the game (the
//             binary's hash) is found at that restore's check and dropped the same way.
//   Switch    in applet mode a point is taken only if the heap keeps the margin plus 1.25 times
//             the last point's raw size free (RecoveryOps::heap_free; ADR 0141).
//
// Log lines: "recovery: point N taken at <trigger> (map A.M, slot S): R MB raw in T ms",
// "recovery: point written: D MB in T ms", "recovery: point skipped: <reason>", and at a start
// "recovery: offered|not offered (<reason>)|resumed|dropped (<reason>)".
//
// Threads: everything but the worker's write runs on the turn hook's thread.
#ifndef MAPLETHIEF_PLATFORM_RECOVERY_H
#define MAPLETHIEF_PLATFORM_RECOVERY_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "maplethief/recomp/recomp.h"

namespace maplethief::frontend {

inline constexpr double kRecoveryIntervalSeconds = 300; // five minutes of the guest's time

// <data dir>/states/recovery-<n>.state, n 1 (the newest) or 2.
std::string recovery_path(const std::string &data_dir, int n);

enum class RecoveryTrigger { kNone, kMapLoad, kGameSave, kTimer };
const char *recovery_trigger_name(RecoveryTrigger t); // "a map load", "a game save", "the timer"

// Where the game is (autosave.h's check): -1 for what cannot be read.
struct RecoveryWhere {
    std::int32_t mode = -1, area = -1, map = -1, slot = -1;
};

// The game's side and the files', which the tests replace.
struct RecoveryOps {
    // mt_snapshot_safe: true, or false with the reason.
    std::function<bool(std::string *why)> safe;
    std::function<RecoveryWhere()> where;
    // mt_snapshot_save with MT_SNAPSHOT_STORE for `slot`.
    std::function<bool(std::uint32_t slot, std::vector<std::uint8_t> *out, std::string *why)>
        save_stored;
    // mt_snapshot_deflate. Runs on the worker thread.
    std::function<bool(const std::vector<std::uint8_t> &stored, std::vector<std::uint8_t> *out,
                       std::string *why)>
        deflate;
    // The file, written atomically (mt_saves_replace_file); read whole. `write` runs on the
    // worker thread.
    std::function<bool(const std::string &path, const std::vector<std::uint8_t> &bytes)> write;
    std::function<std::optional<std::vector<std::uint8_t>>(const std::string &path)> read;
    // A point's check before its restore (check_quick_state: the binary, the slot, the
    // version): "" or the player's reason.
    std::function<std::string(const std::vector<std::uint8_t> &bytes, std::uint32_t slot)> check;
    // The frontend's input let go, then mt_snapshot_restore (the clock re-based).
    std::function<bool(const std::vector<std::uint8_t> &bytes, std::uint32_t slot,
                       std::string *why)>
        restore;
    // In the Switch's applet mode: the heap's free bytes now (nullopt, or unset: no limit),
    // and the margin that must stay free beside a point.
    std::function<std::optional<std::uint64_t>()> heap_free;
    std::uint64_t heap_margin = 0;
};

// The point a start may resume.
struct RecoveryOffer {
    bool offered = false;
    std::string path; // the file
    std::int32_t slot = -1;
    // How long before the crash it was taken (the crash report's time, else the marker's), in
    // minutes; negative if not known.
    double minutes_before = -1;
    std::string why; // not offered: the reason (logged)
};

// The point's header as find_recovery_point needs it: "" and the slot, or the reason it is not
// offered. The default reads mt_snapshot_read_header (no binary: that is checked at the
// restore) and compares the port version with MT_VERSION.
using RecoveryHeaderCheck =
    std::function<std::string(const std::vector<std::uint8_t> &bytes, std::int32_t *slot)>;
std::string recovery_header_check(const std::vector<std::uint8_t> &bytes, std::int32_t *slot);

// At a start. `crashed`: #198's marker was found. Without a crash both files are deleted (a
// clean run leaves none). With one, the newest file that `check` passes and that is not older
// than its slot's theif<N> or quest<N> in `saves_dir` is offered; each file that fails is
// deleted and logged. `crash_file`: the crash report ("" if none), for the minutes.
RecoveryOffer find_recovery_point(const std::string &data_dir, const std::string &saves_dir,
                                  bool crashed, const std::string &crash_file,
                                  const RecoveryHeaderCheck &check = recovery_header_check);

// Deletes both files (a clean exit, or a start that discards the point).
void delete_recovery_points(const std::string &data_dir);

// What a turn did, for the window's notice ("" none) and the tests.
struct RecoveryTurn {
    bool taken = false;   // a stored state was taken; its write runs on the worker
    bool resumed = false; // the armed point was restored
    bool dropped = false; // the armed point was dropped
    RecoveryTrigger trigger = RecoveryTrigger::kNone;
    std::string notice;
};

class RecoveryPoints {
  public:
    RecoveryPoints() = default;
    RecoveryPoints(const RecoveryPoints &) = delete;
    RecoveryPoints &operator=(const RecoveryPoints &) = delete;
    ~RecoveryPoints(); // waits for the write

    // Where the files go (<data_dir>/states/).
    void set_data_dir(std::string data_dir) { data_dir_ = std::move(data_dir); }
    // The timer's interval in guest seconds (kRecoveryIntervalSeconds; the real-asset test takes
    // points sooner).
    void set_interval(double seconds) { interval_ = seconds; }

    // A resume chosen at the start: restored at the first safe turn in the field with its slot.
    void arm(const RecoveryOffer &offer);
    bool armed() const { return armed_slot_ >= 0; }

    // One turn of the turn hook. `on`: crash_recovery. `t`: the guest clock in seconds.
    // `game_saves`: the calls of SaveQuestData so far. `busy`: a quick-save or quick-load ran
    // in this turn (no point then).
    RecoveryTurn turn(const RecoveryOps &ops, bool on, double t, std::uint64_t game_saves,
                      bool busy = false);

    // A write in progress finishes (the run ends after a trap; the tests).
    void wait();
    // A clean exit: wait, then both files deleted.
    void clean_exit();
    bool writing() const;

    struct Stats {
        std::uint64_t taken = 0, written = 0, skipped = 0, failed = 0;
        std::size_t last_raw = 0, last_written = 0;
    };
    Stats stats() const;

  private:
    RecoveryTurn resume(const RecoveryOps &ops, const RecoveryWhere &w);
    void write_async(const RecoveryOps &ops, std::vector<std::uint8_t> stored);
    void skip(const std::string &why);

    std::string data_dir_;
    std::int32_t armed_slot_ = -1;
    std::string armed_path_;
    // The triggers.
    RecoveryTrigger pending_ = RecoveryTrigger::kNone;
    std::int32_t area_ = -1, map_ = -1;
    std::uint64_t saves_ = 0;
    bool saves_seen_ = false;
    std::optional<double> since_; // the guest time of the last point (or of the start)
    std::uint64_t points_ = 0;    // taken, this run
    double interval_ = kRecoveryIntervalSeconds;
    // The worker.
    std::thread worker_;
    std::atomic<bool> busy_{false};
    mutable std::mutex mu_;
    Stats stats_;
};

// The calls of CDataMgr::SaveQuestData so far, counted by a hook wrapper (ADR 0078's hook,
// beside the backups' handler). install_game_save_counter registers it once.
void install_game_save_counter();
std::uint64_t game_saves_seen();

// RecoveryOps on the running game, as game_quick_ops (quick_state.h) with the stored save and
// mt_snapshot_deflate.
RecoveryOps game_recovery_ops(mt_cpu *cpu, std::function<void()> release,
                              void (*rebase)(void *user, std::uint64_t guest_unix_us),
                              void *user);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_RECOVERY_H
