// Quick-save and quick-load (#204, ADR 0105's addendum, docs/re/save-states.md): the save
// states of maplethief/snapshot.h as an Assist (ADR 0070), save_states = off | on, off in every
// preset.
//
//   quick_save  (F5; the port action MT_ACTION_QUICK_SAVE, unbound on pads) the game as it is
//               now into <data dir>/states/slot<N>.state, N the character slot in play (CDataMgr
//               +0x4E4, 0-2): one state per character, written atomically (mt_saves_replace_file,
//               ADR 0074: a temporary file, flushed, renamed over the old state).
//   quick_load  (F9; MT_ACTION_QUICK_LOAD) that state back, in the same process: the frontend's
//               input let go first (save_state_input.h), then mt_snapshot_restore, the clock
//               behind the guest re-based to the state's time.
//
// Both run in the turn hook (between two run-loop turns) at the turn after the press, and only
// at a safe point (mt_snapshot_safe: the field, nothing open, no scene, no animation, nothing
// held). A load does not ask for nothing held: what is held is let go, and the state's own input
// replaces it. Anything else is refused with the reason, as a notice in the window ("Not saved:
// not in a menu"). A load is also refused, before anything changes, for a state of another
// game binary (its SHA-256), another port version (MT_VERSION), another character slot, or a
// damaged file (every length and CRC is checked by the runtime first).
//
// A state is never the game's save: neither action touches theif<N> or quest<N> (ADR 0089 keeps
// "save anywhere" out of them). The game's next save after a load writes whatever is loaded, as
// after loading an older save.
//
// The port's overlays (the settings menu and its save list, the remap overlay, the controls
// card, the disconnect pause, the key pad editor) hold the turn and take every key while open
// (game.cpp's run_overlays): F5 and F9 do nothing there, and opening one lets go of every key.
// The pad pointer is released with the rest of the input (release_for_restore).
//
// Single thread: the turn hook's.
#ifndef MAPLETHIEF_PLATFORM_QUICK_STATE_H
#define MAPLETHIEF_PLATFORM_QUICK_STATE_H

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "maplethief/recomp/recomp.h"

namespace maplethief::frontend {

class Actions;

// <data dir>/states/slot<N>.state ("" for a slot outside 0-2).
std::string quick_state_path(const std::string &data_dir, int slot);

// The player's words (port_text.def, tr) for a refusal of mt_snapshot_safe, mt_snapshot_save or
// mt_snapshot_restore ("not in the field (game mode 4)" -> "not in a menu"); a reason it does not
// know is the runtime's own.
std::string quick_state_reason(const std::string &why);
// Whether a refusal of mt_snapshot_safe is about held input (a key, a touch, queued input),
// which a load lets go of instead.
bool held_input_reason(const std::string &why);

// Checks a state file before a load: the runtime's header check (magic, format, CRC, the
// binary's hash, the slot), then the port version. "" if it may be loaded, else the player's
// reason.
std::string check_quick_state(const std::vector<std::uint8_t> &bytes, const std::uint8_t sha[32],
                              std::uint32_t slot, const char *port_version);

// The state file: written atomically through mt_saves_replace_file (ADR 0074: "<path>.mt-tmp",
// flushed, renamed over `path`; the directories made), read whole (nullopt: none, unreadable, or
// over 1 GiB).
bool write_quick_state(const std::string &path, const std::vector<std::uint8_t> &bytes);
std::optional<std::vector<std::uint8_t>> read_quick_state(const std::string &path);

enum class QuickRequest { kNone, kSave, kLoad };
const char *quick_request_name(QuickRequest r);

// The game's side, which the tests replace.
struct QuickOps {
    // mt_snapshot_safe: true, or false with the reason.
    std::function<bool(std::string *why)> safe;
    // The character slot in play (0-2), or -1.
    std::function<int()> slot;
    // mt_snapshot_save of the game for `slot`.
    std::function<bool(std::uint32_t slot, std::vector<std::uint8_t> *out, std::string *why)> save;
    // check_quick_state for `slot`.
    std::function<std::string(const std::vector<std::uint8_t> &bytes, std::uint32_t slot)> check;
    // Lets go of the frontend's input, then mt_snapshot_restore (the clock re-based).
    std::function<bool(const std::vector<std::uint8_t> &bytes, std::uint32_t slot,
                       std::string *why)>
        restore;
    // The file: written atomically, read whole (nullopt: there is none, or it cannot be read).
    std::function<bool(const std::string &path, const std::vector<std::uint8_t> &bytes)> write;
    std::function<std::optional<std::vector<std::uint8_t>>(const std::string &path)> read;
};

// What a request did: the notice's text (the player's language) and the log's line (English).
struct QuickResult {
    QuickRequest request = QuickRequest::kNone;
    bool done = false;
    std::string notice;
    std::string log;
};

class QuickStates {
  public:
    // The new presses of quick_save and quick_load since the last call, in the field or a menu;
    // kNone with `on` false (save_states off: the press is logged and forgotten). Called every
    // turn whatever the setting, so that a press while it is off never acts later. Both in one
    // turn: neither (logged).
    QuickRequest take(const Actions &actions, bool on);
    // The same on press counts (tests): the totals so far.
    QuickRequest take(std::uint64_t saves, std::uint64_t loads, bool on);

    // Runs `r` (kNone: nothing, an empty result). `data_dir`: where states/ goes.
    QuickResult run(QuickRequest r, const QuickOps &ops, const std::string &data_dir);

    struct Stats {
        std::uint64_t saves = 0, loads = 0, refused = 0;
        std::size_t last_bytes = 0;
    };
    const Stats &stats() const { return stats_; }

  private:
    QuickResult save(const QuickOps &ops, const std::string &data_dir);
    QuickResult load(const QuickOps &ops, const std::string &data_dir);

    std::uint64_t seen_saves_ = 0, seen_loads_ = 0;
    Stats stats_;
};

// QuickOps on the running game: mt_snapshot_safe; the slot from CDataMgr; mt_snapshot_save with
// the binary's hash (taken once); check_quick_state with MT_VERSION; `release` then
// mt_snapshot_restore with `rebase` (mt_snapshot_expect's rebase_clock and user);
// mt_saves_replace_file; a whole-file read.
QuickOps game_quick_ops(mt_cpu *cpu, std::function<void()> release,
                        void (*rebase)(void *user, std::uint64_t guest_unix_us), void *user);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_QUICK_STATE_H
