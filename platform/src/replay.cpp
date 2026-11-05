// Input replay and checkpoints (#91, ADR 0104); see replay.h.
#include "replay.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <sstream>

namespace maplethief::frontend {

namespace {

// SDL3's gamepad names (SDL_GetGamepadStringForButton / ForAxis), by SDL_GamepadButton and
// SDL_GamepadAxis: the virtual pad's 15 buttons and 6 axes (platform_test checks them against
// SDL's).
constexpr const char *kButtons[] = {"a",
                                    "b",
                                    "x",
                                    "y",
                                    "back",
                                    "guide",
                                    "start",
                                    "leftstick",
                                    "rightstick",
                                    "leftshoulder",
                                    "rightshoulder",
                                    "dpup",
                                    "dpdown",
                                    "dpleft",
                                    "dpright"};
constexpr const char *kAxes[] = {"leftx",  "lefty",       "rightx",
                                 "righty", "lefttrigger", "righttrigger"};
constexpr int kButtonCount = static_cast<int>(sizeof kButtons / sizeof kButtons[0]);
constexpr int kAxisCount = static_cast<int>(sizeof kAxes / sizeof kAxes[0]);

// The pad kinds a replay can attach (the driver's virtual pads).
constexpr const char *kPadKinds[] = {"xbox360"};

std::vector<std::string> words(const std::string &line) {
    std::vector<std::string> out;
    std::istringstream in(line);
    std::string w;
    while (in >> w) {
        out.push_back(w);
    }
    return out;
}

// A decimal number in [lo, hi], nothing else in `s`.
bool to_int(const std::string &s, long long lo, long long hi, long long *out) {
    if (s.empty() || s.size() > 20) {
        return false;
    }
    errno = 0;
    char *end = nullptr;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    if (errno != 0 || end != s.c_str() + s.size() || v < lo || v > hi) {
        return false;
    }
    *out = v;
    return true;
}
bool to_u64(const std::string &s, std::uint64_t hi, std::uint64_t *out) {
    if (s.empty() || s.size() > 20 ||
        !std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        return false;
    }
    errno = 0;
    char *end = nullptr;
    const unsigned long long v = std::strtoull(s.c_str(), &end, 10);
    if (errno != 0 || end != s.c_str() + s.size() || v > hi) {
        return false;
    }
    *out = v;
    return true;
}
bool to_hex64(const std::string &s, std::uint64_t *out) {
    if (s.size() != 16) {
        return false;
    }
    std::uint64_t v = 0;
    for (const char c : s) {
        int d = -1;
        if (c >= '0' && c <= '9') {
            d = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            d = c - 'a' + 10;
        }
        if (d < 0) {
            return false;
        }
        v = v << 4 | static_cast<std::uint64_t>(d);
    }
    *out = v;
    return true;
}

// A name of a check: 1 to 64 of [A-Za-z0-9_-].
bool valid_name(const std::string &s) {
    return !s.empty() && s.size() <= 64 && std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-';
    });
}
// A state key or value: no '=', '|' or space.
bool valid_fact(const std::string &s) {
    return !s.empty() && s.size() <= 64 && s.find_first_of("=|<> \t\r\n") == std::string::npos;
}

bool is_number(const std::string &s, long long *v) { return to_int(s, LLONG_MIN, LLONG_MAX, v); }

// The largest step a file may name: 10^12 steps (31 years at 1 ms).
constexpr std::uint64_t kMaxStep = 1000000000000ull;

} // namespace

int replay_button_from_name(const std::string &name) {
    for (int i = 0; i < kButtonCount; i++) {
        if (name == kButtons[i]) {
            return i;
        }
    }
    return -1;
}
int replay_axis_from_name(const std::string &name) {
    for (int i = 0; i < kAxisCount; i++) {
        if (name == kAxes[i]) {
            return i;
        }
    }
    return -1;
}
const char *replay_button_name(int button) {
    return button >= 0 && button < kButtonCount ? kButtons[button] : "";
}
const char *replay_axis_name(int axis) { return axis >= 0 && axis < kAxisCount ? kAxes[axis] : ""; }

std::uint64_t Replay::end_step() const {
    for (const ReplayEvent &e : events) {
        if (e.kind == ReplayKind::kEnd) {
            return e.step;
        }
    }
    return events.empty() ? 0 : events.back().step;
}

bool Replay::add(const ReplayEvent &e) {
    if (!events.empty() &&
        (e.step < events.back().step || events.back().kind == ReplayKind::kEnd)) {
        return false;
    }
    events.push_back(e);
    return true;
}

