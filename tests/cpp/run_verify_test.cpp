// --verify-run's checks and report (#408, ADR 0156, run_verify.h): synthetic run files refused
// for another binary hash, another port version, a flipped byte and a missing checkpoint, and the
// report's format. The replay itself needs the game: it is checked locally (ADR 0156's addendum).
#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "run_file.h"
#include "run_recorder.h"
#include "run_verify.h"

namespace {

namespace fe = maplethief::frontend;

std::array<std::uint8_t, 32> sha_of(std::uint8_t seed) {
    std::array<std::uint8_t, 32> sha{};
    for (std::size_t i = 0; i < sha.size(); i++) {
        sha[i] = static_cast<std::uint8_t>(seed + i);
    }
    return sha;
}

// A run of 2,500 game ticks with one split at turn 1,800: checkpoints at ticks 1,000 and 2,000,
// at the split and at the last turn.
fe::RunFile good_run() {
    fe::RunFile r;
    r.binary_sha256 = sha_of(3);
    r.port_version = MT_VERSION;
    r.settings = "run_timer = on\nrun_category = chapter_1\n";
    r.streams[MT_RUNTAPE_CLOCK] = {{1764000000000000ull, 40}, {1764000000050000ull, 40}};
    r.categories = {
        {"orbis", "title = Orbis\nstart = map_change 2.0\nsplit chapter_end 1 = Orbis\n"}};
    r.splits = {{"Orbis", 1800, 61234000, 1220}};
    r.checkpoints = {{1200, 1000, 0x1111, "ticks=1000 mode=0 map=2.0 popup=0 panel=0"},
                     {1800, 1500, 0x2222, "ticks=1500 mode=0 map=4.1 popup=0 panel=0"},
                     {2400, 2000, 0x3333, "ticks=2000 mode=0 map=4.1 popup=0 panel=0"},
                     {3000, 2500, 0x4444, "ticks=2500 mode=0 map=4.1 popup=0 panel=0"}};
    r.turns = 3000;
    return r;
}

fe::RunRecord clean_record() {
    fe::RunRecord rec;
    rec.category = "orbis";
    rec.category_title = "Orbis";
    rec.finished = true;
    rec.splits = {fe::RunSplit{MT_GAME_EVENT_CHAPTER_END, 1, 0, "Orbis"}};
    rec.times = {fe::RunTime{61234, 1220}};
    rec.end = rec.times.back();
    rec.variables = {{"fix_original_bugs", "off"}, {"skip_intros", "off"}};
    return rec;
}

// What game.cpp gathers after a replay that went exactly as the file says.
fe::RunVerification replayed_as_recorded(const fe::RunFile &r) {
    fe::RunVerification v;
    v.path = "runs/20261127-101500.mtrun";
    v.port_version = r.port_version;
    v.turns = r.turns;
    v.events = r.events.size();
    v.checkpoints = r.checkpoints.size();
    v.categories = r.categories;
    v.recorded = r.splits;
    v.reached = true;
    v.compared = r.checkpoints.size();
    v.replayed = r.splits;
    v.runs = {clean_record()};
    return v;
}

} // namespace

TEST_CASE("verify_run: a file of another binary or another port version is refused") {
    const fe::RunFile r = good_run();
    CHECK(fe::run_binding_mismatch(r, sha_of(3)).empty());
    CHECK(fe::run_binding_mismatch(r, sha_of(4)) ==
          "it was recorded on another game binary (the armv7 slice's SHA-256 differs)");
    fe::RunFile other = r;
    other.port_version = "9.9.9-other";
    CHECK(fe::run_binding_mismatch(other, sha_of(3)) ==
          "it was recorded by MapleThief 9.9.9-other, this is " MT_VERSION
          " (a port update may change timing: play it with the version that made it)");
    // The refusal's report.
    fe::RunVerification v;
    v.path = "a.mtrun";
    v.refused = fe::run_binding_mismatch(r, sha_of(4));
    CHECK_FALSE(fe::run_verified(v));
    CHECK(fe::run_verify_report(v) ==
          "MapleThief --verify-run a.mtrun\n"
          "verdict: NOT VERIFIED (refused: it was recorded on another game binary (the armv7 "
          "slice's SHA-256 differs))\n");
}

