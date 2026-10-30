// Autosave on quit and in the background (#150, docs/re/autosave.md): the port calls the game's
// own save pair, CDataMgr::SaveSaveData 0x38711 and SaveQuestData 0x38F55 (theif<slot> and
// quest<slot>, docs/formats/save.md), when the game is about to be left, and only where the
// game's menu Save would be allowed. "Save anywhere" was rejected (#90): boss state is not saved,
// and the game refuses to save on its boss maps on purpose.
//
// The guards, in the order they are checked (each from guest memory; the evidence is in
// docs/re/autosave.md):
//
//   the game's own, on the way to its Save (CStateGame::OnKeyPressed 0x8D4C9 opens the popup
//   menu, Game_ExitToMainMenu 0x54BF9 saves from its "Do you want to save?" popup)
//     not in the game  the engine's state is not a CStateGame (title, main menu, slot screen)
//     loading          game mode 6, 7 or 8 (entering the game, a map load): keys are ignored
//     scene            game mode 9 or 10: a CAniManager scene (the intro, story, boss scenes)
//     not the field    any other mode but 0 (the field) and 1 (the popup menu, where Save is)
//     popup            a popup is open (0x108C64 != -1)
//     warp             a map change is pending (CStateGame +0xB4)
//     knock-back       the hero is thrown back by a hit (CHero +0x119)
//     scroll           a scroll is being applied (CDataMgr +0x108)
//     dying            the hero is dying or dead (CHero +0x10, action 0 or 1)
//     no-save map      one of the 16 maps where Save says "Cannot save." (no_save_maps())
//   the port's (the game's menu stops the hero before it saves; the autosave does not)
//     hero busy        the hero is not standing, walking or climbing (CHero +0x10 action 2,
//                      3 or 4, and +0xC bits 1 and 3 clear: not in the air, not attacking):
//                      an attack, a jump, a fall or a skill would be saved half done
//
// When: game.cpp's turn hook, between two run-loop turns (never inside a tick), right before
// Lifecycle::deliver sends applicationWillResignActive: (a minimise, iOS's background, a focus
// loss under pause_unfocused) or applicationWillTerminate: (the close button, Cmd-Q, --quit),
// with autosave = on_quit. Once per leave: nothing is checked again until the guest has turned.
// The pair writes through the runtime's atomic file replacement (mt_saves_replace_file) and the
// backup hook on SaveQuestData (#148), as the game's own saves do. A skip logs the guard.
#ifndef MAPLETHIEF_PLATFORM_AUTOSAVE_H
#define MAPLETHIEF_PLATFORM_AUTOSAVE_H

#include <cstdint>
#include <functional>
#include <span>
#include <string>

#include "lifecycle.h"
#include "maplethief/recomp/cpu.h"
#include "settings.h"

namespace maplethief::frontend {

// Guest facts (docs/re/autosave.md).
inline constexpr std::uint32_t kAutosaveCanvas = 0x11A950;     // SkyCanvas*: +0xC the state
inline constexpr std::uint32_t kAutosaveGameVptr = 0x119B78;   // a CStateGame's vtable pointer
inline constexpr std::uint32_t kAutosaveDataMgr = 0x11A970;    // CDataMgr*
inline constexpr std::uint32_t kAutosaveGameMode = 0x11AA00;   // 0 field, 1 popup menu, ...
inline constexpr std::uint32_t kAutosavePopup = 0x108C64;      // the open popup, -1 none
inline constexpr std::uint32_t kAutosaveSaveData = 0x38711;    // CDataMgr::SaveSaveData
inline constexpr std::uint32_t kAutosaveSaveQuests = 0x38F55;  // CDataMgr::SaveQuestData

enum class AutosaveGuard : std::uint8_t {
    kNone, // every guard passes
    kNotInGame,
    kLoading,
    kScene,
    kNotField,
    kPopup,
    kWarp,
    kKnockBack,
    kScroll,
    kDying,
    kNoSaveMap,
    kHeroBusy,
};
// "none", "not in the game", "loading", "scene", "not the field", "popup", "warp",
// "knock-back", "scroll", "dying", "no-save map", "hero busy".
const char *autosave_guard_name(AutosaveGuard guard);

// A map where the game refuses to save, and why (docs/re/autosave.md).
struct NoSaveMap {
    int area, map;
    const char *kind; // "tutorial", "boss", "climb", "Monster Carnival"
};
// The 16, in the game's order (0x54BF9's and 0x8D4C9's compare chains).
std::span<const NoSaveMap> no_save_maps();
// The map's entry in no_save_maps(), or nullptr.
const NoSaveMap *find_no_save_map(int area, int map);

// Reads the 32-bit little-endian word at a guest address; false if it is not mapped.
using GuestRead32 = std::function<bool(std::uint32_t addr, std::uint32_t *out)>;

// What the guards found.
struct AutosaveCheck {
    AutosaveGuard guard = AutosaveGuard::kNotInGame;
    std::uint32_t data_mgr = 0, state = 0, hero = 0;
    std::int32_t mode = -1, popup = -1, area = -1, map = -1, action = -1, slot = -1;
    std::uint32_t flags = 0; // the hero's +0xC
    bool ok() const { return guard == AutosaveGuard::kNone; }
    // The failed guard with its value ("a scene (game mode 9)", "map 3.8, a boss map"), or
    // "slot 0, map 2.0".
    std::string describe() const;
};
AutosaveCheck check_autosave(const GuestRead32 &read);
// On the game's memory (an image segment or the committed heap; nothing else is read).
AutosaveCheck check_autosave(const mt_cpu *cpu);

struct AutosaveResult {
    bool saved = false;
    AutosaveCheck check;
};
// Checks the guards and, if all pass, runs the save pair with mt_call_guest (every register
// kept, inside an autorelease pool). Logs one line: "autosave: slot 0 saved at <trigger> ..."
// or "autosave: not saved at <trigger> ...: <guard>". Call it only between run-loop turns.
AutosaveResult autosave_now(mt_cpu *cpu, const char *trigger, double t);

// The turn hook's part.
class Autosaver {
  public:
    // With `mode` on_quit and a resign or a quit queued in `lifecycle` (Lifecycle::leaving),
    // autosave_now once for this turn (`turn`: the turn hook's count). Returns whether it saved.
    bool before_deliver(mt_cpu *cpu, Autosave mode, const Lifecycle &lifecycle,
                        std::uint64_t turn, double t);

    struct Stats {
        std::uint64_t saves = 0, skips = 0;
        AutosaveGuard last = AutosaveGuard::kNone;
    };
    const Stats &stats() const { return stats_; }

  private:
    std::uint64_t last_turn_ = UINT64_MAX;
    Stats stats_;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_AUTOSAVE_H
