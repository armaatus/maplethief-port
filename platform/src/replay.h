// Input replay and checkpoints for the regression run (#91, ADR 0104).
//
// A replay is the input of one run of the game on the virtual clock (ADR 0044: --fixed-step,
// one run-loop turn a step), as a text file of our own data (no game content):
//
//   maplethief-replay 1
//   step_us 1000                     the virtual time a step (a run-loop turn) advances
//   every 1000                       a checkpoint every N steps (named s<step>; 0: none)
//   data empty                       the data directory it starts from (empty: a fresh install)
//   # STEP  EVENT                    steps count turns from the first (0); at 1 ms a step the
//   500 pad attach xbox360           game's 50 ms tick is 50 steps
//   1234 pad button a down           SDL_GamepadButton names (SDL_GetGamepadStringForButton)
//   1334 pad button a up
//   2000 pad axis leftx -32768       SDL_GamepadAxis names, the raw value -32768..32767
//   41000 check first_map            a named checkpoint at the end of that turn
//   90000 end                        the run ends at that turn (after its checkpoint)
//   expect first_map map=2.8 kills>=0    facts the checkpoint's state must show (=, >=, <=)
//
// Blank lines and lines starting with '#' are comments. Events are in step order (the order of
// events in one step is kept). A file is untrusted: every number and name is checked.
//
// A checkpoint (Checkpoint) is what a run shows at the end of a turn: a 64-bit FNV-1a of the
// composited 480x320 screen (frame_hash: what the device's screen shows), the running hash of
// the mixer's output up to then (AudioHash, audio_out.h: what --audio-capture would write), the
// GL frame index and a state line of key=value facts read from the guest. A CheckpointLog is the
// run's list of them; its digest is the FNV-1a of its text, so two runs, two builds or two
// settings compare by one 16-digit hex number, and compare_logs names the first checkpoint that
// differs. CheckpointPlan says which steps take one: the replay's named checks, every N steps,
// or any added by a caller (#190's "run N steps from a restored state, hash every K").
//
// Pure data and logic: no SDL, no guest. The game driver (tests/cpp/game_driver.h) records a
// bot's pad into a Replay and plays one back; scripts/regress compares the logs.
#ifndef MAPLETHIEF_PLATFORM_REPLAY_H
#define MAPLETHIEF_PLATFORM_REPLAY_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace maplethief::frontend {

inline constexpr int kReplayVersion = 1;

enum class ReplayKind {
    kPadAttach, // a virtual pad is attached (name: its kind, "xbox360")
    kPadButton, // index: SDL_GamepadButton, value: 1 down, 0 up
    kPadAxis,   // index: SDL_GamepadAxis, value: the raw axis
    kCheck,     // name: the checkpoint's
    kEnd,       // the run ends
};

struct ReplayEvent {
    std::uint64_t step = 0;
    ReplayKind kind = ReplayKind::kEnd;
    int index = 0;
    int value = 0;
    std::string name;
    bool operator==(const ReplayEvent &o) const {
        return step == o.step && kind == o.kind && index == o.index && value == o.value &&
               name == o.name;
    }
};

// A fact a checkpoint's state must show: `key op value`. Numbers compare as numbers, anything
// else (a map "2.8") only with '='.
struct ReplayExpect {
    std::string check;
    std::string key;
    std::string op; // "=", ">=", "<="
    std::string value;
};

struct Replay {
    std::uint64_t step_us = 1000;
    std::uint64_t every = 0;
    std::string data = "empty";
    std::vector<ReplayEvent> events;
    std::vector<ReplayExpect> expects;

    // The step of the end event (or of the last event if there is none).
    std::uint64_t end_step() const;
    // Appends an event at `step` (not before the last one's: steps only go forward).
    bool add(const ReplayEvent &e);
};

// The names the file uses: SDL's gamepad button and axis names. Return -1 if unknown.
int replay_button_from_name(const std::string &name);
int replay_axis_from_name(const std::string &name);
const char *replay_button_name(int button); // "" if out of range
const char *replay_axis_name(int axis);

