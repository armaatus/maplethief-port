// Recording a run and playing it back (#405, ADR 0156); see run_recorder.h.
#include "run_recorder.h"

#include <SDL3/SDL_log.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "autosave.h"
#include "maplethief/input.h"
#include "maplethief/memory.h"
#include "maplethief/runtape.h"
#include "maplethief/saves.h"
#include "maplethief/snapshot.h"

namespace maplethief::frontend {

namespace fs = std::filesystem;

namespace {

// CMTimer's lastFrame, the i64 the game sets before every tick (docs/re/timing-threading.md).
constexpr std::uint32_t kLastFrame = 0x11A4D8;
// The local Game Center's file beside the saves (local_game_center.h).
constexpr const char *kGameCenterFile = "gamecenter.json";

RunRecorder *g_recorder = nullptr;

fs::path utf8_path(const std::string &s) { return fs::path(std::u8string(s.begin(), s.end())); }

// Whether [addr, addr + 8) lies in one of the image's segments (a test's guest may map none).
bool image_mapped(const mt_cpu *cpu, std::uint32_t addr) {
    if (cpu == nullptr || cpu->mem == nullptr || std::uint64_t{addr} + 8 > cpu->mem_size) {
        return false;
    }
    const mt_mem *mem = mt_mem_from_cpu(cpu);
    if (mem == nullptr) {
        return false;
    }
    std::uint32_t n = 0;
    const mt_segment *segs = mt_mem_segments(mem, &n);
    for (std::uint32_t i = 0; i < n; i++) {
        if (addr >= segs[i].vmaddr &&
            addr + 8ull <= std::uint64_t{segs[i].vmaddr} + segs[i].vmsize) {
            return true;
        }
    }
    return false;
}

std::string hex64(std::uint64_t v) {
    char text[24];
    std::snprintf(text, sizeof text, "%016" PRIx64, v);
    return text;
}

} // namespace

bool TickCounter::update(const mt_cpu *cpu) {
    if (!image_mapped(cpu, kLastFrame)) {
        return false;
    }
    std::int64_t v = 0;
    std::memcpy(&v, cpu->mem + kLastFrame, 8);
    const bool ticked = seen_ && v != last_;
    seen_ = true;
    last_ = v;
    ticks_ += ticked ? 1 : 0;
    return ticked;
}

std::string run_state_line(const mt_cpu *cpu, std::uint64_t ticks) {
    const AutosaveCheck at = check_autosave(cpu);
    char text[160];
    std::snprintf(text, sizeof text, "ticks=%" PRIu64 " mode=%d map=%d.%d popup=%d panel=%d", ticks,
                  static_cast<int>(at.mode), static_cast<int>(at.area), static_cast<int>(at.map),
                  static_cast<int>(at.popup), static_cast<int>(mt_input_panel()));
    return text;
}

std::vector<RunBootFile> read_boot_files(const std::string &saves_dir) {
    std::vector<RunBootFile> out;
    std::error_code ec;
    for (const fs::directory_entry &e : fs::directory_iterator(utf8_path(saves_dir), ec)) {
        const std::u8string u = e.path().filename().u8string();
        const std::string name(u.begin(), u.end());
        if (!e.is_regular_file(ec) ||
            (mt_saves_is_game_file(name.c_str()) == 0 && name != kGameCenterFile)) {
            continue;
        }
        std::ifstream in(e.path(), std::ios::binary);
        RunBootFile f;
        f.name = name;
        f.bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (in.bad() || f.bytes.size() > kRunMaxFileBytes || out.size() >= kRunMaxFiles) {
            continue;
        }
        out.push_back(std::move(f));
    }
    std::sort(out.begin(), out.end(),
              [](const RunBootFile &a, const RunBootFile &b) { return a.name < b.name; });
    return out;
}

std::string prepare_run_folder(const std::string &path, RunFile *run, std::string *why) {
    std::ifstream in(utf8_path(path), std::ios::binary);
    if (!in) {
        *why = "cannot open the file";
        return {};
    }
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                          std::istreambuf_iterator<char>());
    if (in.bad()) {
        *why = "cannot read the file";
        return {};
    }
    if (!read_run_file(bytes.data(), bytes.size(), run, why)) {
        return {};
    }
    std::error_code ec;
    const fs::path tmp = fs::temp_directory_path(ec);
    if (ec) {
        *why = "no temporary folder: " + ec.message();
        return {};
    }
    fs::path dir;
    for (int i = 0; i < 1000; i++) {
        dir = tmp / ("maplethief-play-run-" + std::to_string(std::time(nullptr)) + "-" +
                     std::to_string(i));
        if (fs::create_directory(dir, ec) && !ec) {
            break;
        }
        dir.clear();
    }
    if (dir.empty() || !fs::create_directories(dir / "saves", ec)) {
        *why = "cannot make a scratch data folder in " + tmp.string();
        return {};
    }
    for (const RunBootFile &f : run->files) {
        std::ofstream out(dir / "saves" / utf8_path(f.name), std::ios::binary);
        out.write(reinterpret_cast<const char *>(f.bytes.data()),
                  static_cast<std::streamsize>(f.bytes.size()));
        if (!out) {
            *why = "cannot write " + f.name + " into the scratch data folder";
            return {};
        }
    }
    // The run categories the session timed (#408): a player's own replaces a shipped one of its
    // name, as in the recording's data folder.
    if (!run->categories.empty() && !fs::create_directories(dir / "categories", ec)) {
        *why = "cannot make the scratch data folder's categories/";
        return {};
    }
    for (const RunFileCategory &c : run->categories) {
        std::ofstream out(dir / "categories" / utf8_path(c.id + ".mtcat"), std::ios::binary);
        out.write(c.text.data(), static_cast<std::streamsize>(c.text.size()));
        if (!out) {
            *why = "cannot write the category " + c.id + " into the scratch data folder";
            return {};
        }
    }
    const std::u8string u = dir.u8string();
    return std::string(u.begin(), u.end());
}

