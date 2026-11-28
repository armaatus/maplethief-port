// Text_Wrap's word-wider-than-the-box runaway (#419, ADR 0228, docs/re/original-bugs.md 15).
//
// Text_Wrap 0x22EC5 (owner, text, width, &count) walks the text one character at a time with
// Text_SubChars 0x22A1D (owner, text, from, to). Its frame (r7-based) holds the character i
// ([r7-36]), the line's start ([r7-44]) and the last space's character ([r7-48], 0 at first).
// When the piece [start, i) is wider than `width` it stores SubChars(start, lastSpace + 1) as a
// line (the call at 0x22FF6, return address 0x22FFB), sets start = lastSpace + 1 (read back
// from the frame after the call), and tries i again. With no space since start that line is
// empty and start stays: the loop stores empty lines without end. Its line array is 128 words
// at sp + 0x60 of its own frame, so the 515th line lands on lastSpace: start becomes a heap
// pointer + 1, and the next SubChars, which never looks at the NUL, skips that many characters
// off the end of memory (SIGBUS at 0x22A60). The device's frame is the same, so it most likely
// dies there too.
//
// The guard (every mode): called from that site with an empty line (to <= from), the line
// becomes [from, i - 1), the characters that fitted (at least one), and lastSpace becomes its
// end - 1, so start moves on to the character that did not fit. Every other call is the
// original's: a wrap the device survives wraps the same, the first line of an unbroken word
// included (lastSpace 0 cuts it after its first character).
//
// The guard is a wrapper (mt_hook_wrap) that changes the call's arguments and passes: a text
// pack's handler on the function (textpack.cpp, its UTF-8 step) or the original does the call.
#include <atomic>
#include <cstdint>
#include <cstring>

#include "maplethief/hle/abi.h"
#include "maplethief/hooks.h"
#include "maplethief/memory.h"

namespace {

using u32 = std::uint32_t;
using i32 = std::int32_t;

constexpr u32 k_too_wide_return = 0x22FFBu; // Text_Wrap's SubChars(start, lastSpace + 1)
constexpr u32 k_frame_i = 36u;              // [r7 - 36]: the character
constexpr u32 k_frame_last_space = 48u;     // [r7 - 48]: the last space's character

std::atomic<std::uint64_t> g_guarded{0};

i32 read_frame(mt_cpu *cpu, u32 offset) {
    i32 v = 0;
    std::memcpy(&v, mt_g2h(cpu, cpu->r[7] - offset, 4u), 4u);
    return v;
}

void write_frame(mt_cpu *cpu, u32 offset, i32 v) {
    std::memcpy(mt_g2h(cpu, cpu->r[7] - offset, 4u), &v, 4u);
}

int guard(mt_cpu *cpu, const mt_hook_call *) {
    const auto from = static_cast<i32>(cpu->r[2]);
    const auto to = static_cast<i32>(cpu->r[3]);
    if (cpu->r[MT_LR] == k_too_wide_return && to <= from) {
        const i32 i = read_frame(cpu, k_frame_i);
        const i32 end = i - 1 > from ? i - 1 : from + 1;
        write_frame(cpu, k_frame_last_space, end - 1);
        cpu->r[3] = static_cast<u32>(end);
        g_guarded.fetch_add(1, std::memory_order_relaxed);
    }
    return MT_HOOK_PASS;
}

} // namespace

extern "C" void mt_hooks_install_text_wrap_guard(int enabled) noexcept {
    if (enabled != 0) {
        mt_hook_wrap(MT_HOOK_FN_TEXT_SUB_CHARS, guard, nullptr, 1);
    } else {
        mt_hook_unwrap(MT_HOOK_FN_TEXT_SUB_CHARS, guard);
    }
}

extern "C" std::uint64_t mt_hooks_text_wrap_guarded(void) noexcept {
    return g_guarded.load(std::memory_order_relaxed);
}
