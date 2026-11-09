// The start-up build line and the Debug build's behind-the-tick detector (#136, ADR 0134).
#include <doctest/doctest.h>

#include <cstdint>
#include <string>

#include "build_info.h"

using maplethief::frontend::build_description;
using maplethief::frontend::TickLag;

namespace {

constexpr std::uint64_t kMs = 1'000'000;
constexpr std::uint64_t kLimit = 55 * kMs;

} // namespace

TEST_CASE("build_info: the build line names the type and the optimisation level") {
    const std::string line = build_description();
    CHECK(line.find(" (") != std::string::npos);
    CHECK(line.back() == ')');
#ifndef NDEBUG
    CHECK(line.rfind("Debug (", 0) == 0);
    CHECK(line.find("2 for the generated code") != std::string::npos);
#endif
}

TEST_CASE("build_info: a build on the tick never warns") {
    TickLag lag;
    std::uint64_t t = kMs;
    for (int i = 0; i < 1000; i++) {
        t += 50 * kMs;
        CHECK_FALSE(lag.tick(t, kLimit));
    }
}

TEST_CASE("build_info: a slow build warns once, after one window of ticks") {
    TickLag lag;
    std::uint64_t t = kMs;
    int fired = 0;
    int at = -1;
    for (int i = 0; i < 1000; i++) {
        t += 160 * kMs;
        if (lag.tick(t, kLimit)) {
            fired++;
            at = i;
        }
    }
    CHECK(fired == 1);
    CHECK(at == static_cast<int>(TickLag::kTicks)); // the first tick has no interval
    CHECK(lag.mean_ms() == doctest::Approx(160.0));
}

TEST_CASE("build_info: one slow tick does not warn, and a gap starts the window over") {
    TickLag lag;
    std::uint64_t t = kMs;
    lag.tick(t, kLimit);
    // 39 slow intervals, then a pause of 3 s (loading): nothing yet, and the window is gone.
    for (std::uint32_t i = 0; i + 1 < TickLag::kTicks; i++) {
        t += 300 * kMs;
        CHECK_FALSE(lag.tick(t, kLimit));
    }
    t += 3000 * kMs;
    CHECK_FALSE(lag.tick(t, kLimit));
    // The window restarts: 39 slow intervals are not a window yet.
    for (std::uint32_t i = 0; i + 1 < TickLag::kTicks; i++) {
        t += 300 * kMs;
        CHECK_FALSE(lag.tick(t, kLimit));
    }
    // One long tick among normal ones is not falling behind, whatever it does to the mean.
    TickLag steady;
    t = kMs;
    steady.tick(t, kLimit);
    for (std::uint32_t i = 0; i < TickLag::kTicks; i++) {
        t += (i == 0 ? 400 : 50) * kMs;
        CHECK_FALSE(steady.tick(t, kLimit));
    }
}
