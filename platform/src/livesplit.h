// The LiveSplit link (#407, ADR 0156's addendum): the run timer's start, splits and resets, and
// the game time, sent to LiveSplit's own TCP server, so a runner keeps their LiveSplit layout.
// Desktop only (kCapLiveSplit), off by default, and it reads nothing of LiveSplit's replies.
//
//   livesplit = off | HOST:PORT     settings.ini (Other, not in the presets), --livesplit
//   livesplit_game_time = on | off  send the game time too, --livesplit-game-time
//
// HOST is `localhost`, an IPv4 address or an IPv6 one in brackets ("[::1]:16834"): never a name
// to look up, so no DNS query can hold a turn. PORT may be left out: LiveSplit's default, 16834.
// The player starts LiveSplit's server by hand (Control, Start TCP Server).
//
// LiveSplit's server (LiveSplit 1.8.37, src/LiveSplit.Core/Server/CommandServer.cs): one command
// per line, ending in '\n'; the ones sent here have no reply. What a run sends (livesplit_lines):
//   - start:  starttimer; with game time also pausegametime and setgametime 0:00:00.000. Paused
//             game time moves only when it is set, so LiveSplit's game time is the ticks'.
//   - split:  with game time setgametime <the split's ticks>, then split.
//   - reset:  reset (a run reset, or a finished one cleared before the next start).
//   - while a run is in progress, with game time: setgametime whenever the ticks change.
// The game time is the run's game ticks × 50 ms (kLiveSplitTickMs), as H:MM:SS.mmm.
//
// The connection never blocks a turn: a non-blocking socket whose connect goes on across turns
// (given up after kLiveSplitConnectSeconds), lines queued while connecting (at most
// kLiveSplitMaxQueueBytes) and sent as the socket takes them. A refused or lost connection is
// tried again every kLiveSplitRetrySeconds, quietly: one log line per run of failures. Lines
// while there is no connection are dropped; LiveSplit then misses them (a run started before it
// connected is not in LiveSplit).
//
// Single thread: the frontend's (game.cpp's turn and the run timer's listener).
#ifndef MAPLETHIEF_PLATFORM_LIVESPLIT_H
#define MAPLETHIEF_PLATFORM_LIVESPLIT_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "run_timer.h"

namespace maplethief::frontend {

inline constexpr std::uint16_t kLiveSplitDefaultPort = 16834;
inline constexpr std::uint64_t kLiveSplitTickMs = 50;
inline constexpr double kLiveSplitRetrySeconds = 3.0;
inline constexpr double kLiveSplitConnectSeconds = 3.0;
inline constexpr std::size_t kLiveSplitMaxQueueBytes = 64 * 1024;
inline constexpr std::size_t kMaxLiveSplitAddressBytes = 64;

struct LiveSplitAddress {
    std::string host; // "localhost", "127.0.0.1", "::1" (no brackets)
    std::uint16_t port = kLiveSplitDefaultPort;

    bool operator==(const LiveSplitAddress &) const = default;
};

// "localhost", "localhost:16834", "192.168.1.20:16834", "[::1]:16834" (ASCII case ignored, the
// host written in lower case); nothing for anything else, "off" included.
std::optional<LiveSplitAddress> parse_livesplit_address(std::string_view text);
// HOST:PORT, as parse_livesplit_address reads it back ("[::1]:16834").
std::string livesplit_address_text(const LiveSplitAddress &a);

// LiveSplit's time of `ticks` game ticks: "0:01:02.350".
std::string livesplit_game_time(std::uint64_t ticks);
// The command lines of one run signal (no line ends), as the header says.
std::vector<std::string> livesplit_lines(RunSignal s, const RunTime &t, bool game_time);

class LiveSplitLink {
  public:
    using Log = std::function<void(const std::string &)>;
    using Now = std::function<double()>; // a monotonic clock, in seconds

    LiveSplitLink(Log log, Now now);
    ~LiveSplitLink();
    LiveSplitLink(const LiveSplitLink &) = delete;
    LiveSplitLink &operator=(const LiveSplitLink &) = delete;

    // Where to connect (none: off) and whether to send the game time. A new address closes the
    // connection and connects to it at the next pump.
    void configure(const std::optional<LiveSplitAddress> &address, bool game_time);
    const std::optional<LiveSplitAddress> &address() const { return address_; }

    // The run timer's listener (RunTimer::set_listener): queues the signal's lines.
    void on_signal(RunSignal s, const RunTime &t);
    // Once a turn: the game time of a run in progress (`running`, its elapsed time), then pump.
    void turn(bool running, const RunTime &elapsed);
    // Moves the connection on and sends what the socket takes; never waits.
    void pump();

    bool connected() const { return state_ == State::kConnected; }

  private:
    enum class State { kOff, kIdle, kConnecting, kConnected };

    void queue(const std::string &line);
    void start_connect();
    void fail(const std::string &why);
    void close_socket();

    Log log_;
    Now now_;
    std::optional<LiveSplitAddress> address_;
    bool game_time_ = true;
    State state_ = State::kOff;
    std::intptr_t socket_ = -1;               // the platform's socket, -1 for none
    double retry_at_ = 0;                     // kIdle: when to connect again
    double connect_started_ = 0;              // kConnecting: when the connect began
    bool failing_ = false;                    // logged a failure since the last connection
    std::string out_;                         // bytes not yet sent
    std::optional<std::uint64_t> sent_ticks_; // the last game time sent in this run
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_LIVESPLIT_H
