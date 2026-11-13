// The Nintendo Switch's applet side (#240, ADR 0141, platform/src/switch_applet.h) with
// synthetic data: the run mode from libnx's applet type, the memory verdict and its log line,
// applet mode's message, when the CPU boost is wanted, and AppletWatch's turns with a fake
// measurement and a recording boost. The lifecycle sequences are in lifecycle_test.cpp.
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "switch_applet.h"

namespace {

namespace fe = maplethief::frontend;

constexpr std::uint64_t kMiB = std::uint64_t{1} << 20;

fe::MemorySample sample(std::uint64_t used_mib, std::uint64_t size_mib = 400) {
    fe::MemorySample s;
    s.guest_committed = used_mib / 2 * kMiB;
    s.heap_size = size_mib * kMiB;
    s.heap_used = used_mib * kMiB;
    s.process_total = (size_mib + 20) * kMiB;
    s.process_used = (size_mib + 10) * kMiB;
    return s;
}

bool contains(const std::string &s, const std::string &part) {
    return s.find(part) != std::string::npos;
}

} // namespace

TEST_CASE("switch_applet: sdl3-nx's events for each focus change and the exit request") {
    fe::SdlSwitchModel sdl;
    CHECK(sdl.focus(fe::SwitchFocus::kOutOfFocus).empty());
    CHECK(sdl.focus(fe::SwitchFocus::kInFocus).empty());
    CHECK(sdl.focus(fe::SwitchFocus::kBackground) ==
          std::vector<std::uint32_t>{SDL_EVENT_WILL_ENTER_BACKGROUND,
                                     SDL_EVENT_DID_ENTER_BACKGROUND});
    CHECK(sdl.background());
    CHECK(sdl.focus(fe::SwitchFocus::kOutOfFocus).empty());
    CHECK(sdl.focus(fe::SwitchFocus::kInFocus) ==
          std::vector<std::uint32_t>{SDL_EVENT_WILL_ENTER_FOREGROUND,
                                     SDL_EVENT_DID_ENTER_FOREGROUND});
    CHECK_FALSE(sdl.background());
    CHECK(fe::SdlSwitchModel::exit_request() ==
          std::vector<std::uint32_t>{SDL_EVENT_TERMINATING, SDL_EVENT_QUIT});
    CHECK(fe::switch_focus_name(1) == "InFocus");
    CHECK(fe::switch_focus_name(2) == "OutOfFocus");
    CHECK(fe::switch_focus_name(3) == "Background");
    CHECK(fe::switch_focus_name(7) == "focus 7");
}

TEST_CASE("switch_applet: title takeover is an application, anything else applet mode") {
    CHECK(fe::switch_run_mode(0) == fe::SwitchRunMode::kTitleTakeover); // Application
    CHECK(fe::switch_run_mode(4) == fe::SwitchRunMode::kTitleTakeover); // SystemApplication
    CHECK(fe::switch_run_mode(2) == fe::SwitchRunMode::kApplet);        // LibraryApplet
    CHECK(fe::switch_run_mode(-1) == fe::SwitchRunMode::kApplet);
    CHECK(std::string(fe::switch_run_mode_name(fe::SwitchRunMode::kApplet)) == "applet mode");
    CHECK(std::string(fe::switch_run_mode_name(fe::SwitchRunMode::kTitleTakeover)) ==
          "title takeover");
}

TEST_CASE("switch_applet: the game fits while the heap keeps the margin free") {
    CHECK(fe::kAppletMarginBytes == 64 * kMiB);
    fe::MemoryVerdict v = fe::judge_memory(sample(300));
    CHECK(v.fits);
    CHECK(v.headroom == 100 * kMiB);
    v = fe::judge_memory(sample(336)); // exactly the margin left
    CHECK(v.fits);
    CHECK(v.headroom == 64 * kMiB);
    v = fe::judge_memory(sample(337));
    CHECK_FALSE(v.fits);
    CHECK(v.headroom == 63 * kMiB);
    v = fe::judge_memory(sample(500)); // over: no headroom, not negative
    CHECK_FALSE(v.fits);
    CHECK(v.headroom == 0);
    CHECK(fe::judge_memory(sample(390), 8 * kMiB).fits);
}

