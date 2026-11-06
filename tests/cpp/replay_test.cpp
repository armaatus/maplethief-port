// Input replay and checkpoints (#91, ADR 0104, platform/src/replay.h) on synthetic data: the
// replay file's round trip, every malformed line refused with its line number, the names against
// SDL's, the cursor's order, the frame hash, the checkpoint lines, digests and the first
// difference, the plan's steps, and the expects. The real run is play_real_test's regress modes
// and scripts/regress (local, real assets).
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "replay.h"

namespace {

namespace fe = maplethief::frontend;

const char *const kGood = R"(# a comment
maplethief-replay 1
step_us 1000
every 1000

data empty
500 pad attach xbox360
1234 pad button a down
1334 pad button a up
1334 pad axis leftx -32768
2000 pad axis lefttrigger 32767
2000 check first_map
2500 check combat
3000 end
expect first_map map=2.8
expect combat kills>=1 hp<=50 map=2.9
)";

fe::Replay good() {
    fe::Replay r;
    std::string error;
    REQUIRE(fe::parse_replay(kGood, &r, &error));
    return r;
}

std::string refused(const std::string &text) {
    fe::Replay r;
    std::string error;
    CHECK(!fe::parse_replay(text, &r, &error));
    return error;
}

} // namespace

TEST_CASE("replay: a file parses, and formats back to the same replay") {
    const fe::Replay r = good();
    CHECK(r.step_us == 1000);
    CHECK(r.every == 1000);
    CHECK(r.data == "empty");
    REQUIRE(r.events.size() == 8);
    CHECK(r.events[0].kind == fe::ReplayKind::kPadAttach);
    CHECK(r.events[0].name == "xbox360");
    CHECK(r.events[1].step == 1234);
    CHECK(r.events[1].kind == fe::ReplayKind::kPadButton);
    CHECK(r.events[1].index == SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK(r.events[1].value == 1);
    CHECK(r.events[2].value == 0);
    CHECK(r.events[3].kind == fe::ReplayKind::kPadAxis);
    CHECK(r.events[3].index == SDL_GAMEPAD_AXIS_LEFTX);
    CHECK(r.events[3].value == -32768);
    CHECK(r.events[4].index == SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    CHECK(r.events[5].kind == fe::ReplayKind::kCheck);
    CHECK(r.events[5].name == "first_map");
    CHECK(r.events[7].kind == fe::ReplayKind::kEnd);
    CHECK(r.end_step() == 3000);
    REQUIRE(r.expects.size() == 4);
    CHECK(r.expects[1].check == "combat");
    CHECK(r.expects[1].key == "kills");
    CHECK(r.expects[1].op == ">=");
    CHECK(r.expects[1].value == "1");
    CHECK(r.expects[2].op == "<=");
    CHECK(r.expects[3].op == "=");
    CHECK(r.expects[3].value == "2.9");

    const std::string text = fe::format_replay(r, {"recorded by a test"});
    CHECK(text.find("# recorded by a test\n") != std::string::npos);
    fe::Replay back;
    std::string error;
    REQUIRE(fe::parse_replay(text, &back, &error));
    CHECK(back.events == r.events);
    CHECK(back.every == r.every);
    CHECK(back.expects.size() == r.expects.size());
    CHECK(fe::format_replay(back, {"recorded by a test"}) == text);
    // CR LF line ends read the same.
    std::string crlf;
    for (const char c : std::string(kGood)) {
        crlf += c == '\n' ? std::string("\r\n") : std::string(1, c);
    }
    fe::Replay r2;
    REQUIRE(fe::parse_replay(crlf, &r2, &error));
    CHECK(r2.events == r.events);
}

TEST_CASE("replay: malformed files are refused with the line") {
    CHECK(refused("") == "line 0: empty: no 'maplethief-replay' header");
    CHECK(refused("hello 1\n").rfind("line 1: not a replay", 0) == 0);
    CHECK(refused("maplethief-replay 2\n").rfind("line 1: version 2", 0) == 0);
    const std::string h = "maplethief-replay 1\n";
    CHECK(refused(h + "step_us 0\n").rfind("line 2: step_us", 0) == 0);
    CHECK(refused(h + "step_us 2000000\n").rfind("line 2: step_us", 0) == 0);
    CHECK(refused(h + "every -1\n").rfind("line 2: every", 0) == 0);
    CHECK(refused(h + "data saved\n").rfind("line 2: data", 0) == 0);
    CHECK(refused(h + "10 pad attach ps9\n").rfind("line 2: an unknown pad kind", 0) == 0);
    CHECK(refused(h + "10 pad button z down\n").rfind("line 2: pad button", 0) == 0);
    CHECK(refused(h + "10 pad button a held\n").rfind("line 2: pad button", 0) == 0);
    CHECK(refused(h + "10 pad axis leftx 40000\n").rfind("line 2: pad axis", 0) == 0);
    CHECK(refused(h + "10 pad axis leftx 1.5\n").rfind("line 2: pad axis", 0) == 0);
    CHECK(refused(h + "10 pad axis wheel 0\n").rfind("line 2: pad axis", 0) == 0);
    CHECK(refused(h + "10 check a/b\n").rfind("line 2: a check's name", 0) == 0);
    CHECK(refused(h + "10 check a\n20 check a\n").rfind("line 3: check 'a' twice", 0) == 0);
    CHECK(refused(h + "10 jump\n").rfind("line 2: an unknown event", 0) == 0);
    CHECK(refused(h + "-5 end\n").rfind("line 2: '-5' is not a step", 0) == 0);
    CHECK(refused(h + "12x end\n").rfind("line 2: '12x' is not a step", 0) == 0);
    CHECK(refused(h + "10\n").rfind("line 2: a step without an event", 0) == 0);
    CHECK(refused(h + "20 end\n10 check a\n").rfind("line 3: an event after the end", 0) == 0);
    CHECK(refused(h + "20 check b\n10 check a\n").rfind("line 3: step 10 is before", 0) == 0);
    CHECK(refused(h + "20 end\n30 end\n").rfind("line 3: an event after the end", 0) == 0);
    CHECK(refused(h + "expect a map=1\n").rfind("line 2: expect on check 'a'", 0) == 0);
    CHECK(refused(h + "10 check a\nexpect a\n").rfind("line 3: expect needs", 0) == 0);
    CHECK(refused(h + "10 check a\nexpect a map\n").rfind("line 3: a fact is", 0) == 0);
    CHECK(refused(h + "10 check a\nexpect a kills>=x\n").rfind("line 3: a bad fact", 0) == 0);
    CHECK(refused(h + "10 check a\nexpect a =1\n").rfind("line 3: a bad fact", 0) == 0);
    CHECK(refused(h + "1000000000001 end\n").rfind("line 2:", 0) == 0);
}

TEST_CASE("replay: the button and axis names are SDL's") {
    for (int b = 0; b < 15; b++) {
        const char *sdl = SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(b));
        REQUIRE(sdl != nullptr);
        CHECK(std::string(fe::replay_button_name(b)) == sdl);
        CHECK(fe::replay_button_from_name(sdl) == b);
    }
    for (int a = 0; a < 6; a++) {
        const char *sdl = SDL_GetGamepadStringForAxis(static_cast<SDL_GamepadAxis>(a));
        REQUIRE(sdl != nullptr);
        CHECK(std::string(fe::replay_axis_name(a)) == sdl);
        CHECK(fe::replay_axis_from_name(sdl) == a);
    }
    CHECK(std::string(fe::replay_button_name(15)).empty());
    CHECK(std::string(fe::replay_axis_name(-1)).empty());
    CHECK(fe::replay_button_from_name("misc1") == -1);
}

TEST_CASE("replay: the cursor hands out the input events due, in order, once") {
    const fe::Replay r = good();
    fe::ReplayCursor c(&r);
    CHECK(c.next(499) == nullptr);
    const fe::ReplayEvent *e = c.next(500);
    REQUIRE(e != nullptr);
    CHECK(e->kind == fe::ReplayKind::kPadAttach);
    CHECK(c.next(500) == nullptr);
    CHECK(c.next(1233) == nullptr);
    e = c.next(1334); // a late turn gets what it missed, in order
    REQUIRE(e != nullptr);
    CHECK(e->step == 1234);
    e = c.next(1334);
    REQUIRE(e != nullptr);
    CHECK((e->kind == fe::ReplayKind::kPadButton && e->value == 0));
    e = c.next(1334);
    REQUIRE(e != nullptr);
    CHECK(e->kind == fe::ReplayKind::kPadAxis);
    CHECK(c.next(1334) == nullptr);
    CHECK(!c.finished());
    e = c.next(5000); // the trigger; checks and the end are not input
    REQUIRE(e != nullptr);
    CHECK(e->index == SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    CHECK(c.next(5000) == nullptr);
    CHECK(c.finished());
}

TEST_CASE("replay: the frame hash covers the size and every byte") {
    std::vector<std::uint8_t> px(4 * 3 * 2, 7);
    const std::uint64_t h = fe::frame_hash(px.data(), 3, 2);
    CHECK(h == fe::frame_hash(px.data(), 3, 2));
    CHECK(h != fe::frame_hash(px.data(), 2, 3)); // the same bytes, another shape
    px[23] = 8;
    CHECK(h != fe::frame_hash(px.data(), 3, 2));
    // FNV-1a 64's reference values.
    CHECK(fe::fnv1a64("", 0) == 0xcbf29ce484222325ull);
    CHECK(fe::fnv1a64("a", 1) == 0xaf63dc4c8601ec8cull);
    CHECK(fe::hex64(0xaf63dc4c8601ec8cull) == "af63dc4c8601ec8c");
    CHECK(fe::hex64(1) == "0000000000000001");
}

TEST_CASE("replay: checkpoint lines, the log's digest and its first difference") {
    fe::Checkpoint c;
    c.name = "first_map";
    c.step = 41000;
    c.frame = 0x0123456789abcdefull;
    c.audio = 0xfedcba9876543210ull;
    c.audio_frames = 1808100;
    c.gl_frame = 812;
    c.state = "map=2.8 x=-12 hp=50";
    const std::string line = fe::format_checkpoint(c);
    CHECK(line == "first_map step 41000 frame 0123456789abcdef audio fedcba9876543210/1808100 gl "
                  "812 | map=2.8 x=-12 hp=50");
    fe::Checkpoint back;
    std::string error;
    REQUIRE(fe::parse_checkpoint(line, &back, &error));
    CHECK(fe::format_checkpoint(back) == line);
    CHECK(!fe::parse_checkpoint("first_map step 1 frame 0 audio 0/0 gl 0 | ", &back, &error));
    CHECK(!fe::parse_checkpoint("first_map step 41000", &back, &error));
    CHECK(!fe::parse_checkpoint(line + " broken", &back, &error)); // a word without '='

    fe::CheckpointLog a;
    a.add(c);
    c.name = "combat";
    c.step = 90000;
    a.add(c);
    fe::CheckpointLog b;
    REQUIRE(fe::CheckpointLog::parse(a.text(), &b, &error));
    CHECK(b.text() == a.text());
    CHECK(b.digest() == a.digest());
    CHECK(a.digest().size() == 16);
    CHECK(fe::compare_logs(a, b).same);
    REQUIRE(a.find("combat") != nullptr);
    CHECK(a.find("combat")->step == 90000);
    CHECK(a.find("nothing") == nullptr);

    fe::CheckpointLog d;
    d.add(a.points()[0]);
    c.frame ^= 1; // one bit of the screen
    d.add(c);
    CHECK(d.digest() != a.digest());
    const fe::LogDifference diff = fe::compare_logs(a, d);
    CHECK(!diff.same);
    CHECK(diff.index == 1);
    CHECK(diff.a.rfind("combat step 90000 frame 0123456789abcdef", 0) == 0);
    CHECK(diff.b.rfind("combat step 90000 frame 0123456789abcdee", 0) == 0);
    // A prefix: the first checkpoint past the shorter log.
    fe::CheckpointLog p;
    p.add(a.points()[0]);
    const fe::LogDifference short_diff = fe::compare_logs(a, p);
    CHECK(!short_diff.same);
    CHECK(short_diff.index == 1);
    CHECK(short_diff.b.empty());
    CHECK(!fe::CheckpointLog::parse("garbage\n", &b, &error));
    CHECK(error.rfind("line 1:", 0) == 0);
}

TEST_CASE("replay: the plan takes the named checks, every N steps, up to the end") {
    const fe::Replay r = good();
    const fe::CheckpointPlan plan(r);
    CHECK(plan.named() == 2);
    CHECK(plan.due(0).empty());
    CHECK(plan.due(999).empty());
    CHECK(plan.due(1000) == std::vector<std::string>{"s1000"});
    CHECK(plan.due(2000) == std::vector<std::string>{"s2000", "first_map"});
    CHECK(plan.due(2500) == std::vector<std::string>{"combat"});
    CHECK(plan.due(3000) == std::vector<std::string>{"s3000"});
    CHECK(plan.due(4000).empty()); // past the end
    // A caller's plan (#190: run N steps from a state, hash every K).
    fe::CheckpointPlan mine;
    mine.set_every(50);
    mine.set_end(120);
    mine.add(120, "end");
    mine.add(120, "end_again");
    CHECK(mine.due(50) == std::vector<std::string>{"s50"});
    CHECK(mine.due(120) == std::vector<std::string>{"end", "end_again"});
    CHECK(mine.due(150).empty());
}

TEST_CASE("replay: an expect holds or says what the state shows") {
    std::map<std::string, std::string> state;
    REQUIRE(fe::parse_state("map=2.9 kills=2 hp=40", &state));
    CHECK(state.at("map") == "2.9");
    std::string why;
    const fe::Replay r = good();
    for (const fe::ReplayExpect &e : r.expects) {
        if (e.check == "combat") {
            CHECK(fe::expect_holds(e, state, &why));
        }
    }
    CHECK(!fe::expect_holds({"x", "map", "=", "2.8"}, state, &why));
    CHECK(why == "map=2.9");
    CHECK(!fe::expect_holds({"x", "kills", ">=", "3"}, state, &why));
    CHECK(fe::expect_holds({"x", "kills", "<=", "2"}, state, &why));
    CHECK(!fe::expect_holds({"x", "level", "=", "1"}, state, &why));
    CHECK(why == "level is not in the state");
    CHECK(!fe::expect_holds({"x", "map", ">=", "2"}, state, &why)); // not a number
    CHECK(!fe::parse_state("map=2.9 broken", &state));
    CHECK(!fe::parse_state("=1", &state));
    CHECK(fe::parse_state("", &state));
    CHECK(state.empty());
}

TEST_CASE("replay: a recorded replay only goes forward") {
    fe::Replay r;
    CHECK(r.add({10, fe::ReplayKind::kPadAttach, 0, 0, "xbox360"}));
    CHECK(r.add({10, fe::ReplayKind::kPadButton, 0, 1, ""}));
    CHECK(!r.add({9, fe::ReplayKind::kPadButton, 0, 0, ""}));
    CHECK(r.add({20, fe::ReplayKind::kEnd, 0, 0, ""}));
    CHECK(!r.add({30, fe::ReplayKind::kCheck, 0, 0, "late"}));
    CHECK(r.end_step() == 20);
    fe::Replay parsed;
    std::string error;
    REQUIRE(fe::parse_replay(fe::format_replay(r), &parsed, &error));
    CHECK(parsed.events == r.events);
}
