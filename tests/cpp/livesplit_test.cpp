// The LiveSplit link (#407, livesplit.h, ADR 0156's addendum): the address parser, the command
// lines of each run signal, the exact lines a test TCP server receives for a synthetic run of the
// run timer (start, two splits, a reset, the game time), and a refused or unanswered connection
// that never holds a turn. The server is a listening socket of this test on 127.0.0.1.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "livesplit.h"
#include "maplethief/events.h"
#include "run_timer.h"
#include "settings.h"

namespace {

namespace fe = maplethief::frontend;

#if defined(_WIN32)
using Socket = SOCKET;
const Socket kNoSocket = INVALID_SOCKET;
void close_socket(Socket s) { closesocket(s); }
#else
using Socket = int;
const Socket kNoSocket = -1;
void close_socket(Socket s) { ::close(s); }
#endif

// A listening socket on 127.0.0.1 and a free port; accept() and read the lines of one client.
class TestServer {
  public:
    TestServer() {
#if defined(_WIN32)
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
#endif
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        REQUIRE(listener_ != kNoSocket);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        REQUIRE(bind(listener_, reinterpret_cast<sockaddr *>(&a),
                     static_cast<socklen_t>(sizeof a)) == 0);
        REQUIRE(listen(listener_, 4) == 0);
        socklen_t len = static_cast<socklen_t>(sizeof a);
        REQUIRE(getsockname(listener_, reinterpret_cast<sockaddr *>(&a), &len) == 0);
        port_ = ntohs(a.sin_port);
    }
    ~TestServer() {
        if (client_ != kNoSocket) {
            close_socket(client_);
        }
        close_socket(listener_);
    }
    TestServer(const TestServer &) = delete;
    TestServer &operator=(const TestServer &) = delete;

    std::uint16_t port() const { return port_; }

    // Whether a client is waiting or connected (polls, never waits long).
    bool accept_client() {
        if (client_ != kNoSocket) {
            return true;
        }
        if (!ready(listener_)) {
            return false;
        }
        client_ = accept(listener_, nullptr, nullptr);
        return client_ != kNoSocket;
    }

    // What the client sent so far (reads what is ready).
    const std::string &received() {
        while (client_ != kNoSocket && ready(client_)) {
            char b[512];
            const auto n = recv(client_, b, static_cast<int>(sizeof b), 0);
            if (n <= 0) {
                break;
            }
            data_.append(b, static_cast<std::size_t>(n));
        }
        return data_;
    }

    // Closes the client's connection (LiveSplit quitting).
    void drop_client() {
        if (client_ != kNoSocket) {
            close_socket(client_);
            client_ = kNoSocket;
        }
    }

  private:
    // Something to read (or a client to accept) within 20 ms.
    static bool ready(Socket s) {
#if defined(_WIN32)
        WSAPOLLFD p{s, POLLRDNORM, 0};
        return WSAPoll(&p, 1, 20) > 0;
#else
        pollfd p{s, POLLIN, 0};
        return poll(&p, 1, 20) > 0;
#endif
    }