std::string run_binding_mismatch(const RunFile &run, const std::array<std::uint8_t, 32> &sha) {
    if (run.binary_sha256 != sha) {
        return "it was recorded on another game binary (the armv7 slice's SHA-256 differs)";
    }
    if (run.port_version != MT_VERSION) {
        return "it was recorded by MapleThief " + run.port_version + ", this is " + MT_VERSION +
               " (a port update may change timing: play it with the version that made it)";
    }
    return {};
}

bool RunRecorder::start(const std::array<std::uint8_t, 32> &binary_sha256, const Settings &settings,
                        const std::string &saves_dir) {
    run_ = RunFile{};
    run_.binary_sha256 = binary_sha256;
    run_.port_version = MT_VERSION;
    run_.settings = serialise_settings(settings);
    run_.files = read_boot_files(saves_dir);
    last_ = settings;
    if (mt_runtape_record() != 0) {
        SDL_Log("runs: record_runs: the clocks could not be wrapped (GCD runs already); this "
                "session is not recorded");
        return false;
    }
    mode_ = Mode::kRecording;
    SDL_Log("runs: recording this session (record_runs = on; %zu game files bound)",
            run_.files.size());
    return true;
}

bool RunRecorder::play(const RunFile &run, std::string *why) {
    run_ = run;
    const mt_runtape_run *runs[MT_RUNTAPE_STREAMS];
    std::size_t counts[MT_RUNTAPE_STREAMS];
    for (std::uint32_t s = 0; s < MT_RUNTAPE_STREAMS; s++) {
        runs[s] = run_.streams[s].data();
        counts[s] = run_.streams[s].size();
    }
    if (mt_runtape_play(runs, counts, run_.events.data(), run_.events.size()) != 0) {
        *why = "the tape's events are out of order, or GCD runs already";
        return false;
    }
    mode_ = Mode::kPlaying;
    return true;
}

std::vector<const SettingInfo *> RunRecorder::turn_begin(Settings *settings) {
    std::vector<const SettingInfo *> rows;
    if (mode_ != Mode::kPlaying) {
        return rows;
    }
    const std::uint64_t turn = mt_runtape_get_stats().turn;
    while (next_change_ < run_.changes.size() && run_.changes[next_change_].turn <= turn) {
        const RunSettingChange &c = run_.changes[next_change_++];
        std::string why;
        if (!apply_setting(settings, c.key, c.value, &why)) {
            SDL_Log("runs: the file's change %s = %s at turn %" PRIu64 " does not apply: %s",
                    c.key.c_str(), c.value.c_str(), c.turn, why.c_str());
            continue;
        }
        if (const SettingInfo *row = find_setting(c.key)) {
            rows.push_back(row);
        }
    }
    return rows;
}

