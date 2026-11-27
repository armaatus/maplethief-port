// The speedrun timer (#406, ADR 0156): a run, its splits, both of its times, whether it was clean
// and the personal best of its category. No SDL: game.cpp feeds it the run events and reads its
// overlay lines; the tests feed it synthetic events and a fake clock.
//
//   run_timer = on | off     settings.ini (off by default, outside the presets), --run-timer
//   run_category = NAME      the category it times (any by default), --run-category
//
// A category (docs/formats/run-category.md) is our own data file: a name, the values its run
// variables may take, and a list of splits, each a run event of maplethief/events.h with its
// values. The port ships any.mtcat (Any%: a new character to mob 69's death) and chapter_1.mtcat
// (a new character to the trip to Orbis), embedded in the binary from platform/categories/; a
// player's own files go in <data dir>/categories/ (one of the same name replaces the shipped one).
//
// The run.
//   - It starts at MT_GAME_EVENT_NEW_CHARACTER (a new character in any slot), and again at the
//     next one: a run in progress then ends as reset and a new one starts.
//   - Its splits fire in the category's order: only the next split's event counts (a later
//     split's event before it does nothing). The last split ends the run (finished).
//   - Each split records two times since the start: real time (the host's monotonic clock, what
//     the overlay shows) and game ticks (mt_input_get_stats's tick count). Both are the clock's
//     (RunClock), so a test gives a fake one.
//   - reset() ends a run in progress as reset: run_timer turned off, another category, the game
//     quitting, the next new character.
//   - A finished or reset run becomes a RunRecord; take_records() hands them to the caller,
//     which writes each one to <data dir>/runs/ (write_run).
//
// A clean run (ADR 0156, ADR 0070). Every row of settings_registry() in SettingGroup::kAssists
// must hold its neutral value (its Original column) from the start to the end. check() reads
// every such row at the start, every turn (turn()) and at the last split; one that differs marks
// the run assisted for good, even if it goes back, and the record names it. The rows are found
// by their group, so a new Assist is covered with no change here.
//
// Run variables: fix_original_bugs and skip_intros. Allowed, recorded (every value seen during
// the run) and shown. A category names the values it allows (`allow` lines); a variable it does
// not name may only be off (its Original column). A run whose variable takes another value is
// outside the category: it is recorded and shown, never a personal best.
//
// The personal best of a category is its fastest finished run that is clean and inside the
// category, by real time; the overlay compares the last split reached with the best's same
// split. write_run keeps it beside the logs (runs/<category>.best, the same text as a log).
//
// Single thread: the frontend's. on_event is called from a game-event listener, on the guest
// thread inside the guest's call (maplethief/events.h), which is the frontend's in the turn hook.
#ifndef MAPLETHIEF_PLATFORM_RUN_TIMER_H
#define MAPLETHIEF_PLATFORM_RUN_TIMER_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "maplethief/events.h"
#include "settings.h"

namespace maplethief::frontend {

// --- Categories ---------------------------------------------------------------------------------

inline constexpr const char *kRunCategoryExtension = ".mtcat";
inline constexpr std::size_t kMaxRunCategoryFileBytes = 16 * 1024;
inline constexpr std::size_t kMaxRunSplits = 64;
inline constexpr std::size_t kMaxRunNameBytes = 32; // a category's or a split's title
// The run variables (ADR 0156): allowed in a run, recorded and shown.
inline constexpr const char *kRunVariables[] = {"fix_original_bugs", "skip_intros"};

// One split: a run event of `kind` whose values are `a` (the event's value) and, for a job
// advance and a map change, `b` (more[0]: the tier, the map).
struct RunSplit {
    mt_game_event_kind kind = MT_GAME_EVENT_CHAPTER_END;
    std::int32_t a = 0;
    std::int32_t b = 0;
    std::string title; // the overlay's name of it

