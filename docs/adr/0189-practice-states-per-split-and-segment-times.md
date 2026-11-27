# 0189. Practice states per split and segment times

- **Status:** Accepted (delegated by Joris, 2026-11-27)
- **Date:** 2026-11-27
- **Issue:** #409 (epic #402)

## Context

The speedrun timer (#406, [ADR 0156](0156-speedrun-kit-splits-clean-runs-and-verifiable-replays.md)) knows a category's splits. Save states ([ADR 0105](0105-save-states-in-process-snapshot-at-safe-points.md), #204) save and load the running game at safe points, one state per character slot, behind the `save_states` Assist. A runner who wants to practise one segment (the chapter 1 boss, say) still has to play the run up to it each time. ADR 0156 already says practice tools are Assists, so a practice is never a clean run.

Questions to settle: where and when the states are taken, how a player chooses one, what the timer times after a load, and which segment times count as bests.

## Options considered

1. **When a state is taken.**
   - *At the split's own event*: the event fires inside a guest call, never at a safe point. Not possible.
   - *At the first safe turn after the split* (the quick-save's rule: the field, nothing open, nothing held). A later split before a safe turn replaces the armed one. Chosen.
2. **Where.** One file per category, character slot and split, `<data dir>/states/<category>/slot<S>-split<N>.state`, beside the quick-save's `slot<N>.state`. Per slot, because a restore checks the slot (another character's state is refused). The last split gets none: nothing follows it.
3. **How a player chooses.**
   - *One action per split*: unbounded, too many bindings.
   - *A menu*: heavy, and the overlay is not interactive.
   - *Two actions*: `practice_pick` cycles through the splits that have a state for the slot in play, and `practice_load` loads the picked one. F7 and F8 (F5 and F9 are the quick states), bindable, with no pad button by default, gated by `save_states` like F5 and F9. Chosen.
4. **What the timer times after a load.** The run in progress ends as reset (its log says so). The timer is then **practice**: it times each segment from the load (then from each split) to the next split's event, to the category's last split. A practice writes no run log and makes no personal best. A new character starts a real run again.
5. **Which segments count as bests.**
   - *Clean runs only*: practice needs `save_states`, so it could never set a best, and best segments would be useless for practice.
   - *Every segment*: fast-forward or game speed would make bests meaningless.
   - *A segment counts when no Assist but `save_states` left its neutral value during it, and every run variable stayed inside the category.* It counts from runs (each split's time less the one before) and from practice alike. Chosen.

## Decision

- `platform/src/practice_state.{h,cpp}`. With `save_states` and `run_timer` both on, each split the timer reaches (except the last) arms a state. The turn hook takes it at the first safe turn after the split, through the quick states' own save path (`save_state_to`: `mt_snapshot_safe`, `mt_snapshot_save`, `mt_saves_replace_file`). It is never taken in a quick-save's or quick-load's turn. A notice says it was kept.
- `practice_pick` (F7) and `practice_load` (F8) are port actions `MT_ACTION_PRACTICE_PICK` and `MT_ACTION_PRACTICE_LOAD`. The load goes through the quick-load's own path (`load_state_from`), with its checks and reasons and one more: "no practice state for this split yet". With no category timed, a press is refused with a notice.
- `RunTimer::start_practice(split)` marks the timer as practice. The overlay shows the loaded split, the next split, its best segment, the last segment and its difference to the best, and the sum of best. Outside a practice it shows the picked split.
- Best segments live in `runs/<category>.segments` ([run-category.md](../formats/run-category.md#best-segments)). The file is written whole when a best improves, and kept only while its splits equal the category's. The sum of best adds up every split's best once each has one.

## Consequences

- A runner can practise any segment after the first split without replaying the run. The first segment (from a new character) is practised by starting a new character, as before.
- Practice states cost disk: about 5 MB a state (deflated) per split and slot. A category of 6 splits and 3 slots is under 100 MB at most.
- The real-asset test `play.practice_state` takes two states from a save and loads the first. It found a snapshot bug: the audio section asked a larger minimum size of each voice than the smallest one takes, so a state with two music voices with empty buffers (Perion's map change) was refused. The fix is in `runtime/src/audio/audio.cpp`, with a synthetic regression test in `snapshot_test.cpp`.
- We'd know this was wrong if players want to practise from arbitrary points (not splits). The quick states already cover that, so it would be a new Assist, not a change here.