TEST_CASE("verify_run: a flipped byte anywhere in the file is refused") {
    const std::vector<std::uint8_t> good = fe::write_run_file(good_run());
    REQUIRE(!good.empty());
    fe::RunFile back;
    std::string why;
    REQUIRE(fe::read_run_file(good.data(), good.size(), &back, &why));
    CHECK(back == good_run());
    CHECK(fe::run_file_problem(back).empty());
    // Every byte of the packed body is covered by its CRC-32, the header by its own checks: a
    // flipped bit anywhere refuses the file, or (the header's flags) reads as another file.
    std::size_t refused = 0;
    for (std::size_t i = 0; i < good.size(); i++) {
        std::vector<std::uint8_t> b = good;
        b[i] ^= 0x10;
        fe::RunFile out;
        std::string w;
        if (!fe::read_run_file(b.data(), b.size(), &out, &w)) {
            refused++;
            CHECK(w.rfind("byte ", 0) == 0);
        }
    }
    CHECK(refused >= good.size() - 4); // the header's 4 flag bytes are not checked
    std::vector<std::uint8_t> b = good;
    b[good.size() - 1] ^= 0x01;
    CHECK(fe::read_run_file(b.data(), b.size(), &back, &why) == false);
    CHECK(why == "byte 32: the packed body's CRC-32 does not match");
}

TEST_CASE("verify_run: a missing checkpoint is refused before the replay") {
    const fe::RunFile good = good_run();
    CHECK(fe::run_file_problem(good).empty());
    // The tick checkpoint at 2,000.
    fe::RunFile r = good;
    r.checkpoints.erase(r.checkpoints.begin() + 2);
    CHECK(fe::run_file_problem(r) == "a checkpoint is missing: none at tick 2000");
    // The split's.
    r = good;
    r.checkpoints.erase(r.checkpoints.begin() + 1);
    CHECK(fe::run_file_problem(r) ==
          "a checkpoint is missing: none at the split Orbis (turn 1800)");
    // The first: the last checkpoint's tick still asks for every 1,000 before it.
    r = good;
    r.checkpoints.erase(r.checkpoints.begin());
    CHECK(fe::run_file_problem(r) == "a checkpoint is missing: none at tick 1000");
    // Out of order, and after the recording's end.
    r = good;
    std::swap(r.checkpoints[0], r.checkpoints[1]);
    CHECK(fe::run_file_problem(r) ==
          "checkpoint 2 (turn 1200) is not after the one before (turn 1800)");
    r = good;
    r.turns = 2999;
    CHECK(fe::run_file_problem(r) == "checkpoint 4 is at turn 3000, after the recording's last "
                                     "(2999)");
    // A refusal before the replay is reported as main.cpp prints it.
    fe::RunVerification v;
    v.path = "b.mtrun";
    v.refused = "a checkpoint is missing: none at tick 2000";
    CHECK(fe::run_verify_report(v) ==
          "MapleThief --verify-run b.mtrun\n"
          "verdict: NOT VERIFIED (refused: a checkpoint is missing: none at tick 2000)\n");
}

TEST_CASE("verify_run: the report of a verified run") {
    const fe::RunVerification v = replayed_as_recorded(good_run());
    CHECK(fe::run_verified(v));
    CHECK(fe::run_verify_report(v) ==
          "MapleThief --verify-run runs/20261127-101500.mtrun\n"
          "file: recorded by MapleThief " MT_VERSION ", 3000 turns, 0 input events, 4 "
          "checkpoints\n"
          "category: orbis (the file's own)\n"
          "run 1: Orbis (orbis), finished, 1 of 1 splits\n"
          "  clean: every Assist held its neutral value\n"
          "  run variables: fix_original_bugs = off; skip_intros = off\n"
          "splits:  name                    recorded                  replayed\n"
          "  1. Orbis                   1:01.234 1220 ticks       1:01.234 1220 ticks  same\n"
          "checkpoints: 4 of 4 compared, all the same\n"
          "verdict: VERIFIED\n");
}