bool parse_replay(const std::string &text, Replay *out, std::string *error) {
    Replay r;
    std::istringstream in(text);
    std::string line;
    int n = 0;
    bool header = false;
    std::vector<std::string> checks;
    auto fail = [&](const std::string &why) {
        if (error != nullptr) {
            *error = "line " + std::to_string(n) + ": " + why;
        }
        return false;
    };
    while (std::getline(in, line)) {
        n++;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::vector<std::string> w = words(line);
        if (w.empty() || w[0][0] == '#') {
            continue;
        }
        if (!header) {
            long long v = 0;
            if (w.size() != 2 || w[0] != "maplethief-replay") {
                return fail("not a replay (want 'maplethief-replay " +
                            std::to_string(kReplayVersion) + "')");
            }
            if (!to_int(w[1], 1, 1000, &v) || v != kReplayVersion) {
                return fail("version " + w[1] + " (this build reads " +
                            std::to_string(kReplayVersion) + ")");
            }
            header = true;
            continue;
        }
        if (w[0] == "step_us" || w[0] == "every") {
            std::uint64_t v = 0;
            if (w.size() != 2 || !to_u64(w[1], w[0] == "step_us" ? 1000000 : kMaxStep, &v) ||
                (w[0] == "step_us" && v == 0)) {
                return fail(w[0] + " needs a number" +
                            (w[0] == "step_us" ? " from 1 to 1000000" : ""));
            }
            (w[0] == "step_us" ? r.step_us : r.every) = v;
            continue;
        }
        if (w[0] == "data") {
            if (w.size() != 2 || w[1] != "empty") {
                return fail("data: only 'empty' (a fresh install) is known");
            }
            r.data = w[1];
            continue;
        }
        if (w[0] == "expect") {
            if (w.size() < 3) {
                return fail("expect needs a check and at least one fact");
            }
            if (std::find(checks.begin(), checks.end(), w[1]) == checks.end()) {
                return fail("expect on check '" + w[1] + "', which is not before it");
            }
            for (std::size_t i = 2; i < w.size(); i++) {
                ReplayExpect e;
                e.check = w[1];
                std::size_t at = w[i].find(">=");
                e.op = ">=";
                if (at == std::string::npos) {
                    at = w[i].find("<=");
                    e.op = "<=";
                }
                if (at == std::string::npos) {
                    at = w[i].find('=');
                    e.op = "=";
                }
                if (at == std::string::npos) {
                    return fail("a fact is key=value, key>=N or key<=N: '" + w[i] + "'");
                }
                e.key = w[i].substr(0, at);
                e.value = w[i].substr(at + e.op.size());
                long long v = 0;
                if (!valid_fact(e.key) || !valid_fact(e.value) ||
                    (e.op != "=" && !is_number(e.value, &v))) {
                    return fail("a bad fact '" + w[i] + "'");
                }
                r.expects.push_back(e);
            }
            continue;
        }
        // STEP EVENT ...
        ReplayEvent e;
        if (!to_u64(w[0], kMaxStep, &e.step)) {
            return fail("'" + w[0] + "' is not a step, a header line or an expect");
        }
        if (w.size() < 2) {
            return fail("a step without an event");
        }
        const std::string &what = w[1];
        if (what == "pad" && w.size() == 4 && w[2] == "attach") {
            if (std::find_if(std::begin(kPadKinds), std::end(kPadKinds),
                             [&](const char *k) { return w[3] == k; }) == std::end(kPadKinds)) {
                return fail("an unknown pad kind '" + w[3] + "'");
            }
            e.kind = ReplayKind::kPadAttach;
            e.name = w[3];
        } else if (what == "pad" && w.size() == 5 && w[2] == "button") {
            e.kind = ReplayKind::kPadButton;
            e.index = replay_button_from_name(w[3]);
            if (e.index < 0 || (w[4] != "down" && w[4] != "up")) {
                return fail("pad button NAME down|up: '" + w[3] + " " + w[4] + "'");
            }
            e.value = w[4] == "down" ? 1 : 0;
        } else if (what == "pad" && w.size() == 5 && w[2] == "axis") {
            e.kind = ReplayKind::kPadAxis;
            e.index = replay_axis_from_name(w[3]);
            long long v = 0;
            if (e.index < 0 || !to_int(w[4], -32768, 32767, &v)) {
                return fail("pad axis NAME -32768..32767: '" + w[3] + " " + w[4] + "'");
            }
            e.value = static_cast<int>(v);
        } else if (what == "check" && w.size() == 3) {
            if (!valid_name(w[2])) {
                return fail("a check's name is 1 to 64 of [A-Za-z0-9_-]: '" + w[2] + "'");
            }
            if (std::find(checks.begin(), checks.end(), w[2]) != checks.end()) {
                return fail("check '" + w[2] + "' twice");
            }
            checks.push_back(w[2]);
            e.kind = ReplayKind::kCheck;
            e.name = w[2];
        } else if (what == "end" && w.size() == 2) {
            e.kind = ReplayKind::kEnd;
        } else {
            return fail("an unknown event '" + line + "'");
        }
        if (!r.events.empty() && r.events.back().kind == ReplayKind::kEnd) {
            return fail("an event after the end");
        }
        if (!r.add(e)) {
            return fail("step " + w[0] + " is before the step of the line before it");
        }
    }
    if (!header) {
        n = 0;
        return fail("empty: no 'maplethief-replay' header");
    }
    *out = std::move(r);
    return true;
}