void RunRecorder::seal() {
    if (sealed_) {
        return;
    }
    sealed_ = true;
    run_.turns = complete_turn_;
    for (std::uint32_t s = 0; s < MT_RUNTAPE_STREAMS; s++) {
        std::size_t n = 0;
        const mt_runtape_run *runs = mt_runtape_runs(s, &n);
        run_.streams[s].assign(runs, runs + n);
    }
    std::size_t n = 0;
    const mt_runtape_event *events = mt_runtape_events(&n);
    run_.events.assign(events, events + n);
    // The events of the turn that did not complete never play: the replay ends before it.
    while (!run_.events.empty() && run_.events.back().turn > run_.turns) {
        run_.events.pop_back();
    }
    mt_runtape_stop();
}

RunCheckpoint RunRecorder::take_checkpoint(const mt_cpu *cpu) const {
    RunCheckpoint c;
    c.turn = mt_runtape_get_stats().turn;
    c.tick = ticks_.ticks();
    c.digest = mt_snapshot_memory_digest(const_cast<mt_cpu *>(cpu));
    c.state = run_state_line(cpu, c.tick);
    return c;
}

void RunRecorder::checkpoint(const mt_cpu *cpu) {
    const std::uint64_t turn = mt_runtape_get_stats().turn;
    if (!run_.checkpoints.empty() && run_.checkpoints.back().turn == turn) {
        return; // one a turn: a tick's, a split's and the end's at one turn are one checkpoint
    }
    run_.checkpoints.push_back(take_checkpoint(cpu));
}

void RunRecorder::differ(const std::string &what) {
    if (first_difference_.empty()) {
        first_difference_ = what;
    }
}

void RunRecorder::compare(const mt_cpu *cpu, bool due) {
    const std::uint64_t turn = mt_runtape_get_stats().turn;
    while (next_compare_ < run_.checkpoints.size() && run_.checkpoints[next_compare_].turn < turn) {
        next_compare_++; // a turn the replay never ran (cannot happen: turns go one by one)
    }
    if (next_compare_ >= run_.checkpoints.size() || run_.checkpoints[next_compare_].turn != turn) {
        if (due) {
            const RunCheckpoint c = take_checkpoint(cpu);
            SDL_Log("runs: the replay needs a checkpoint at turn %" PRIu64 " (tick %" PRIu64
                    "), the file has none: %s",
                    turn, c.tick, c.state.c_str());
            differ("turn " + std::to_string(turn) + " (tick " + std::to_string(c.tick) +
                   "): the replay takes a checkpoint (" + c.state + ") where the file has none");
        }
        return;
    }
    const RunCheckpoint &want = run_.checkpoints[next_compare_++];
    const RunCheckpoint c = take_checkpoint(cpu);
    compared_++;
    const bool same = want.digest == c.digest && want.state == c.state && want.tick == c.tick;
    SDL_Log("runs: checkpoint at turn %" PRIu64 " (tick %" PRIu64 "): %s  digest %s  %s", c.turn,
            c.tick, same ? "same" : "DIFFERS", hex64(c.digest).c_str(), c.state.c_str());
    if (!same) {
        SDL_Log("runs: recorded: tick %" PRIu64 " digest %s  %s", want.tick,
                hex64(want.digest).c_str(), want.state.c_str());
        differ("turn " + std::to_string(c.turn) + ": recorded tick " + std::to_string(want.tick) +
               " digest " + hex64(want.digest) + " " + want.state + "; replayed tick " +
               std::to_string(c.tick) + " digest " + hex64(c.digest) + " " + c.state);
    }
}

void RunRecorder::split(const std::string &name, std::uint64_t rta_us, std::uint64_t ticks) {
    if (mode_ == Mode::kOff || sealed_) {
        return;
    }
    RunFileSplit s{name, mt_runtape_get_stats().turn, rta_us, ticks};
    SDL_Log("runs: split %s at turn %" PRIu64 ": %.3f s, %" PRIu64 " ticks", name.c_str(), s.turn,
            static_cast<double>(rta_us) / 1e6, ticks);
    (mode_ == Mode::kRecording ? run_.splits : replayed_splits_).push_back(std::move(s));
    split_due_ = true;
}