TEST_CASE("verify_run: an assisted run, a shipped category, a differing checkpoint") {
    fe::RunFile r = good_run();
    // The shipped chapter_1 category, as the recorder writes it (run_category_text).
    for (const auto &[name, text] : fe::shipped_run_category_files()) {
        if (name == "chapter_1") {
            std::vector<std::string> problems;
            r.categories = {
                {name, fe::run_category_text(*fe::parse_run_category(name, text, &problems))}};
        }
    }
    fe::RunVerification v = replayed_as_recorded(r);
    v.runs[0].assists = {"game_speed", "turbo_attack"};
    v.runs[0].variables = {{"fix_original_bugs", "off"}, {"skip_intros", "off,on"}};
    v.runs[0].in_category = false;
    // An input moved: the checkpoint after it differs, and so does the split.
    v.compared = 2;
    v.missed = 2;
    v.reached = false;
    v.first_difference = "turn 1800: recorded tick 1500 digest 0000000000002222 ticks=1500 mode=0 "
                         "map=4.1 popup=0 panel=0; replayed tick 1500 digest 0000000000009999 "
                         "ticks=1500 mode=0 map=2.0 popup=0 panel=0";
    v.replayed = {};
    CHECK_FALSE(fe::run_verified(v));
    CHECK(fe::run_unverified_reason(v) == "a checkpoint differs at " + v.first_difference);
    CHECK(fe::run_verify_report(v) ==
          "MapleThief --verify-run runs/20261127-101500.mtrun\n"
          "file: recorded by MapleThief " MT_VERSION ", 3000 turns, 0 input events, 4 "
          "checkpoints\n"
          "category: chapter_1 (shipped)\n"
          "run 1: Orbis (orbis), finished, 1 of 1 splits\n"
          "  assisted: game_speed, turbo_attack left the neutral value\n"
          "  run variables: fix_original_bugs = off; skip_intros = off,on (outside the category: "
          "never a personal best)\n"
          "splits:  name                    recorded                  replayed\n"
          "  1. Orbis                   1:01.234 1220 ticks       -  DIFFERS\n"
          "checkpoints: 2 of 4 compared, first difference at " +
              v.first_difference +
              "\n"
              "verdict: NOT VERIFIED (a checkpoint differs at " +
              v.first_difference + ")\n");
    // A category text that is not the shipped one's is the file's own.
    v.categories[0].text += "split boss_killed 69 = Last\n";
    CHECK(fe::run_verify_report(v).find(
              "category: chapter_1 (the file's own, differs from the shipped one)\n") !=
          std::string::npos);
}

TEST_CASE("verify_run: every other reason a replay does not verify") {
    const fe::RunVerification good = replayed_as_recorded(good_run());
    fe::RunVerification v = good;
    v.reached = false;
    CHECK(fe::run_unverified_reason(v) == "the replay ended before the recording's last turn");
    v = good;
    v.guest_ok = false;
    CHECK(fe::run_unverified_reason(v) == "the game stopped during the replay");
    v = good;
    v.missed = 1;
    CHECK(fe::run_unverified_reason(v) == "1 checkpoints not reached");
    v = good;
    v.replayed.clear();
    CHECK(fe::run_unverified_reason(v) == "the replay reached 0 splits, the file has 1");
    v = good;
    v.replayed[0].rta_us += 1000; // a time the tape does not give
    CHECK(fe::run_unverified_reason(v) == "split 1 (Orbis) differs from the file's");
    v = good;
    v.replayed[0].ticks += 1;
    CHECK(fe::run_unverified_reason(v) == "split 1 (Orbis) differs from the file's");
}

TEST_CASE("verify_run: times to the millisecond") {
    CHECK(fe::format_run_time_us(0) == "0:00.000");
    CHECK(fe::format_run_time_us(61234567) == "1:01.234");
    CHECK(fe::format_run_time_us(3723456000ull) == "1:02:03.456");
}