std::string format_replay(const Replay &r, const std::vector<std::string> &comment) {
    std::ostringstream o;
    o << "maplethief-replay " << kReplayVersion << "\n";
    for (const std::string &c : comment) {
        o << "# " << c << "\n";
    }
    o << "step_us " << r.step_us << "\n";
    if (r.every != 0) {
        o << "every " << r.every << "\n";
    }
    o << "data " << r.data << "\n";
    for (const ReplayEvent &e : r.events) {
        o << e.step << " ";
        switch (e.kind) {
        case ReplayKind::kPadAttach:
            o << "pad attach " << e.name;
            break;
        case ReplayKind::kPadButton:
            o << "pad button " << replay_button_name(e.index) << (e.value != 0 ? " down" : " up");
            break;
        case ReplayKind::kPadAxis:
            o << "pad axis " << replay_axis_name(e.index) << " " << e.value;
            break;
        case ReplayKind::kCheck:
            o << "check " << e.name;
            break;
        case ReplayKind::kEnd:
            o << "end";
            break;
        }
        o << "\n";
    }
    // The expects, a line per check, in the order of the checks.
    std::vector<std::string> order;
    for (const ReplayExpect &e : r.expects) {
        if (std::find(order.begin(), order.end(), e.check) == order.end()) {
            order.push_back(e.check);
        }
    }
    for (const std::string &check : order) {
        o << "expect " << check;
        for (const ReplayExpect &e : r.expects) {
            if (e.check == check) {
                o << " " << e.key << e.op << e.value;
            }
        }
        o << "\n";
    }
    return o.str();
}

const ReplayEvent *ReplayCursor::next(std::uint64_t step) {
    while (next_ < replay_->events.size()) {
        const ReplayEvent &e = replay_->events[next_];
        if (e.step > step) {
            return nullptr;
        }
        next_++;
        if (e.kind == ReplayKind::kPadAttach || e.kind == ReplayKind::kPadButton ||
            e.kind == ReplayKind::kPadAxis) {
            return &e;
        }
    }
    return nullptr;
}

bool ReplayCursor::finished() const { return next_ >= replay_->events.size(); }

// --- checkpoints --------------------------------------------------------------------------------

std::uint64_t fnv1a64(const void *data, std::size_t n, std::uint64_t h) {
    const auto *p = static_cast<const std::uint8_t *>(data);
    for (std::size_t i = 0; i < n; i++) {
        h = (h ^ p[i]) * 1099511628211ull;
    }
    return h;
}

std::uint64_t frame_hash(const std::uint8_t *rgba, std::uint32_t w, std::uint32_t h) {
    const std::uint8_t size[8] = {
        static_cast<std::uint8_t>(w),       static_cast<std::uint8_t>(w >> 8),
        static_cast<std::uint8_t>(w >> 16), static_cast<std::uint8_t>(w >> 24),
        static_cast<std::uint8_t>(h),       static_cast<std::uint8_t>(h >> 8),
        static_cast<std::uint8_t>(h >> 16), static_cast<std::uint8_t>(h >> 24)};
    return fnv1a64(rgba, std::size_t{w} * h * 4, fnv1a64(size, sizeof size));
}

std::string hex64(std::uint64_t v) {
    static const char digits[] = "0123456789abcdef";
    std::string s(16, '0');
    for (int i = 15; i >= 0; i--) {
        s[static_cast<std::size_t>(i)] = digits[v & 15];
        v >>= 4;
    }
    return s;
}

std::string format_checkpoint(const Checkpoint &c) {
    return c.name + " step " + std::to_string(c.step) + " frame " + hex64(c.frame) + " audio " +
           hex64(c.audio) + "/" + std::to_string(c.audio_frames) + " gl " +
           std::to_string(c.gl_frame) + " | " + c.state;
}

