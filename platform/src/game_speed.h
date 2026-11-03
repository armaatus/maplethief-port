// Game speed and fast-forward (#151, ADR 0087): two Assists, off in every preset.
//
// game_speed (settings.h's GameSpeed). The game ticks when CMTimer's interval (the i64 at
// 0x11A478) has passed on its clock; CDataMgr::ApplyGameSpeed 0x37C41 sets it, from
// LoadConfig at boot and from the options menu, to g_gameSpeedIntervals[config[1]]: the i32
// table {65, 50, 40, -1} at 0x108C58, indexed by the Speed option's byte of the game's config
// (docs/re/timing-threading.md). A game_speed other than `game` writes its tick length into the
// table's three entries:
//   - at the start, after the process loads and before it runs (patch_game_speed): LoadConfig's
//     ApplyGameSpeed then sets it, as it sets the game's own;
//   - while the game runs (apply_game_speed, from the turn hook, for the settings menu): the
//     table again, and CMTimer's interval directly, as ApplyGameSpeed's SetFrameInterval
//     0x21DA1 would (`game` puts back the game's table and the Speed option's own length).
// The game's Speed option then changes nothing (each of its values reads the same length), and
// the config file keeps the option's own index (0 to 2): a port-only length never reaches it,
// and #78's clamp of a bad index (mt_saves_sanitise) is untouched. `game` writes nothing at the
// start: the run is the original's, byte for byte.
//
// fast_forward (settings.h's FastForward): fast_forward_rate() is the guest clock's rate while
// the fast_forward action is held (actions.h: a port action, never sent to the guest). game.cpp
// passes it to the paced clock's plan (pacing.h).
#ifndef MAPLETHIEF_PLATFORM_GAME_SPEED_H
#define MAPLETHIEF_PLATFORM_GAME_SPEED_H

#include <cstdint>

#include "maplethief/recomp/cpu.h"
#include "settings.h"

namespace maplethief::frontend {

// The game's addresses (armv7 VM addresses; docs/re/timing-threading.md).
inline constexpr std::uint32_t kGameSpeedTable = 0x108C58; // g_gameSpeedIntervals, i32[4]
inline constexpr std::uint32_t kFrameInterval = 0x11A478;  // CMTimer's interval, i64 ms
inline constexpr std::uint32_t kConfigPointer = 0x11A98C;  // the config's 6 bytes (a pointer)
// The table as the game ships it: 65, 50 and 40 ms, then the popup id that a speed byte of 3
// reads (-1).
inline constexpr std::int32_t kGameSpeedLengths[3] = {65, 50, 40};

// The tick length of a game_speed in ms; 0 for `game` (the game's own option decides).
std::uint32_t game_speed_ms(GameSpeed speed);

// Writes `speed`'s table into guest memory: its length three times, or for `game` the game's
// own {65, 50, 40}. False (nothing written) when the guest does not map the table or the table
// is not the game's ({65, 50, 40, -1}, or a length written before, three times, then -1).
bool patch_game_speed(mt_cpu *cpu, GameSpeed speed);

// A change while the game runs: patch_game_speed, then CMTimer's interval, once the game has
// its config (the pointer at 0x11A98C set): `speed`'s length, or for `game` the game's own
// length for its Speed option (config byte 1, clamped to 2 as mt_saves_sanitise reads it).
// Returns the interval written (ms), or 0 if none was (no table, no config yet).
std::int64_t apply_game_speed(mt_cpu *cpu, GameSpeed speed);

// The guest clock's rate: 2 or 3 while the fast-forward key is held with fast_forward on, else 1.
std::uint32_t fast_forward_rate(FastForward ff, bool held);

// The tick intervals over this are overruns (FrameStats::set_overrun_ns): 1.1 x the tick's
// length on the host's clock (interval / rate). The default's 55 ms for an unknown interval.
std::uint64_t overrun_ns_for(std::int64_t tick_ms, std::uint32_t rate);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_GAME_SPEED_H
