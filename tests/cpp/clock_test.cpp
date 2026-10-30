// The fixed-step mode's virtual clock (#60, ADR 0044, platform/src/clock.h): once installed it is
// the time of every guest clock (the libc clock behind gettimeofday and CFAbsoluteTimeGetCurrent,
// and GCD's), it moves only when the host advances it, and uninstalling gives the host clocks
// back. Its start puts the game's 32-bit GetTimeMs well clear of the 2^31 jump, and so does the
// real-time mode's launch clock (#155).
#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstdint>

#include "clock.h"
#include "maplethief/cf.h"
#include "maplethief/gcd.h"

namespace fe = maplethief::frontend;

namespace {

double host_absolute_time() {
    using namespace std::chrono;
    return static_cast<double>(
               duration_cast<microseconds>(system_clock::now().time_since_epoch()).count()) /
               1e6 -
           MT_CF_ABSOLUTE_TIME_1970;
}

} // namespace

TEST_CASE("clock: the virtual clock drives the libc and GCD clocks, and only the host moves it") {
    {
        fe::VirtualClock clock;
        REQUIRE(clock.install());
        CHECK(clock.installed());
        // 2025-01-01T00:00:00Z is CFAbsoluteTime 757,382,400.
        CHECK(mt_cf_absolute_time() == 757382400.0);
        CHECK(mt_gcd_now() == fe::VirtualClock::kGcdOrigin);
        // Standing still: reading it does not move it.
        CHECK(mt_cf_absolute_time() == 757382400.0);
        clock.advance(1500000); // 1.5 ms
        // As mt_cf_absolute_time computes it: microseconds / 1e6 - the 1970 offset.
        CHECK(mt_cf_absolute_time() == 1735689600.0015 - MT_CF_ABSOLUTE_TIME_1970);
        CHECK(mt_cf_absolute_time() - 757382400.0 == doctest::Approx(0.0015).epsilon(1e-6));
        CHECK(mt_gcd_now() == fe::VirtualClock::kGcdOrigin + 1500000);
        CHECK(clock.unix_us() == fe::VirtualClock::kStartUnixUs + 1500);
        // advance_to never goes back.
        clock.advance_to(1000);
        CHECK(clock.elapsed_ns() == 1500000);
        clock.advance_to(3000000000ull);
        CHECK(clock.elapsed_ns() == 3000000000ull);
        CHECK(mt_cf_absolute_time() == 757382403.0);
        clock.uninstall();
        CHECK_FALSE(clock.installed());
    }
    // The host clocks are back (and the destructor of an installed clock uninstalls it too).
    CHECK(std::abs(mt_cf_absolute_time() - host_absolute_time()) < 5.0);
    {
        fe::VirtualClock clock;
        REQUIRE(clock.install());
    }
    CHECK(std::abs(mt_cf_absolute_time() - host_absolute_time()) < 5.0);
}

TEST_CASE("clock: the start keeps the game's GetTimeMs below 2^30, clear of its 2^31 jump") {
    const std::int32_t start = fe::guest_time_ms(fe::VirtualClock::kStartUnixUs);
    CHECK(start == 522812416);
    CHECK(start >= 0);
    CHECK(start < (1 << 30));
    // 18.8 days of virtual time before the jump.
    CHECK(static_cast<double>(INT32_MAX - start) / 86400000.0 > 18.0);
    // The 32-bit multiply-add: 1,000 ms later is 1,000 more.
    CHECK(fe::guest_time_ms(fe::VirtualClock::kStartUnixUs + 1000000) == start + 1000);
}

TEST_CASE("clock: the launch clock starts GetTimeMs below 2^30, clear of its 2^31 jump") {
    // 2026-12-16T17:56:53Z (docs/re/timing-threading.md): GetTimeMs 0x7FFFFE88, 376 ms before
    // the jump that freezes the original's ticks for 49.7 days.
    const std::uint64_t before_jump = 1797443813000ull * 1000u;
    CHECK(fe::guest_time_ms(before_jump) == 0x7FFFFE88);
    CHECK(fe::guest_time_ms(before_jump + 400000) < 0); // the jump, 0.4 s later
    // Moved back by 2^30 ms (12.4 days): GetTimeMs keeps its low 30 bits.
    const std::uint64_t shift = fe::LaunchClock::shift_for(before_jump);
    CHECK(shift == (1ull << 30) * 1000u);
    CHECK(fe::guest_time_ms(before_jump - shift) == 0x3FFFFE88);
    // Every launch: a shift of 0 to 3 times 2^30 ms, and GetTimeMs in [0, 2^30) after it, so the
    // jump is at least 2^30 ms (12.4 days) of running away.
    for (std::uint64_t ms :
         {0ull, 1000ull, 0x3FFFFFFFull, 0x40000000ull, 0x80000000ull, 0xBFFFFFFFull, 0xC0000005ull,
          0xFFFFFFFFull, 1797443813123ull, 1793148846000ull, 1735689600000ull}) {
        CAPTURE(ms);
        const std::uint64_t us = ms * 1000u + 999u;
        const std::uint64_t s = fe::LaunchClock::shift_for(us);
        CHECK(s % ((1ull << 30) * 1000u) == 0u);
        CHECK(s <= 3u * (1ull << 30) * 1000u);
        const std::int32_t t = fe::guest_time_ms(us - s);
        CHECK(t >= 0);
        CHECK(t < (1 << 30));
        CHECK((static_cast<std::uint32_t>(t) & 0x3FFFFFFFu) ==
              (static_cast<std::uint32_t>(fe::guest_time_ms(us)) & 0x3FFFFFFFu));
    }
    // Installed, it is the libc clock (gettimeofday, CFAbsoluteTimeGetCurrent): the wall clock
    // less the shift; uninstalled, the host's clock is back.
    {
        fe::LaunchClock clock;
        clock.install();
        CHECK(clock.installed());
        const std::int32_t now = fe::guest_time_ms(clock.unix_us());
        CHECK(now >= 0);
        CHECK(now < (1 << 30));
        const double shifted = host_absolute_time() - static_cast<double>(clock.shift_us()) / 1e6;
        CHECK(std::abs(mt_cf_absolute_time() - shifted) < 5.0);
    }
    CHECK(std::abs(mt_cf_absolute_time() - host_absolute_time()) < 5.0);
}
