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
// When, with autosave = on_quit:
//   - game.cpp's turn hook, between two run-loop turns (never inside a tick), right before
//     Lifecycle::deliver sends applicationWillResignActive: (a minimise, iOS's background, a
//     focus loss under pause_unfocused) or applicationWillTerminate: (the close button, Cmd-Q,
//     the settings menu's Quit, --quit). Once per leave: nothing is checked again until the
//     guest has turned.
//   - the exit to the main menu (#179): Yes on the popup menu's "exit" popup 14, at the entry
//     of Game_ExitToMainMenu 0x54BF9 (hooked), which never saves; the popup guard lets that
//     popup through. The death popup's "Go to main menu" (CStateGame::OnKeyPressed 0x8D4C9) and
//     a scene's exit (0x83C71) do not pass there: they leave without a save, as the game does.
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
inline constexpr std::uint32_t kAutosaveCanvas = 0x11A950;    // SkyCanvas*: +0xC the state
inline constexpr std::uint32_t kAutosaveGameVptr = 0x119B78;  // a CStateGame's vtable pointer
inline constexpr std::uint32_t kAutosaveDataMgr = 0x11A970;   // CDataMgr*
inline constexpr std::uint32_t kAutosaveGameMode = 0x11AA00;  // 0 field, 1 popup menu, ...
inline constexpr std::uint32_t kAutosavePopup = 0x108C64;     // the open popup, -1 none
inline constexpr std::uint32_t kAutosaveSaveData = 0x38711;   // CDataMgr::SaveSaveData
inline constexpr std::uint32_t kAutosaveSaveQuests = 0x38F55; // CDataMgr::SaveQuestData
// The exit to the main menu (#179; Game_ExitToMainMenu, MT_HOOK_FN_GAME_EXIT_TO_MAIN_MENU):
// its popup 14, and the open popup's widget (+0x14: 0 when Yes is picked).
inline constexpr std::int32_t kAutosaveExitPopup = 14;          // "exit to the main menu"
inline constexpr std::uint32_t kAutosavePopupWidget = 0x11A96C; // the popup's widget*

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
// A GuestRead32 on the game's memory: an image segment or the committed heap, nothing else.
GuestRead32 guest_reader(const mt_cpu *cpu);

// `open_popup`: a popup the guard lets through (the exit's 14), -1 none.
AutosaveCheck check_autosave(const GuestRead32 &read, std::int32_t open_popup = -1);
// On the game's memory (an image segment or the committed heap; nothing else is read).
AutosaveCheck check_autosave(const mt_cpu *cpu, std::int32_t open_popup = -1);

// Whether Game_ExitToMainMenu's `key` leaves for the main menu (#179): popup 14 is open, the
// key is OK (-5 or '5') and Yes is picked, the branch at 0x5521C (docs/re/autosave.md).
bool is_exit_to_menu_confirm(const GuestRead32 &read, std::int32_t key);

struct AutosaveResult {
    bool saved = false;
    AutosaveCheck check;
};
// Checks the guards and, if all pass, runs the save pair with mt_call_guest (every register
// kept, inside an autorelease pool). Logs one line: "autosave: slot 0 saved at <trigger> ..."
// or "autosave: not saved at <trigger> ...: <guard>". Call it between run-loop turns, or from a
// hook at a function's entry where the game itself could save (Game_ExitToMainMenu's: the
// caller restores every register after it, as exit_to_menu_hook does).
AutosaveResult autosave_now(mt_cpu *cpu, const char *trigger, double t,
                            std::int32_t open_popup = -1);

// The turn hook's part, and the exit to the main menu's hook.
class Autosaver {
  public:
    Autosaver() = default;
    Autosaver(const Autosaver &) = delete;
    Autosaver &operator=(const Autosaver &) = delete;
    ~Autosaver() { uninstall_exit_to_menu(); }

    // Hooks Game_ExitToMainMenu (#179): with *mode on_quit, Yes on its popup 14 autosaves
    // before the game leaves for the main menu. `mode` (the settings in effect) and `clock` (the
    // log's seconds) are kept and read at each call.
    void install_exit_to_menu(const Autosave *mode, std::function<double()> clock);
    void uninstall_exit_to_menu();
    // The hook's part: autosave_now if `key` confirms the exit and *mode is on_quit.
    bool exit_to_menu(mt_cpu *cpu, std::int32_t key);

    // With `mode` on_quit and a resign or a quit queued in `lifecycle` (Lifecycle::leaving),
    // autosave_now once for this turn (`turn`: the turn hook's count). Returns whether it saved.
    bool before_deliver(mt_cpu *cpu, Autosave mode, const Lifecycle &lifecycle, std::uint64_t turn,
                        double t);

    struct Stats {
        std::uint64_t saves = 0, skips = 0;
        AutosaveGuard last = AutosaveGuard::kNone;
    };
    const Stats &stats() const { return stats_; }

  private:
    void count(const AutosaveResult &r);

    std::uint64_t last_turn_ = UINT64_MAX;
    Stats stats_;
    const Autosave *exit_mode_ = nullptr;
    std::function<double()> clock_;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_AUTOSAVE_H