// Parses a replay file's text. False with `error` ("line N: ...") on anything malformed: a
// wrong header or version, an unknown event, a name or a number out of range, steps going
// back, a check named twice, an expect on a check that is not there, events after the end.
bool parse_replay(const std::string &text, Replay *out, std::string *error);
// The text of a replay (parse_replay of it gives it back). `comment` lines go after the header.
std::string format_replay(const Replay &r, const std::vector<std::string> &comment = {});

// Walks a replay's input events (pad attach, button, axis) in step order.
class ReplayCursor {
  public:
    explicit ReplayCursor(const Replay *replay) : replay_(replay) {}
    // The next input event due at or before `step`, or nullptr (then the caller's turn is done).
    const ReplayEvent *next(std::uint64_t step);
    bool finished() const;

  private:
    const Replay *replay_;
    std::size_t next_ = 0;
};

// --- checkpoints --------------------------------------------------------------------------------

inline constexpr std::uint64_t kFnvStart = 14695981039346656037ull;
std::uint64_t fnv1a64(const void *data, std::size_t n, std::uint64_t h = kFnvStart);
// The hash of an RGBA8 image (rows top first, w * 4 bytes each): its width and height (as two
// little-endian u32), then its bytes.
std::uint64_t frame_hash(const std::uint8_t *rgba, std::uint32_t w, std::uint32_t h);
std::string hex64(std::uint64_t v); // 16 lower-case hex digits

struct Checkpoint {
    std::string name;
    std::uint64_t step = 0;
    std::uint64_t frame = 0;        // frame_hash of the composited screen
    std::uint64_t audio = 0;        // AudioHash::hash() at the checkpoint
    std::uint64_t audio_frames = 0; // and its frames
    std::uint32_t gl_frame = 0;     // the GL frame shown
    std::string state;              // "key=value key=value ..." (no '|', no newline)
};

// One line: "NAME step N frame HEX audio HEX/FRAMES gl N | STATE".
std::string format_checkpoint(const Checkpoint &c);
bool parse_checkpoint(const std::string &line, Checkpoint *out, std::string *error);

// The facts of a state line ("map=2.8 x=12"): key -> value. False on a malformed word.
bool parse_state(const std::string &state, std::map<std::string, std::string> *out);
// Whether `state` shows `e`; `why` says what it shows instead.
bool expect_holds(const ReplayExpect &e, const std::map<std::string, std::string> &state,
                  std::string *why);

class CheckpointLog {
  public:
    void add(const Checkpoint &c) { points_.push_back(c); }
    const std::vector<Checkpoint> &points() const { return points_; }
    const Checkpoint *find(const std::string &name) const;
    // The log's text: a line a checkpoint, each ending in '\n'.
    std::string text() const;
    // fnv1a64 of text(), as 16 hex digits: the run in one word.
    std::string digest() const;
    // Parses text() back.
    static bool parse(const std::string &text, CheckpointLog *out, std::string *error);

  private:
    std::vector<Checkpoint> points_;
};

// The first difference between two logs: `index` of the first checkpoint that differs (or the
// length of the shorter, if one is a prefix of the other), and both lines ("" past an end).
struct LogDifference {
    bool same = true;
    std::size_t index = 0;
    std::string a, b;
};
LogDifference compare_logs(const CheckpointLog &a, const CheckpointLog &b);

// Which steps take a checkpoint: named ones (a replay's checks, or a caller's), and every
// `every` steps from step `every` on (named s<step>), up to and including `end` (if set).
class CheckpointPlan {
  public:
    CheckpointPlan() = default;
    // The replay's check events, its `every` and its end.
    explicit CheckpointPlan(const Replay &replay);
    void set_every(std::uint64_t every) { every_ = every; }
    void set_end(std::uint64_t end) { end_ = end; }
    // A named checkpoint at `step` (several at one step are taken in the order added).
    void add(std::uint64_t step, const std::string &name);
    // The names of the checkpoints at `step` (empty: none).
    std::vector<std::string> due(std::uint64_t step) const;
    std::size_t named() const { return named_.size(); }

  private:
    std::multimap<std::uint64_t, std::string> named_;
    std::uint64_t every_ = 0;
    std::uint64_t end_ = UINT64_MAX;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_REPLAY_H
