// The LiveSplit link (#407): see livesplit.h.
#include "livesplit.h"

#include <cstdio>
#include <cstring>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#elif !defined(__SWITCH__)
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace maplethief::frontend {

namespace {

bool parse_port(std::string_view text, std::uint16_t *out) {
    if (text.empty() || text.size() > 5) {
        return false;
    }
    std::uint32_t v = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        v = v * 10 + static_cast<std::uint32_t>(c - '0');
    }
    if (v == 0 || v > 65535) {
        return false;
    }
    *out = static_cast<std::uint16_t>(v);
    return true;
}

// Four decimal parts of 0 to 255, no leading zeros.
bool is_ipv4(std::string_view text) {
    int parts = 0;
    std::size_t i = 0;
    while (true) {
        const std::size_t start = i;
        std::uint32_t v = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9' && i - start < 3) {
            v = v * 10 + static_cast<std::uint32_t>(text[i] - '0');
            i++;
        }
        const std::size_t n = i - start;
        if (n == 0 || v > 255 || (n > 1 && text[start] == '0')) {
            return false;
        }
        parts++;
        if (i == text.size()) {
            return parts == 4;
        }
        if (text[i] != '.' || parts == 4) {
            return false;
        }
        i++;
    }
}

// Hex digits, ':' and '.' (an IPv4 tail), with at least two ':'; the socket's inet_pton has the
// last word.
bool looks_ipv6(std::string_view text) {
    int colons = 0;
    for (const char c : text) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (c == ':') {
            colons++;
        } else if (!hex && c != '.') {
            return false;
        }
    }
    return colons >= 2;
}

} // namespace

std::optional<LiveSplitAddress> parse_livesplit_address(std::string_view text) {
    if (text.empty() || text.size() > kMaxLiveSplitAddressBytes) {
        return std::nullopt;
    }
    std::string lower;
    for (const char c : text) {
        lower += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    const std::string_view t = lower;
    LiveSplitAddress a;
    std::string_view host;
    std::string_view port;
    if (t.front() == '[') {
        const std::size_t close = t.find(']');
        if (close == std::string_view::npos) {
            return std::nullopt;
        }
        host = t.substr(1, close - 1);
        const std::string_view rest = t.substr(close + 1);
        if (!rest.empty()) {
            if (rest.front() != ':') {
                return std::nullopt;
            }
            port = rest.substr(1);
            if (port.empty()) {
                return std::nullopt;
            }
        }
        if (!looks_ipv6(host)) {
            return std::nullopt;
        }
    } else {
        const std::size_t colon = t.find(':');
        host = t.substr(0, colon);
        if (colon != std::string_view::npos) {
            port = t.substr(colon + 1);
            if (port.empty()) {
                return std::nullopt;
            }
        }
        if (host != "localhost" && !is_ipv4(host)) {
            return std::nullopt;
        }
    }
    if (!port.empty() && !parse_port(port, &a.port)) {
        return std::nullopt;
    }
    a.host = std::string(host);
    return a;
}

std::string livesplit_address_text(const LiveSplitAddress &a) {
    const bool v6 = a.host.find(':') != std::string::npos;
    return (v6 ? "[" + a.host + "]" : a.host) + ":" + std::to_string(a.port);
}

std::string livesplit_game_time(std::uint64_t ticks) {
    const std::uint64_t ms = ticks * kLiveSplitTickMs;
    char b[48];
    std::snprintf(b, sizeof b, "%llu:%02u:%02u.%03u", static_cast<unsigned long long>(ms / 3600000),
                  static_cast<unsigned>(ms / 60000 % 60), static_cast<unsigned>(ms / 1000 % 60),
                  static_cast<unsigned>(ms % 1000));
    return b;
}

std::vector<std::string> livesplit_lines(RunSignal s, const RunTime &t, bool game_time) {
    switch (s) {
    case RunSignal::kStart:
        if (game_time) {
            return {"starttimer", "pausegametime", "setgametime " + livesplit_game_time(0)};
        }
        return {"starttimer"};
    case RunSignal::kSplit:
        if (game_time) {
            return {"setgametime " + livesplit_game_time(t.ticks), "split"};
        }
        return {"split"};
    case RunSignal::kReset:
        break;
    }
    return {"reset"};
}

// --- The socket ----------------------------------------------------------------------------------

namespace {

#if defined(_WIN32)
using Socket = SOCKET;
const Socket kNoSocket = INVALID_SOCKET;
int last_error() { return WSAGetLastError(); }
bool would_block(int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
void close_handle(Socket s) { closesocket(s); }
std::string error_text(int e) {
    char b[256] = {};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   static_cast<DWORD>(e), 0, b, static_cast<DWORD>(sizeof b), nullptr);
    std::string s = b;
    while (!s.empty() &&
           (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '.')) {
        s.pop_back();
    }
    return s.empty() ? "error " + std::to_string(e) : s;
}
#elif !defined(__SWITCH__)
using Socket = int;
const Socket kNoSocket = -1;
int last_error() { return errno; }
bool would_block(int e) { return e == EINPROGRESS || e == EAGAIN || e == EWOULDBLOCK; }
void close_handle(Socket s) { ::close(s); }
std::string error_text(int e) { return std::strerror(e); }
#endif

#if !defined(__SWITCH__)
Socket as_socket(std::intptr_t s) { return static_cast<Socket>(s); }

#if defined(_WIN32)
bool winsock_ready() {
    static const bool ready = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ready;
}
#endif

// Whether `s` can be written, has failed or has something to read, now. Windows: select, whose
// exception set reports a failed connect (WSAPoll did not before Windows 10 2004).
void poll_socket(Socket s, bool *writable, bool *failed, bool *readable) {
#if defined(_WIN32)
    fd_set w, e, r;
    FD_ZERO(&w);
    FD_ZERO(&e);
    FD_ZERO(&r);
    FD_SET(s, &w);
    FD_SET(s, &e);
    FD_SET(s, &r);
    timeval zero{};
    const int n = select(0, &r, &w, &e, &zero);
    *writable = n > 0 && FD_ISSET(s, &w);
    *failed = n > 0 && FD_ISSET(s, &e);
    *readable = n > 0 && FD_ISSET(s, &r);
#else
    pollfd p{s, POLLIN | POLLOUT, 0};
    const int n = poll(&p, 1, 0);
    *writable = n > 0 && (p.revents & POLLOUT) != 0;
    *failed = n > 0 && (p.revents & (POLLERR | POLLNVAL)) != 0;
    *readable = n > 0 && (p.revents & (POLLIN | POLLHUP)) != 0;
#endif
}

int socket_error(Socket s) {
    int err = 0;
    socklen_t len = static_cast<socklen_t>(sizeof err);
    getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&err), &len);
    return err;
}
#endif

} // namespace

