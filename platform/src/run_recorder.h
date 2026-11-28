// Recording a run and playing it back (#405, ADR 0156): the frontend's side of the run tape
// (maplethief/runtape.h) and the run file (run_file.h).
//
// Recording (record_runs = on): start() before the process loads wraps the clocks installed for
// the run; turn_end() at the end of every turn hook notes the settings that changed, counts the
// game's ticks, takes a checkpoint every kRunCheckpointTicks and checks the census's refusals
// (run_census.h); finish() at the end writes <data dir>/runs/<UTC time>.mtrun.
//
// Splits (#408): split() at each split the live timer reaches; the recorder keeps its name and
// both times at that turn and takes a checkpoint at the turn's end. category() keeps each run
// category the session timed (its text), so a player's own category travels with the file.
//
// Playing (MapleThief --play-run FILE, --verify-run FILE): play() makes the tape the clock and the
// only input; turn_begin() applies the settings changed at this turn, turn_end() takes a
// checkpoint at every turn the file has one and compares it, and done() says when the recorded
// turns are over. A replay that reaches a checkpoint's tick (every kRunCheckpointTicks) or a split
// at a turn the file has no checkpoint for differs there too. Its own splits are kept
// (replayed_splits) for the verifier to compare with the file's.
//
// A refusal (run_refuse, from the census's refused sites): the recording ends at that turn, its
// file saying why; it replays up to there.
#ifndef MAPLETHIEF_PLATFORM_RUN_RECORDER_H
#define MAPLETHIEF_PLATFORM_RUN_RECORDER_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "maplethief/recomp/recomp.h"
#include "run_file.h"
#include "settings.h"

namespace maplethief::frontend {

// A checkpoint every this many game ticks (and one at the end).
inline constexpr std::uint64_t kRunCheckpointTicks = 1000;

// The game's ticks, seen through CMTimer's lastFrame (0x11A4D8), which the game sets before
// every SkyCanvas::Tick (docs/re/timing-threading.md): a turn runs the 1 ms timer at most once,
// so it ticked when lastFrame changed.
class TickCounter {
  public:
    // Reads lastFrame; true if the game ticked since the last call.
    bool update(const mt_cpu *cpu);
    std::uint64_t ticks() const { return ticks_; }

  private:
    std::uint64_t ticks_ = 0;
    std::int64_t last_ = 0;
    bool seen_ = false;
};

// The state line of a checkpoint: the game mode, the map, the key pad's panel and the ticks.
std::string run_state_line(const mt_cpu *cpu, std::uint64_t ticks);

// The game's own files in `saves_dir` that the guest reads (mt_saves_is_game_file, and the
// local Game Center's gamecenter.json), by name.
std::vector<RunBootFile> read_boot_files(const std::string &saves_dir);

// --play-run's scratch data folder: reads the run file at `path` into *run, makes a new folder
// under the system's temporary one with the file's game files in its saves/ and its run
// categories in its categories/ (#408), and returns it; ""
// with `why` (the file does not read, is not a run file, or the folder cannot be made).
std::string prepare_run_folder(const std::string &path, RunFile *run, std::string *why);
// Why a run file cannot play on this binary and port: "" if it can.
std::string run_binding_mismatch(const RunFile &run, const std::array<std::uint8_t, 32> &sha);

class RunRecorder {
  public:
    enum class Mode { kOff, kRecording, kPlaying };

    // Recording, before the process loads: wraps the clocks (false if GCD refused: logged).
    bool start(const std::array<std::uint8_t, 32> &binary_sha256, const Settings &settings,
               const std::string &saves_dir);
    // Playing, before the process loads. False (with `why`) if the tape cannot play.
    bool play(const RunFile &run, std::string *why);
    Mode mode() const { return mode_; }

    // Playing: the settings changed at this turn, applied to `settings`; returns the rows.
    std::vector<const SettingInfo *> turn_begin(Settings *settings);
    // The end of a turn hook's turn: checkpoints, setting changes and refusals (recording);
    // checkpoints compared (playing).
    void turn_end(const mt_cpu *cpu, const Settings &settings);
    // Recording: ends the tape (if `refused`: why) and writes the file into `runs_dir`;
    // returns its path, or "" (logged). Playing: ends the tape.
    std::string finish(RunEnd end, const std::string &detail, const std::string &runs_dir,
                       const mt_cpu *cpu);

    // A split the live timer reached in this turn (its name, its time since the run's start):
    // recorded with a checkpoint at the turn's end (recording), or kept (playing).
    void split(const std::string &name, std::uint64_t rta_us, std::uint64_t ticks);
    // Recording: a run category the timer times (its id and file text), kept once.
    void category(const std::string &id, const std::string &text);
    // Playing: the splits the replay's timer reached, as split() was told them.
    const std::vector<RunFileSplit> &replayed_splits() const { return replayed_splits_; }

    // Playing: the recorded turns are over.
    bool done() const;
    // Playing: checkpoints compared, and the first that differed ("" if none).
    std::uint64_t compared() const { return compared_; }
    const std::string &first_difference() const { return first_difference_; }
    // Playing: the checkpoints the file has but the replay did not reach.
    std::uint64_t missed() const;
    const RunFile &file() const { return run_; }

    // A refused site became active (run_census.h): the recording ends with the last complete turn,
    // before the refused site touches the guest.
    void refuse(const char *what);

  private:
    RunCheckpoint take_checkpoint(const mt_cpu *cpu) const;
    // Recording: a checkpoint at the end of this turn (one a turn).
    void checkpoint(const mt_cpu *cpu);
    // Playing: this turn's checkpoint compared with the file's; `due` if the replay's own
    // triggers (a tick checkpoint, a split) asked for one.
    void compare(const mt_cpu *cpu, bool due);
    void differ(const std::string &what);
    // Recording: the tape so far into run_, and the tape stopped.
    void seal();

    Mode mode_ = Mode::kOff;
    RunFile run_;
    Settings last_; // recording: the settings at the last turn's end
    TickCounter ticks_;
    std::uint64_t next_checkpoint_ = kRunCheckpointTicks;
    std::size_t next_change_ = 0; // playing
    std::size_t next_compare_ = 0;
    std::uint64_t compared_ = 0;
    std::string first_difference_;
    std::string refused_;
    std::vector<RunFileSplit> replayed_splits_;
    bool split_due_ = false;          // a split in this turn: a checkpoint at its end
    std::uint64_t complete_turn_ = 0; // the last turn whose turn_end ran
    bool sealed_ = false;
};

// The recorder of this run (nullptr when none): the census's refused sites call run_refuse.
void set_run_recorder(RunRecorder *recorder);
void run_refuse(const char *what);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_RUN_RECORDER_H