    Socket listener_ = kNoSocket;
    Socket client_ = kNoSocket;
    std::uint16_t port_ = 0;
    std::string data_;
};

// A clock the test moves by hand, for the run timer and the link's retry delay.
struct FakeClock {
    fe::RunInstant at;
    fe::RunClock clock() {
        return fe::RunClock{[this] { return at; },
                            [] { return std::string("2026-11-27 10:00:00"); }};
    }
};

mt_game_event event(mt_game_event_kind kind, std::int32_t value) {
    mt_game_event e{};
    e.kind = static_cast<std::uint32_t>(kind);
    e.value = value;
    return e;
}

fe::RunCategory three_chapters() {
    std::vector<std::string> problems;
    std::optional<fe::RunCategory> c = fe::parse_run_category("test",
                                                              "title = Test%\n"
                                                              "split chapter_end 0 = Tutorial\n"
                                                              "split chapter_end 1 = Chapter 1\n"
                                                              "split chapter_end 2 = Chapter 2\n",
                                                              &problems);
    REQUIRE(c.has_value());
    return *c;
}

double wall_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Pumps `link` (and the server) until `done` or a second has gone by.
template <typename Done> bool pump_until(fe::LiveSplitLink &link, Done done) {
    const double until = wall_seconds() + 2.0;
    while (wall_seconds() < until) {
        link.pump();
        if (done()) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("livesplit: addresses") {
    using A = fe::LiveSplitAddress;
    CHECK(fe::parse_livesplit_address("localhost") == A{"localhost", 16834});
    CHECK(fe::parse_livesplit_address("LocalHost:16835") == A{"localhost", 16835});
    CHECK(fe::parse_livesplit_address("127.0.0.1:16834") == A{"127.0.0.1", 16834});
    CHECK(fe::parse_livesplit_address("192.168.1.20") == A{"192.168.1.20", 16834});
    CHECK(fe::parse_livesplit_address("[::1]:9000") == A{"::1", 9000});
    CHECK(fe::parse_livesplit_address("[::1]") == A{"::1", 16834});
    for (const char *bad : {"", "off", "example.com:16834", "127.0.0.1:", "127.0.0.1:0",
                            "127.0.0.1:65536", "127.0.0.1:12x", "256.0.0.1", "1.2.3", "01.2.3.4",
                            "::1:16834", "[::1", "[::1]16834", "[host]:1", "localhost:1:2"}) {
        CAPTURE(bad);
        CHECK_FALSE(fe::parse_livesplit_address(bad).has_value());
    }
    CHECK(fe::livesplit_address_text(A{"::1", 16834}) == "[::1]:16834");
    CHECK(fe::livesplit_address_text(A{"localhost", 16834}) == "localhost:16834");
}

TEST_CASE("livesplit: the settings rows") {
    fe::Settings s;
    std::string why;
    const fe::SettingInfo *row = fe::find_setting("livesplit");
    REQUIRE(row != nullptr);
    CHECK(fe::setting_text(s, *row) == "off");
    REQUIRE(fe::apply_setting(&s, "livesplit", "LocalHost", &why));
    CHECK(s.livesplit == "localhost:16834");
    CHECK(fe::setting_text(s, *row) == "localhost:16834");
    CHECK_FALSE(fe::apply_setting(&s, "livesplit", "example.com:16834", &why));
    CHECK(s.livesplit == "localhost:16834");
    REQUIRE(fe::apply_setting(&s, "livesplit", "OFF", &why));
    CHECK(s.livesplit.empty());
    CHECK(s.livesplit_game_time);
    REQUIRE(fe::apply_setting(&s, "livesplit_game_time", "off", &why));
    CHECK_FALSE(s.livesplit_game_time);
}

TEST_CASE("livesplit: the lines of each signal and the game time") {
    CHECK(fe::livesplit_game_time(0) == "0:00:00.000");
    CHECK(fe::livesplit_game_time(1247) == "0:01:02.350");
    CHECK(fe::livesplit_game_time(72000 + 1) == "1:00:00.050");
    using S = fe::RunSignal;
    const fe::RunTime t{12345, 247};
    CHECK(fe::livesplit_lines(S::kStart, {}, false) == std::vector<std::string>{"starttimer"});
    CHECK(fe::livesplit_lines(S::kStart, {}, true) ==
          std::vector<std::string>{"starttimer", "pausegametime", "setgametime 0:00:00.000"});
    CHECK(fe::livesplit_lines(S::kSplit, t, false) == std::vector<std::string>{"split"});
    CHECK(fe::livesplit_lines(S::kSplit, t, true) ==
          std::vector<std::string>{"setgametime 0:00:12.350", "split"});
    CHECK(fe::livesplit_lines(S::kReset, t, true) == std::vector<std::string>{"reset"});
}

TEST_CASE("livesplit: a synthetic run reaches a test server as exact lines") {
    TestServer server;
    FakeClock clock;
    std::vector<std::string> log;
    fe::LiveSplitLink link([&](const std::string &l) { log.push_back(l); },
                           [&] { return clock.at.seconds; });
    fe::RunTimer timer(clock.clock());
    timer.set_category(three_chapters(), std::nullopt);
    timer.set_listener([&](fe::RunSignal s, const fe::RunTime &t) { link.on_signal(s, t); });
    link.configure(fe::LiveSplitAddress{"127.0.0.1", server.port()}, true);
    REQUIRE(pump_until(link, [&] { return server.accept_client() && link.connected(); }));

    // One turn of the game: the link sends the game time of a run in progress, then pumps.
    const auto turn = [&](double seconds, std::uint64_t ticks) {
        clock.at.seconds += seconds;
        clock.at.ticks += ticks;
        timer.turn();
        link.turn(timer.state() == fe::RunTimer::State::kRunning, timer.elapsed());
    };
    timer.on_event(event(MT_GAME_EVENT_NEW_CHARACTER, 0));
    turn(0.05, 1);
    turn(0.05, 0); // the same tick: no line
    turn(1.00, 19);
    timer.on_event(event(MT_GAME_EVENT_CHAPTER_END, 0));
    turn(0.05, 1);
    timer.on_event(event(MT_GAME_EVENT_CHAPTER_END, 2)); // a later split's event: nothing
    timer.on_event(event(MT_GAME_EVENT_CHAPTER_END, 1));
    turn(2.0, 40);
    timer.reset();
    turn(0.05, 1); // no run: no game time

    const std::string expected = "starttimer\n"
                                 "pausegametime\n"
                                 "setgametime 0:00:00.000\n"
                                 "setgametime 0:00:00.050\n"
                                 "setgametime 0:00:01.000\n"
                                 "setgametime 0:00:01.000\n"
                                 "split\n"
                                 "setgametime 0:00:01.050\n"
                                 "setgametime 0:00:01.050\n"
                                 "split\n"
                                 "setgametime 0:00:03.050\n"
                                 "reset\n";
    REQUIRE(pump_until(link, [&] { return server.received().size() >= expected.size(); }));
    CHECK(server.received() == expected);
    CHECK(log == std::vector<std::string>{"livesplit: connected to 127.0.0.1:" +
                                          std::to_string(server.port())});
}

TEST_CASE("livesplit: without game time, only start, split and reset; a finished run resets") {
    TestServer server;
    FakeClock clock;
    fe::LiveSplitLink link([](const std::string &) {}, [&] { return clock.at.seconds; });
    fe::RunTimer timer(clock.clock());
    timer.set_category(three_chapters(), std::nullopt);
    timer.set_listener([&](fe::RunSignal s, const fe::RunTime &t) { link.on_signal(s, t); });
    link.configure(fe::LiveSplitAddress{"localhost", server.port()}, false);
    REQUIRE(pump_until(link, [&] { return server.accept_client() && link.connected(); }));
    timer.on_event(event(MT_GAME_EVENT_NEW_CHARACTER, 0));
    for (std::int32_t chapter = 0; chapter < 3; chapter++) {
        clock.at.ticks += 10;
        link.turn(true, timer.elapsed());
        timer.on_event(event(MT_GAME_EVENT_CHAPTER_END, chapter));
    }
    CHECK(timer.state() == fe::RunTimer::State::kFinished);
    timer.on_event(event(MT_GAME_EVENT_NEW_CHARACTER, 0)); // the next run
    const std::string expected = "starttimer\nsplit\nsplit\nsplit\nreset\nstarttimer\n";
    REQUIRE(pump_until(link, [&] { return server.received().size() >= expected.size(); }));
    CHECK(server.received() == expected);
}

TEST_CASE("livesplit: a refused connection never stalls a turn, logs once, and retries") {
    std::uint16_t closed_port = 0;
    {
        TestServer gone; // a port nobody listens on once it closes
        closed_port = gone.port();
    }
    FakeClock clock;
    std::vector<std::string> log;
    fe::LiveSplitLink link([&](const std::string &l) { log.push_back(l); },
                           [&] { return clock.at.seconds; });
    link.configure(fe::LiveSplitAddress{"127.0.0.1", closed_port}, true);
    double slowest = 0;
    for (int i = 0; i < 400; i++) { // 20 s of turns at 50 ms
        const double before = wall_seconds();
        link.on_signal(fe::RunSignal::kStart, {});
        link.turn(true, fe::RunTime{i * 50, static_cast<std::uint64_t>(i)});
        slowest = std::max(slowest, wall_seconds() - before);
        clock.at.seconds += 0.05;
        CHECK_FALSE(link.connected());
    }
    CHECK(slowest < 0.05);
    REQUIRE(log.size() == 1);
    CHECK(log[0].rfind("livesplit: cannot connect to 127.0.0.1:" + std::to_string(closed_port),
                       0) == 0);

    // LiveSplit starts later: the next retry connects, quietly until then.
    TestServer server;
    link.configure(fe::LiveSplitAddress{"127.0.0.1", server.port()}, true);
    REQUIRE(pump_until(link, [&] { return server.accept_client() && link.connected(); }));
    CHECK(log.back() == "livesplit: connected to 127.0.0.1:" + std::to_string(server.port()));

    // It quits: the link notices, logs once, and sends nothing until it is back.
    server.drop_client();
    const std::size_t lines = log.size();
    pump_until(link, [&] { return !link.connected(); });
    CHECK_FALSE(link.connected());
    CHECK(log.size() == lines + 1);
    CHECK(log.back().rfind("livesplit: lost 127.0.0.1:", 0) == 0);
}

TEST_CASE("livesplit: an address that never answers never stalls a turn") {
    FakeClock clock;
    std::vector<std::string> log;
    fe::LiveSplitLink link([&](const std::string &l) { log.push_back(l); },
                           [&] { return clock.at.seconds; });
    // TEST-NET-1 (RFC 5737): no host answers; the connect stays pending or fails at once.
    link.configure(fe::LiveSplitAddress{"192.0.2.1", 16834}, true);
    double slowest = 0;
    for (int i = 0; i < 200; i++) {
        const double before = wall_seconds();
        link.turn(true, fe::RunTime{i * 50, static_cast<std::uint64_t>(i)});
        slowest = std::max(slowest, wall_seconds() - before);
        clock.at.seconds += 0.05;
    }
    CHECK(slowest < 0.05);
    CHECK_FALSE(link.connected());
    CHECK(log.size() == 1); // given up after kLiveSplitConnectSeconds, or refused: one line
}

TEST_CASE("livesplit: off sends nothing and opens no socket") {
    FakeClock clock;
    std::vector<std::string> log;
    fe::LiveSplitLink link([&](const std::string &l) { log.push_back(l); },
                           [&] { return clock.at.seconds; });
    link.configure(std::nullopt, true);
    link.on_signal(fe::RunSignal::kStart, {});
    link.turn(true, fe::RunTime{50, 1});
    CHECK_FALSE(link.connected());
    CHECK(log.empty());
}