LiveSplitLink::LiveSplitLink(Log log, Now now) : log_(std::move(log)), now_(std::move(now)) {}

LiveSplitLink::~LiveSplitLink() { close_socket(); }

void LiveSplitLink::configure(const std::optional<LiveSplitAddress> &address, bool game_time) {
    game_time_ = game_time;
    if (address == address_) {
        return;
    }
    close_socket();
    address_ = address;
    failing_ = false;
    retry_at_ = 0;
    state_ = address_ ? State::kIdle : State::kOff;
}

void LiveSplitLink::on_signal(RunSignal s, const RunTime &t) {
    if (state_ == State::kOff) {
        return;
    }
    for (const std::string &line : livesplit_lines(s, t, game_time_)) {
        queue(line);
    }
    sent_ticks_ = s == RunSignal::kReset ? std::nullopt : std::optional<std::uint64_t>(t.ticks);
}

void LiveSplitLink::turn(bool running, const RunTime &elapsed) {
    if (state_ != State::kOff && running && game_time_ && sent_ticks_ != elapsed.ticks) {
        queue("setgametime " + livesplit_game_time(elapsed.ticks));
        sent_ticks_ = elapsed.ticks;
    }
    pump();
}

void LiveSplitLink::queue(const std::string &line) {
    if (state_ != State::kConnecting && state_ != State::kConnected) {
        return; // no connection: LiveSplit misses it
    }
    if (out_.size() + line.size() + 1 > kLiveSplitMaxQueueBytes) {
        fail("LiveSplit is not reading");
        return;
    }
    out_ += line;
    out_ += '\n';
}

void LiveSplitLink::fail(const std::string &why) {
    const bool was_connected = state_ == State::kConnected;
    close_socket();
    state_ = State::kIdle;
    retry_at_ = now_() + kLiveSplitRetrySeconds;
    if (!failing_) {
        failing_ = true;
        log_("livesplit: " + std::string(was_connected ? "lost " : "cannot connect to ") +
             (address_ ? livesplit_address_text(*address_) : std::string("?")) + " (" + why +
             "); trying again every " + std::to_string(static_cast<int>(kLiveSplitRetrySeconds)) +
             " s, quietly");
    }
}

