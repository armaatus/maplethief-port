// The frontend's side of a save state's restore (#203, ADR 0105 addendum): before
// mt_snapshot_restore, everything the player holds is let go and the per-turn helpers start over.
//
// The runtime's own input (the key pad's held actions, the queue, the touches UIKit tracks) is
// part of the state and comes back as saved: released, since a save is only made with nothing
// held (mt_snapshot_safe). The frontend's layers above it are not part of the state. They still
// hold what the player holds now (a key, a button, a finger, the pointer's touch) and keep timing
// of their own (turbo, potion, walk latch, menu repeat, the scene skip's tap): those would press
// or release into the restored game what belongs to the present. So, in the turn hook, before the
// restore: every source's controls are released (the releases reach the runtime's queue, which the
// restore then replaces), every touch is lifted, and the helpers forget their state. A key still
// physically down after the restore acts again only when it is pressed again.
#ifndef MAPLETHIEF_PLATFORM_SAVE_STATE_INPUT_H
#define MAPLETHIEF_PLATFORM_SAVE_STATE_INPUT_H

namespace maplethief::frontend {

class Input;
class Actions;
class Gamepads;
class PadPointer;
class MenuInput;
class Assists;
class SceneSkip;

// The frontend's input layers (any may be null: a layer the caller does not run).
struct RestoreInput {
    Input *input = nullptr;
    Actions *actions = nullptr;
    Gamepads *gamepads = nullptr;
    PadPointer *pointer = nullptr;
    MenuInput *menu = nullptr;
    Assists *assists = nullptr;
    SceneSkip *scene_skip = nullptr;
};

// Lets go of everything and restarts the helpers, as above. `t`: the run's seconds (the log).
void release_for_restore(const RestoreInput &layers, double t);

} // namespace maplethief::frontend

#endif