bool parse_checkpoint(const std::string &line, Checkpoint *out, std::string *error) {
    auto fail = [&](const std::string &why) {
        if (error != nullptr) {
            *error = why + ": '" + line + "'";
        }
        return false;
    };
    const std::size_t bar = line.find(" | ");
    if (bar == std::string::npos) {
        return fail("no ' | ' before the state");
    }
    const std::vector<std::string> w = words(line.substr(0, bar));
    Checkpoint c;
    std::uint64_t gl = 0;
    if (w.size() != 9 || w[1] != "step" || w[3] != "frame" || w[5] != "audio" || w[7] != "gl") {
        return fail("not 'NAME step N frame HEX audio HEX/N gl N'");
    }
    const std::size_t slash = w[6].find('/');
    if (!valid_name(w[0]) || !to_u64(w[2], UINT64_MAX, &c.step) || !to_hex64(w[4], &c.frame) ||
        slash == std::string::npos || !to_hex64(w[6].substr(0, slash), &c.audio) ||
        !to_u64(w[6].substr(slash + 1), UINT64_MAX, &c.audio_frames) ||
        !to_u64(w[8], UINT32_MAX, &gl)) {
        return fail("a bad name or number");
    }
    c.name = w[0];
    c.gl_frame = static_cast<std::uint32_t>(gl);
    c.state = line.substr(bar + 3);
    std::map<std::string, std::string> facts;
    if (!parse_state(c.state, &facts)) {
        return fail("a bad state");
    }
    *out = c;
    return true;
}

bool parse_state(const std::string &state, std::map<std::string, std::string> *out) {
    std::map<std::string, std::string> facts;
    for (const std::string &w : words(state)) {
        const std::size_t eq = w.find('=');
        if (eq == std::string::npos || !valid_fact(w.substr(0, eq)) ||
            !valid_fact(w.substr(eq + 1))) {
            return false;
        }
        facts[w.substr(0, eq)] = w.substr(eq + 1);
    }
    *out = std::move(facts);
    return true;
}

bool expect_holds(const ReplayExpect &e, const std::map<std::string, std::string> &state,
                  std::string *why) {
    const auto it = state.find(e.key);
    if (it == state.end()) {
        if (why != nullptr) {
            *why = e.key + " is not in the state";
        }
        return false;
    }
    bool ok = false;
    long long have = 0;
    long long want = 0;
    if (e.op == "=") {
        ok = it->second == e.value;
    } else if (is_number(it->second, &have) && is_number(e.value, &want)) {
        ok = e.op == ">=" ? have >= want : have <= want;
    }
    if (why != nullptr) {
        *why = e.key + "=" + it->second;
    }
    return ok;
}

const Checkpoint *CheckpointLog::find(const std::string &name) const {
    for (const Checkpoint &c : points_) {
        if (c.name == name) {
            return &c;
        }
    }
    return nullptr;
}

std::string CheckpointLog::text() const {
    std::string s;
    for (const Checkpoint &c : points_) {
        s += format_checkpoint(c) + "\n";
    }
    return s;
}

std::string CheckpointLog::digest() const {
    const std::string t = text();
    return hex64(fnv1a64(t.data(), t.size()));
}

bool CheckpointLog::parse(const std::string &text, CheckpointLog *out, std::string *error) {
    CheckpointLog log;
    std::istringstream in(text);
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        n++;
        if (line.empty()) {
            continue;
        }
        Checkpoint c;
        std::string why;
        if (!parse_checkpoint(line, &c, &why)) {
            if (error != nullptr) {
                *error = "line " + std::to_string(n) + ": " + why;
            }
            return false;
        }
        log.add(c);
    }
    *out = std::move(log);
    return true;
}

LogDifference compare_logs(const CheckpointLog &a, const CheckpointLog &b) {
    LogDifference d;
    const std::vector<Checkpoint> &pa = a.points();
    const std::vector<Checkpoint> &pb = b.points();
    const std::size_t n = std::max(pa.size(), pb.size());
    for (std::size_t i = 0; i < n; i++) {
        const std::string la = i < pa.size() ? format_checkpoint(pa[i]) : "";
        const std::string lb = i < pb.size() ? format_checkpoint(pb[i]) : "";
        if (la != lb) {
            d.same = false;
            d.index = i;
            d.a = la;
            d.b = lb;
            return d;
        }
    }
    d.index = n;
    return d;
}

CheckpointPlan::CheckpointPlan(const Replay &replay) : every_(replay.every) {
    bool ended = false;
    for (const ReplayEvent &e : replay.events) {
        if (e.kind == ReplayKind::kCheck) {
            add(e.step, e.name);
        }
        ended = ended || e.kind == ReplayKind::kEnd;
    }
    if (ended) {
        end_ = replay.end_step();
    }
}

void CheckpointPlan::add(std::uint64_t step, const std::string &name) {
    named_.emplace(step, name);
}

std::vector<std::string> CheckpointPlan::due(std::uint64_t step) const {
    std::vector<std::string> out;
    if (step > end_) {
        return out;
    }
    if (every_ != 0 && step != 0 && step % every_ == 0) {
        out.push_back("s" + std::to_string(step));
    }
    const auto range = named_.equal_range(step);
    for (auto it = range.first; it != range.second; ++it) {
        out.push_back(it->second);
    }
    return out;
}

} // namespace maplethief::frontend