void LiveSplitLink::close_socket() {
#if !defined(__SWITCH__)
    if (socket_ != -1) {
        close_handle(as_socket(socket_));
    }
#endif
    socket_ = -1;
    out_.clear();
}

void LiveSplitLink::start_connect() {
#if defined(__SWITCH__)
    fail("no network on this platform");
#else
#if defined(_WIN32)
    if (!winsock_ready()) {
        fail("Winsock did not start");
        return;
    }
#endif
    if (!address_) {
        return;
    }
    const LiveSplitAddress &to = *address_;
    sockaddr_storage addr{};
    socklen_t addr_len = 0;
    const std::string host = to.host == "localhost" ? "127.0.0.1" : to.host;
    auto *v4 = reinterpret_cast<sockaddr_in *>(&addr);
    auto *v6 = reinterpret_cast<sockaddr_in6 *>(&addr);
    if (inet_pton(AF_INET, host.c_str(), &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET;
        v4->sin_port = htons(to.port);
        addr_len = static_cast<socklen_t>(sizeof *v4);
    } else if (inet_pton(AF_INET6, host.c_str(), &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons(to.port);
        addr_len = static_cast<socklen_t>(sizeof *v6);
    } else {
        fail("not an address");
        return;
    }
    const Socket s = socket(addr.ss_family, SOCK_STREAM, IPPROTO_TCP);
    if (s == kNoSocket) {
        fail(error_text(last_error()));
        return;
    }
    socket_ = static_cast<std::intptr_t>(s);
#if defined(_WIN32)
    u_long nonblocking = 1;
    const bool set = ioctlsocket(s, FIONBIO, &nonblocking) == 0;
#else
    const bool set = fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK) == 0;
#ifdef SO_NOSIGPIPE
    const int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
#endif
    if (!set) {
        fail(error_text(last_error()));
        return;
    }
    state_ = State::kConnecting;
    connect_started_ = now_();
    if (connect(s, reinterpret_cast<const sockaddr *>(&addr), addr_len) != 0) {
        const int e = last_error();
        if (!would_block(e)) {
            fail(error_text(e));
        }
    }
#endif
}

void LiveSplitLink::pump() {
    if (state_ == State::kOff) {
        return;
    }
    if (state_ == State::kIdle) {
        if (now_() < retry_at_) {
            return;
        }
        start_connect();
        if (state_ != State::kConnecting) {
            return;
        }
    }
#if !defined(__SWITCH__)
    const Socket s = as_socket(socket_);
    bool writable = false, failed = false, readable = false;
    poll_socket(s, &writable, &failed, &readable);
    if (state_ == State::kConnecting) {
        const int e = (writable || failed) ? socket_error(s) : 0;
        if (failed || e != 0) {
            fail(error_text(e != 0 ? e : last_error()));
            return;
        }
        if (!writable) {
            if (now_() - connect_started_ > kLiveSplitConnectSeconds) {
                fail("no answer");
            }
            return;
        }
        state_ = State::kConnected;
        failing_ = false;
        if (address_) {
            log_("livesplit: connected to " + livesplit_address_text(*address_));
        }
    }
    if (readable) {
        // LiveSplit answers none of these commands; read what comes to notice a close.
        char b[256];
#if defined(_WIN32)
        const int n = recv(s, b, static_cast<int>(sizeof b), 0);
#else
        const ssize_t n = recv(s, b, sizeof b, 0);
#endif
        if (n == 0) {
            fail("LiveSplit closed the connection");
            return;
        }
        if (n < 0 && !would_block(last_error())) {
            fail(error_text(last_error()));
            return;
        }
    }
    while (!out_.empty()) {
#if defined(MSG_NOSIGNAL)
        const int flags = MSG_NOSIGNAL;
#else
        const int flags = 0;
#endif
#if defined(_WIN32)
        const int n = send(s, out_.data(), static_cast<int>(out_.size()), flags);
#else
        const ssize_t n = send(s, out_.data(), out_.size(), flags);
#endif
        if (n < 0) {
            const int e = last_error();
            if (!would_block(e)) {
                fail(error_text(e));
            }
            return;
        }
        out_.erase(0, static_cast<std::size_t>(n));
    }
#endif
}

} // namespace maplethief::frontend
