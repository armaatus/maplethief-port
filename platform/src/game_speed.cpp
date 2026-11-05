// Game speed and fast-forward (#151, ADR 0087); see game_speed.h.
#include "game_speed.h"

#include <algorithm>
#include <cstring>
#include <iterator>

#include "clock.h"
#include "frame_stats.h"
#include "maplethief/memory.h"
#include "maplethief/recomp/cpu.h"

namespace maplethief::frontend {

namespace {

// Whether [addr, addr + size) is in guest memory and, when the cpu has the runtime's memory, in
// one of the image's segments (a test's guest may map neither).
bool image_mapped(const mt_cpu *cpu, std::uint32_t addr, std::uint32_t size) {
    if (cpu == nullptr || cpu->mem == nullptr || std::uint64_t{addr} + size > cpu->mem_size) {
        return false;
    }
    if (const mt_mem *mem = mt_mem_from_cpu(cpu)) {
        std::uint32_t n = 0;
        const mt_segment *segs = mt_mem_segments(mem, &n);
        for (std::uint32_t i = 0; i < n; i++) {
            if (addr >= segs[i].vmaddr &&
                std::uint64_t{addr} + size <= std::uint64_t{segs[i].vmaddr} + segs[i].vmsize) {
                return true;
            }
        }
        return false;
    }
    return true;
}

std::int32_t read_i32(const mt_cpu *cpu, std::uint32_t addr) {
    std::int32_t v = 0;
    std::memcpy(&v, cpu->mem + addr, sizeof v);
    return v;
}

void write_i32(mt_cpu *cpu, std::uint32_t addr, std::int32_t v) {
    std::memcpy(cpu->mem + addr, &v, sizeof v);
}

// The table holds the game's lengths, or one length three times (a patch), then -1.
bool table_ok(const mt_cpu *cpu) {
    const std::int32_t a = read_i32(cpu, kGameSpeedTable);
    const std::int32_t b = read_i32(cpu, kGameSpeedTable + 4);
    const std::int32_t c = read_i32(cpu, kGameSpeedTable + 8);
    if (read_i32(cpu, kGameSpeedTable + 12) != -1) {
        return false;
    }
    const bool game =
        a == kGameSpeedLengths[0] && b == kGameSpeedLengths[1] && c == kGameSpeedLengths[2];
    return game || (a == b && b == c && a > 0 && a <= 1000);
}

} // namespace

std::uint32_t game_speed_ms(GameSpeed speed) {
    switch (speed) {
    case GameSpeed::kGame:
        return 0;
    case GameSpeed::k65:
        return 65;
    case GameSpeed::k50:
        return 50;
    case GameSpeed::k40:
        return 40;
    case GameSpeed::k33:
        return 33;
    case GameSpeed::k25:
        return 25;
    }
    return 0;
}

bool patch_game_speed(mt_cpu *cpu, GameSpeed speed) {
    if (!image_mapped(cpu, kGameSpeedTable, 16) || !table_ok(cpu)) {
        return false;
    }
    const std::uint32_t ms = game_speed_ms(speed);
    for (std::uint32_t i = 0; i < 3; i++) {
        write_i32(cpu, kGameSpeedTable + 4 * i,
                  ms != 0 ? static_cast<std::int32_t>(ms) : kGameSpeedLengths[i]);
    }
    return true;
}

std::int64_t apply_game_speed(mt_cpu *cpu, GameSpeed speed) {
    if (!patch_game_speed(cpu, speed) || !image_mapped(cpu, kConfigPointer, 4) ||
        !image_mapped(cpu, kFrameInterval, 8)) {
        return 0;
    }
    std::uint32_t config = 0;
    std::memcpy(&config, cpu->mem + kConfigPointer, sizeof config);
    if (config == 0 || std::uint64_t{config} + 2 > cpu->mem_size) {
        return 0; // no config yet: LoadConfig's ApplyGameSpeed reads the patched table
    }
    std::uint32_t index = cpu->mem[config + 1];
    index = index > 2 ? 2 : index; // mt_saves_sanitise's reading of a bad byte (#78)
    const std::int64_t ms = read_i32(cpu, kGameSpeedTable + 4 * index);
    std::memcpy(cpu->mem + kFrameInterval, &ms, sizeof ms);
    return ms;
}

std::uint32_t fast_forward_rate(FastForward ff, bool held) {
    if (!held) {
        return 1;
    }
    switch (ff) {
    case FastForward::kOff:
        return 1;
    case FastForward::kX2:
        return 2;
    case FastForward::kX3:
        return 3;
    }
    return 1;
}

std::uint64_t overrun_ns_for(std::int64_t tick_ms, std::uint32_t rate) {
    if (tick_ms <= 0 || tick_ms > 1000 || rate == 0) {
        return FrameStats::kDefaultOverrunNs;
    }
    return static_cast<std::uint64_t>(tick_ms) * 1'100'000u / rate;
}

std::int64_t gate_in_us(const mt_cpu *cpu, std::uint64_t unix_us) {
    if (!image_mapped(cpu, kFrameInterval, 8) || !image_mapped(cpu, kLastFrame, 8)) {
        return kNoGate;
    }
    std::int64_t interval = 0;
    std::int64_t last_frame = 0;
    std::memcpy(&interval, cpu->mem + kFrameInterval, sizeof interval);
    std::memcpy(&last_frame, cpu->mem + kLastFrame, sizeof last_frame);
    if (interval <= 0 || interval > 1000) {
        return kNoGate;
    }
    // The gate's signed 64-bit compare of two sign-extended GetTimeMs readings.
    const std::int64_t now_ms = guest_time_ms(unix_us);
    const std::int64_t left_ms = last_frame + interval - now_ms;
    return left_ms * 1000 - static_cast<std::int64_t>(unix_us % 1000u);
}

double ff_gate_wait(double timer_s, std::int64_t gate_us) {
    if (gate_us == kNoGate) {
        return timer_s;
    }
    const double gate_s = static_cast<double>(gate_us + kGateMarginUs) / 1e6;
    const bool soon_after = gate_s > timer_s && gate_s - timer_s <= kGatePollUs / 1e6;
    return soon_after ? gate_s : timer_s;
}

void FastForwardPresents::presented(std::uint64_t ns) {
    costs_[next_] = ns;
    next_ = (next_ + 1) % kCosts;
}

std::uint64_t FastForwardPresents::cost_ns() const {
    return *std::max_element(std::begin(costs_), std::end(costs_)) + kMarginNs;
}

void FastForwardPresents::restart() {
    budget_ = 0;
    last_ = 0;
}

bool FastForwardPresents::allow(std::uint64_t now, std::uint64_t since, std::uint64_t refresh_ns,
                                std::int64_t gate_ns) {
    const double cost = static_cast<double>(cost_ns());
    if (last_ != 0 && now > last_) {
        // At most one present's worth: the budget never lets a burst of overruns through.
        budget_ = std::min(cost, budget_ + kShare * static_cast<double>(now - last_));
    }
    last_ = now;
    if (since < refresh_ns) {
        return false;
    }
    const double overrun =
        gate_ns == kNoGate ? 0 : cost - static_cast<double>(std::max<std::int64_t>(gate_ns, 0));
    if (overrun <= 0) {
        allowed_++;
        return true;
    }
    if (budget_ >= overrun) {
        budget_ -= overrun;
        allowed_++;
        overran_++;
        return true;
    }
    return false;
}

} // namespace maplethief::frontend