    bool matches(const mt_game_event &e) const;
    bool operator==(const RunSplit &) const = default;
};

// "chapter_end", "job_advance", "boss_killed", "map_change"; nullptr for another kind.
const char *run_split_kind_name(mt_game_event_kind kind);
// "chapter_end 1", "job_advance 2 2", "boss_killed 64", "map_change 4.1": the split's event as a
// category file writes it.
std::string run_split_event_text(const RunSplit &s);

struct RunVariableRule {
    std::string key;                 // one of kRunVariables
    std::vector<std::string> values; // the values allowed, as the settings file writes them

    bool operator==(const RunVariableRule &) const = default;
};

struct RunCategory {
    std::string id;    // the file's name without .mtcat: run_category's value
    std::string title; // `title = ...`: what the overlay shows
    std::vector<RunVariableRule> allow;
    std::vector<RunSplit> splits;

    // The values `key` may take: its `allow` line's, else its Original column only.
    std::vector<std::string> allowed(std::string_view key) const;
    bool operator==(const RunCategory &) const = default;
};

// Parses a category file's text (the format in docs/formats/run-category.md) as category `id`.
// Nothing on any problem: `problems` then has one message per bad line ("line 4: ..."), or one
// for the whole file (too large, no title, no split).
std::optional<RunCategory> parse_run_category(std::string_view id, std::string_view text,
                                              std::vector<std::string> *problems);
// The category file's text of `c` (parse_run_category reads it back as `c`).
std::string run_category_text(const RunCategory &c);

// The shipped categories' files, as embedded at build time: {name, text}, in name order.
std::vector<std::pair<std::string, std::string>> shipped_run_category_files();
// The shipped categories and those of <data_dir>/categories/*.mtcat (when data_dir is not
// empty), by name; a player's file replaces a shipped one of the same name. A file that does not
// parse is left out with its problems in `problems` ("categories/x.mtcat: line 3: ...").
std::vector<RunCategory> load_run_categories(const std::string &data_dir,
                                             std::vector<std::string> *problems);

// --- Runs ----------------------------------------------------------------------------------------

// A reading of the run's two clocks.
struct RunInstant {
    double seconds = 0;      // real time: the host's monotonic clock, in seconds
    std::uint64_t ticks = 0; // the game's ticks so far

    bool operator==(const RunInstant &) const = default;
};

// A time of a run: since its start.
struct RunTime {
    std::int64_t ms = 0;     // real time, whole milliseconds
    std::uint64_t ticks = 0; // game ticks

    bool operator==(const RunTime &) const = default;
};

// A finished or reset run, as its log holds it.
struct RunRecord {
    std::string category;       // the category's id
    std::string category_title; // its title
    std::string started;        // the wall-clock time it started ("2026-11-26 21:03:04")
    bool finished = false;      // false: reset before its last split
    // The category's splits (their events and titles), and the times of those reached.
    std::vector<RunSplit> splits;
    std::vector<RunTime> times;
    RunTime end;                      // the last split's time, or when it was reset
    std::vector<std::string> assists; // the Assists that left their neutral value, in order
    // Every run variable with every value it had during the run, in order ("off", "off,on").
    std::vector<std::pair<std::string, std::string>> variables;
    bool in_category = true; // every run variable's values allowed by the category
    std::string settings;    // every setting at the start, "key = value; key = value"

