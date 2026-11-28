// Practice states (#409, ADR 0189): a save state after each split of the speedrun timer's
// category, and a port action that loads the one a player picks, so a segment can be practised
// without replaying the run before it.
//
//   With save_states and run_timer on, each split the timer reaches (run_timer.h's take_reached),
//   but the category's last, arms a state, taken at the first safe turn after it (as quick_save
//   takes one: mt_snapshot_safe, then mt_snapshot_save, written atomically) to
//   <data dir>/states/<category>/slot<S>-split<N>.state: one per category, character slot and
//   split. A later split before a safe turn replaces the armed one. A practice's own splits take
//   states too, so the newest state of a split is the one kept.
//
//   practice_pick (F7; MT_ACTION_PRACTICE_PICK, unbound on pads) picks the next split that has a
//   state for the character in play, after the one picked (from the first, wrapping); the overlay
//   shows it. practice_load (F8; MT_ACTION_PRACTICE_LOAD) loads the picked split's state (none
//   picked: it picks first), with the quick-load's checks and reasons (quick_state.h's
//   load_state_from), and the timer then practises from it (RunTimer::start_practice).
//
// The presses do nothing with save_states off (logged, as quick_save's). With save_states on but
// no category timed, they are refused with a notice. Single thread: the turn hook's.
#ifndef MAPLETHIEF_PLATFORM_PRACTICE_STATE_H
#define MAPLETHIEF_PLATFORM_PRACTICE_STATE_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "quick_state.h"

namespace maplethief::frontend {

class Actions;

// <data dir>/states/<category>/slot<S>-split<N>.state ("" for a slot outside 0-2, split 0 or a
// category that is not 1-32 of a-z, 0-9 and _).
std::string practice_state_path(const std::string &data_dir, const std::string &category, int slot,
                                std::size_t split);
// Whether the file at `path` exists (a regular file).
bool practice_state_exists(const std::string &path);

enum class PracticeRequest { kNone, kPick, kLoad };

class PracticeStates {
  public:
    // The new presses of practice_pick and practice_load since the last call, as QuickStates::take
    // (kNone with `on` false, and for both in one turn).
    PracticeRequest take(const Actions &actions, bool on);
    PracticeRequest take(std::uint64_t picks, std::uint64_t loads, bool on);

    // Split `split` (1-based) of a category of `splits` splits was reached: its state is armed
    // (the last split's is not: nothing follows it).
    void reached(std::size_t split, std::size_t splits);
    // The split armed, if one is.
    const std::optional<std::size_t> &armed() const { return armed_; }
    // Drops the armed split (another category, the timer or save_states off).
    void forget() { armed_.reset(); }

    // At a turn: the armed split's state, if the game is at a safe point now; nothing while it is
    // not (the next turn tries again). A save or write that fails drops it (the result says why).
    QuickResult take_armed(const QuickOps &ops, const std::string &data_dir,
                           const std::string &category);

    // The split after `picked` (none: from the first) whose state exists for the slot in play, up
    // to the last but one split of `splits`, wrapping; none if no split has one.
    std::optional<std::size_t> pick(std::optional<std::size_t> picked, std::size_t splits,
                                    const QuickOps &ops, const std::string &data_dir,
                                    const std::string &category,
                                    const std::function<bool(const std::string &)> &exists) const;
    // Loads split `split`'s state for the slot in play, as quick_load does.
    QuickResult load(const QuickOps &ops, const std::string &data_dir, const std::string &category,
                     std::size_t split);

    struct Stats {
        std::uint64_t saves = 0, loads = 0, refused = 0;
    };
    const Stats &stats() const { return stats_; }

  private:
    std::uint64_t seen_picks_ = 0, seen_loads_ = 0;
    std::optional<std::size_t> armed_;
    Stats stats_;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_PRACTICE_STATE_H
