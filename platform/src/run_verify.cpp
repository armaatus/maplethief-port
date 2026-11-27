// Verifying a shared run file (#408, ADR 0156); see run_verify.h.
#include "run_verify.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <optional>

#include "run_recorder.h"

namespace maplethief::frontend {

namespace {

// The shipped category of `id` in its canonical text (run_category_text), or "".
std::string shipped_category_text(const std::string &id) {
    for (const auto &[name, text] : shipped_run_category_files()) {
        if (name != id) {
            continue;
        }
        std::vector<std::string> problems;
        if (std::optional<RunCategory> c = parse_run_category(name, text, &problems)) {
            return run_category_text(*c);
        }
    }
    return {};
}

std::string split_times(const RunFileSplit &s) {
    return format_run_time_us(s.rta_us) + " " + std::to_string(s.ticks) + " ticks";
}

std::string pad(std::string s, std::size_t width) {
    if (s.size() < width) {
        s.append(width - s.size(), ' ');
    }
    return s;
}

} // namespace

std::string run_file_problem(const RunFile &run) {
    std::uint64_t last_turn = 0;
    std::uint64_t max_tick = 0;
    for (std::size_t i = 0; i < run.checkpoints.size(); i++) {
        const RunCheckpoint &c = run.checkpoints[i];
        if (i != 0 && c.turn <= last_turn) {
            return "checkpoint " + std::to_string(i + 1) + " (turn " + std::to_string(c.turn) +
                   ") is not after the one before (turn " + std::to_string(last_turn) + ")";
        }
        if (c.turn > run.turns) {
            return "checkpoint " + std::to_string(i + 1) + " is at turn " + std::to_string(c.turn) +
                   ", after the recording's last (" + std::to_string(run.turns) + ")";
        }
        last_turn = c.turn;
        max_tick = std::max(max_tick, c.tick);
    }
    for (std::uint64_t t = kRunCheckpointTicks; t <= max_tick; t += kRunCheckpointTicks) {
        bool found = false;
        for (const RunCheckpoint &c : run.checkpoints) {
            found = found || c.tick == t;
        }
        if (!found) {
            return "a checkpoint is missing: none at tick " + std::to_string(t);
        }
    }
    for (const RunFileSplit &s : run.splits) {
        bool found = false;
        for (const RunCheckpoint &c : run.checkpoints) {
            found = found || c.turn == s.turn;
        }
        if (!found) {
            return "a checkpoint is missing: none at the split " + s.name + " (turn " +
                   std::to_string(s.turn) + ")";
        }
    }
    return {};
}

std::string run_unverified_reason(const RunVerification &v) {
    if (!v.refused.empty()) {
        return "refused: " + v.refused;
    }
    if (!v.first_difference.empty()) {
        return "a checkpoint differs at " + v.first_difference;
    }
    if (!v.guest_ok) {
        return "the game stopped during the replay";
    }
    if (!v.reached) {
        return "the replay ended before the recording's last turn";
    }
    if (v.missed != 0) {
        return std::to_string(v.missed) + " checkpoints not reached";
    }
    if (v.replayed.size() != v.recorded.size()) {
        return "the replay reached " + std::to_string(v.replayed.size()) +
               " splits, the file has " + std::to_string(v.recorded.size());
    }
    for (std::size_t i = 0; i < v.recorded.size(); i++) {
        if (!(v.recorded[i] == v.replayed[i])) {
            return "split " + std::to_string(i + 1) + " (" + v.recorded[i].name +
                   ") differs from the file's";
        }
    }
    return {};
}

bool run_verified(const RunVerification &v) { return run_unverified_reason(v).empty(); }

std::string format_run_time_us(std::uint64_t us) {
    const std::uint64_t ms = us / 1000;
    const std::uint64_t h = ms / 3600000, m = ms / 60000 % 60, s = ms / 1000 % 60, f = ms % 1000;
    char text[48];
    if (h != 0) {
        std::snprintf(text, sizeof text, "%" PRIu64 ":%02" PRIu64 ":%02" PRIu64 ".%03" PRIu64, h, m,
                      s, f);
    } else {
        std::snprintf(text, sizeof text, "%" PRIu64 ":%02" PRIu64 ".%03" PRIu64, m, s, f);
    }
    return text;
}

std::string run_verify_report(const RunVerification &v) {
    std::string out = "MapleThief --verify-run " + v.path + "\n";
    if (!v.refused.empty() && v.port_version.empty()) {
        return out + "verdict: NOT VERIFIED (" + run_unverified_reason(v) + ")\n";
    }
    out += "file: recorded by MapleThief " + v.port_version + ", " + std::to_string(v.turns) +
           " turns, " + std::to_string(v.events) + " input events, " +
           std::to_string(v.checkpoints) + " checkpoints\n";
    for (const RunFileCategory &c : v.categories) {
        const std::string shipped = shipped_category_text(c.id);
        out += "category: " + c.id +
               (shipped.empty()     ? " (the file's own)"
                : shipped == c.text ? " (shipped)"
                                    : " (the file's own, differs from the shipped one)") +
               "\n";
    }
    if (v.refused.empty()) {
        std::size_t n = 0;
        for (const RunRecord &r : v.runs) {
            n++;
            out += "run " + std::to_string(n) + ": " + r.category_title + " (" + r.category +
                   "), " + (r.finished ? "finished" : "reset") + ", " +
                   std::to_string(r.times.size()) + " of " + std::to_string(r.splits.size()) +
                   " splits\n";
            if (r.clean()) {
                out += "  clean: every Assist held its neutral value\n";
            } else {
                std::string list;
                for (const std::string &a : r.assists) {
                    list += (list.empty() ? "" : ", ") + a;
                }
                out += "  assisted: " + list + " left the neutral value\n";
            }
            std::string vars;
            for (const auto &[key, values] : r.variables) {
                vars += (vars.empty() ? "" : "; ") + key + " = " + values;
            }
            out += "  run variables: " + vars +
                   (r.in_category ? "" : " (outside the category: never a personal best)") + "\n";
        }
        if (v.runs.empty()) {
            out += "runs: none timed (run_timer was off, or no run started)\n";
        }
    }
    if (!v.recorded.empty() || !v.replayed.empty()) {
        out += "splits:  " + pad("name", 24) + pad("recorded", 26) + "replayed\n";
        const std::size_t rows = std::max(v.recorded.size(), v.replayed.size());
        for (std::size_t i = 0; i < rows; i++) {
            const RunFileSplit *a = i < v.recorded.size() ? &v.recorded[i] : nullptr;
            const RunFileSplit *b = i < v.replayed.size() ? &v.replayed[i] : nullptr;
            const bool same = a != nullptr && b != nullptr && *a == *b;
            const std::string name = a != nullptr ? a->name : b != nullptr ? b->name : "";
            out += "  " + std::to_string(i + 1) + ". " + pad(name, 24) +
                   pad(a != nullptr ? split_times(*a) : "-", 26) +
                   (b != nullptr ? split_times(*b) : "-") + (same ? "  same" : "  DIFFERS") + "\n";
        }
    }
    if (v.refused.empty()) {
        out += "checkpoints: " + std::to_string(v.compared) + " of " +
               std::to_string(v.checkpoints) + " compared, " +
               (v.first_difference.empty() ? std::string("all the same")
                                           : "first difference at " + v.first_difference) +
               "\n";
    }
    const std::string why = run_unverified_reason(v);
    out += why.empty() ? "verdict: VERIFIED\n" : "verdict: NOT VERIFIED (" + why + ")\n";
    return out;
}

} // namespace maplethief::frontend