    bool clean() const { return assists.empty(); }
    // A personal best can be made of it: finished, clean and inside the category.
    bool counts() const { return finished && clean() && in_category; }
    bool operator==(const RunRecord &) const = default;
};

// A run log's text (docs/formats/run-category.md#the-run-log), and the record read back from
// one. Nothing for a malformed log (`problems` says why, by line).
std::string run_log_text(const RunRecord &r);
std::optional<RunRecord> parse_run_log(std::string_view text, std::vector<std::string> *problems);

// "12:34.5", "1:02:03.4" (tenths), "-0:01.2" with `sign` (a difference: '+' or '-' always).
std::string format_run_time(std::int64_t ms, bool sign = false);

// The run log's file name: "<category>-<started, digits only>.log" ("any-20261126-210304.log").
std::string run_log_name(const RunRecord &r);
// Writes `r` to <data_dir>/runs/<run_log_name> (another name with "-2", "-3", ... if that one
// exists), and, when it counts and beats `best` (or there is none), to runs/<category>.best.
// Returns false with `why` if a file could not be written. `log_path` gets the log's path,
// `new_best` whether the best was replaced.
bool write_run(const std::string &data_dir, const RunRecord &r, const RunRecord *best,
               std::string *log_path, bool *new_best, std::string *why);
// The personal best of `category` in <data_dir>/runs/, if there is one and it reads.
std::optional<RunRecord> load_run_best(const std::string &data_dir, std::string_view category);

// The run's clocks: what now reads, and the wall-clock stamp a record's `started` gets.
struct RunClock {
    std::function<RunInstant()> now;
    std::function<std::string()> stamp;
};

// What a run did, for a listener that mirrors it (the LiveSplit link, #407): a run started, the
// next split was reached (its time), or a run ended as reset or a finished run was cleared.
enum class RunSignal { kStart, kSplit, kReset };
using RunListener = std::function<void(RunSignal, const RunTime &)>;

class RunTimer {
  public:
    enum class State { kWaiting, kRunning, kFinished };

    explicit RunTimer(RunClock clock) : clock_(std::move(clock)) {}

    // The settings the clean check and the run variables read (game.cpp's g.settings); none:
    // a default Settings.
    void attach(const Settings *settings) { settings_ = settings; }
    // The category to time, and its personal best (none: no comparison). A run in progress ends
    // as reset; the timer then waits for the next start.
    void set_category(RunCategory category, std::optional<RunRecord> best);
    const RunCategory &category() const { return category_; }
    const std::optional<RunRecord> &best() const { return best_; }
    // A new personal best made by this timer's last run (write_run said so).
    void set_best(RunRecord best) { best_ = std::move(best); }
    // Told of every start, split and reset (none: nobody). A start after a finished run is a
    // reset, then the start.
    void set_listener(RunListener listener) { listener_ = std::move(listener); }

    // A game event (the listener's body): a new character starts a run, the next split's event
    // splits.
    void on_event(const mt_game_event &e);
    // The clean check and the run variables, every turn while a run is in progress.
    void turn();
    // Starts a run now (a new character; a test or practice from a save). A run in progress
    // ends as reset first.
    void start();
    // Ends a run in progress as reset (nothing if none is). A finished run's display is cleared
    // back to waiting.
    void reset();

    State state() const { return state_; }
    // The next split's index (splits reached so far).
    std::size_t next_split() const { return times_.size(); }
    const std::vector<RunTime> &times() const { return times_; }
    // Real time and ticks since the start: up to now while running, the end once finished.
    RunTime elapsed() const;
    // The Assists that left their neutral value during this run; the run variables' values.
    const std::vector<std::string> &assists() const { return assists_; }
    std::vector<std::pair<std::string, std::string>> variables() const;
    bool in_category() const { return in_category_; }
    // The difference, in ms, between the last split reached and the best's same split; none
    // with no best or no split reached.
    std::optional<std::int64_t> difference() const;

    // The finished and reset runs since the last call, oldest first.
    std::vector<RunRecord> take_records();
    // The overlay's lines: the category and the time, the next split, the difference to the
    // personal best, clean or assisted (and outside the category), the run variables.
    std::vector<std::string> overlay_lines() const;

  private:
    const Settings &settings() const;
    RunTime since_start(const RunInstant &now) const;
    void check();
    void end_run(bool finished, const RunInstant &now);
    void signal(RunSignal s, const RunTime &t) const;
    // A finished run back to waiting (its display cleared): a reset to the listener.
    void clear_finished();

    RunClock clock_;
    RunListener listener_;
    const Settings *settings_ = nullptr;
    RunCategory category_;
    std::optional<RunRecord> best_;
    State state_ = State::kWaiting;
    RunInstant start_;
    std::string started_;
    std::string start_settings_;
    std::vector<RunTime> times_;
    RunTime end_;
    std::vector<std::string> assists_;
    // Each run variable and every value it had, in order.
    std::vector<std::pair<std::string, std::vector<std::string>>> variables_;
    bool in_category_ = true;
    std::vector<RunRecord> records_;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_RUN_TIMER_H