void RunRecorder::category(const std::string &id, const std::string &text) {
    if (mode_ != Mode::kRecording || sealed_) {
        return;
    }
    const RunFileCategory c{id, text};
    if (std::find(run_.categories.begin(), run_.categories.end(), c) == run_.categories.end() &&
        run_.categories.size() < kRunMaxCategories && text.size() <= kRunMaxCategoryText) {
        run_.categories.push_back(c);
    }
}

void RunRecorder::turn_end(const mt_cpu *cpu, const Settings &settings) {
    if (mode_ == Mode::kOff || sealed_) {
        return;
    }
    const std::uint64_t turn = mt_runtape_get_stats().turn;
    bool due = split_due_;
    split_due_ = false;
    if (ticks_.update(cpu) && ticks_.ticks() >= next_checkpoint_) {
        due = true;
        next_checkpoint_ += kRunCheckpointTicks;
    }
    complete_turn_ = turn;
    if (mode_ != Mode::kRecording) {
        compare(cpu, due); // and at the recording's last turn, its own when it ended on one
        return;
    }
    if (due) {
        checkpoint(cpu);
    }
    if (!(settings == last_)) {
        for (const SettingInfo &row : settings_registry()) {
            const std::string now = setting_text(settings, row);
            if (now != setting_text(last_, row)) {
                run_.changes.push_back({turn, row.key, now});
            }
        }
        last_ = settings;
    }
}

std::string RunRecorder::finish(RunEnd end, const std::string &detail, const std::string &runs_dir,
                                const mt_cpu *cpu) {
    if (mode_ == Mode::kOff) {
        return {};
    }
    const Mode was = mode_;
    if (was != Mode::kRecording) {
        mt_runtape_stop();
        mode_ = Mode::kOff;
        return {};
    }
    // The last checkpoint, when the session ended on a whole turn (--seconds, a window that
    // closed between turns); a quit ends inside its turn (the delegate hears it terminates), so
    // the replay ends with the turn before.
    if (!sealed_ && cpu != nullptr && complete_turn_ == mt_runtape_get_stats().turn &&
        complete_turn_ != 0) {
        checkpoint(cpu);
    }
    seal();
    mode_ = Mode::kOff;
    if (!refused_.empty()) {
        end = RunEnd::kRefused;
    }
    run_.end = end;
    run_.end_detail = !refused_.empty() ? refused_ : detail;
    const std::vector<std::uint8_t> bytes = write_run_file(run_);
    std::error_code ec;
    fs::create_directories(utf8_path(runs_dir), ec);
    const fs::path path =
        utf8_path(runs_dir) / run_file_name(static_cast<std::int64_t>(std::time(nullptr)));
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    out.close();
    const std::u8string u = path.u8string();
    const std::string p(u.begin(), u.end());
    if (bytes.empty() || !out) {
        SDL_Log("runs: the run file %s could not be written", p.c_str());
        return {};
    }
    SDL_Log("runs: wrote %s: %zu bytes, %" PRIu64 " turns, %zu events, %zu checkpoints, %.1f s "
            "of guest time (%s%s%s)",
            p.c_str(), bytes.size(), run_.turns, run_.events.size(), run_.checkpoints.size(),
            static_cast<double>(run_tape_span_us(run_)) / 1e6, run_end_name(run_.end),
            run_.end_detail.empty() ? "" : ": ", run_.end_detail.c_str());
    return p;
}

bool RunRecorder::done() const {
    return mode_ == Mode::kPlaying && mt_runtape_get_stats().turn >= run_.turns;
}

std::uint64_t RunRecorder::missed() const { return run_.checkpoints.size() - next_compare_; }

void RunRecorder::refuse(const char *what) {
    if (mode_ != Mode::kRecording || !refused_.empty()) {
        return;
    }
    refused_ = what;
    SDL_Log("runs: %s cannot be recorded: the recording ends with turn %" PRIu64, what,
            complete_turn_);
    seal();
}

void set_run_recorder(RunRecorder *recorder) { g_recorder = recorder; }

void run_refuse(const char *what) {
    if (g_recorder != nullptr) {
        g_recorder->refuse(what);
    }
}

} // namespace maplethief::frontend