TEST_CASE("switch_applet: the memory line and applet mode's message") {
    const std::string fits =
        fe::memory_line("map 1.0 loaded at 12 s", fe::SwitchRunMode::kApplet, sample(300));
    CHECK(fits == "memory: map 1.0 loaded at 12 s (applet mode): guest committed 150.0 MiB, "
                  "heap 300.0 MiB of 400.0 MiB in use, process 410.0 MiB of 420.0 MiB; headroom "
                  "100.0 MiB");
    const std::string tight =
        fe::memory_line("a new peak at 40 s", fe::SwitchRunMode::kApplet, sample(350));
    CHECK(contains(tight, "headroom 50.0 MiB, below the 64.0 MiB margin"));
    const std::string msg = fe::applet_mode_message(sample(350));
    CHECK(contains(msg, "applet mode"));
    CHECK(contains(msg, "50.0 MiB left of 400.0 MiB"));
    CHECK(contains(msg, "through a game title"));
}

TEST_CASE("switch_applet: the CPU boost only before the first turn and in the loading modes") {
    CHECK(fe::boost_wanted(false, -1));
    CHECK(fe::boost_wanted(false, 0));
    for (const std::int32_t mode : {6, 7, 8}) {
        CHECK(fe::boost_wanted(true, mode));
    }
    for (const std::int32_t mode : {-1, 0, 1, 2, 5, 9, 10}) {
        CHECK_FALSE(fe::boost_wanted(true, mode)); // the field, menus, scenes, the title
    }
}

TEST_CASE("switch_applet: AppletWatch boosts loads, logs maps and peaks, warns applet mode once") {
    std::vector<bool> boosts;
    std::uint64_t used = 200;
    int sampled = 0;
    fe::AppletWatch w;
    CHECK_FALSE(w.enabled());
    CHECK_FALSE(w.on_turn(1, 0, 0, 0.0).has_value()); // not configured: nothing at all
    w.configure(
        fe::SwitchRunMode::kApplet,
        [&]() -> std::optional<fe::MemorySample> {
            sampled++;
            return sample(used);
        },
        [&](bool on) { boosts.push_back(on); });
    CHECK(w.enabled());
    CHECK(boosts == std::vector<bool>{true}); // the start is a long load
    // The title (not in the game): the boost ends, the first turn measures.
    CHECK_FALSE(w.on_turn(-1, -1, -1, 0.5).has_value());
    CHECK(boosts == std::vector<bool>{true, false});
    CHECK(sampled == 1);
    CHECK_FALSE(w.on_turn(-1, -1, -1, 0.9).has_value()); // within the second: no sample
    CHECK(sampled == 1);
    // A map load (mode 7): boosted, no map line while loading.
    used = 260;
    CHECK_FALSE(w.on_turn(7, 0, 0, 1.0).has_value());
    CHECK(boosts == std::vector<bool>{true, false, true});
    CHECK(sampled == 1); // half a second since the last sample
    CHECK_FALSE(w.on_turn(7, 0, 0, 1.2).has_value());
    CHECK(sampled == 1);
    // In the field: the boost off, the new map measured at once.
    CHECK_FALSE(w.on_turn(0, 0, 0, 1.3).has_value());
    CHECK(boosts == std::vector<bool>{true, false, true, false});
    CHECK(sampled == 2);
    CHECK(w.peak().heap_used == 260 * kMiB);
    // Below the margin: the message once.
    used = 350;
    const std::optional<std::string> msg = w.on_turn(0, 0, 0, 2.5);
    REQUIRE(msg.has_value());
    CHECK(contains(*msg, "through a game title"));
    used = 360;
    CHECK_FALSE(w.on_turn(1, 0, 1, 2.6).has_value()); // the next map: measured, not warned again
    CHECK(sampled == 4);
    CHECK(w.peak().heap_used == 360 * kMiB);
    w.finish();
    CHECK(boosts.back() == false);
    CHECK(w.samples() == 4);
}

TEST_CASE("switch_applet: title takeover never shows the message; no measurement, no lines") {
    std::uint64_t used = 399;
    fe::AppletWatch w;
    w.configure(
        fe::SwitchRunMode::kTitleTakeover,
        [&]() -> std::optional<fe::MemorySample> { return sample(used); }, nullptr);
    CHECK(w.boosted()); // without a switch to turn, the state is still kept
    CHECK_FALSE(w.on_turn(0, 1, 0, 0.0).has_value());
    CHECK_FALSE(w.boosted());
    CHECK(w.samples() == 1);
    w.finish();
    // A sampler that has nothing (svcGetInfo refused): no sample counted.
    fe::AppletWatch none;
    none.configure(
        fe::SwitchRunMode::kApplet,
        []() -> std::optional<fe::MemorySample> { return std::nullopt; }, nullptr);
    CHECK_FALSE(none.on_turn(0, 1, 0, 0.0).has_value());
    CHECK(none.samples() == 0);
    none.finish();
}
