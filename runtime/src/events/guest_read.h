// Checked reads of guest memory for the game and text events (events.cpp, text_events.cpp): a
// read happens only where the guest has memory that is there, an image segment or the committed
// heap (as the input module reads, runtime/src/input/input.cpp). A synthetic guest may have no
// image, and its arena's other pages are not readable. Nothing here writes.
#ifndef MAPLETHIEF_RUNTIME_EVENTS_GUEST_READ_H
#define MAPLETHIEF_RUNTIME_EVENTS_GUEST_READ_H

#include <cstdint>
#include <cstring>
#include <string>

#include "maplethief/memory.h"
#include "maplethief/recomp/recomp.h"

namespace mt_events {

// The end of the readable region that holds `addr` (an image segment or the committed heap), or
// 0 if `addr` is in none.
inline std::uint64_t readable_end(const mt_cpu *cpu, std::uint32_t addr) {
    if (addr == 0 || addr >= cpu->mem_size) {
        return 0;
    }
    mt_mem *mem = mt_mem_from_cpu(cpu);
    if (mem == nullptr) {
        return 0;
    }
    std::uint32_t n = 0;
    const mt_segment *segs = mt_mem_segments(mem, &n);
    for (std::uint32_t i = 0; segs != nullptr && i < n; i++) {
        const std::uint64_t end = static_cast<std::uint64_t>(segs[i].vmaddr) + segs[i].vmsize;
        if (addr >= segs[i].vmaddr && addr < end) {
            return end < cpu->mem_size ? end : cpu->mem_size;
        }
    }
    mt_heap_stats heap{};
    mt_heap_get_stats(mem, &heap);
    if (addr >= MT_GUEST_HEAP_BASE && addr < heap.committed) {
        return heap.committed < cpu->mem_size ? heap.committed : cpu->mem_size;
    }
    return 0;
}

// Whether [addr, addr + n) is readable.
inline bool readable(const mt_cpu *cpu, std::uint32_t addr, std::uint32_t n = 4) {
    return static_cast<std::uint64_t>(addr) + n <= readable_end(cpu, addr);
}

inline bool rd32(const mt_cpu *cpu, std::uint32_t addr, std::int32_t *out) {
    if (!readable(cpu, addr)) {
        return false;
    }
    std::memcpy(out, cpu->mem + addr, 4);
    return true;
}

// A pointer field: 0 when it is not readable.
inline std::uint32_t rdptr(const mt_cpu *cpu, std::uint32_t addr) {
    std::int32_t v = 0;
    return rd32(cpu, addr, &v) ? static_cast<std::uint32_t>(v) : 0u;
}

// The bytes of the C string at `addr`, up to its NUL or `max` bytes (a fixed-width field may
// have no NUL); false if `addr` is not readable or the string runs out of its region first.
inline bool rd_cstr(const mt_cpu *cpu, std::uint32_t addr, std::size_t max, std::string *out) {
    const std::uint64_t end = readable_end(cpu, addr);
    if (end == 0) {
        return false;
    }
    out->clear();
    for (std::uint64_t a = addr; out->size() < max; a++) {
        if (a >= end) {
            return false;
        }
        const char c = static_cast<char>(cpu->mem[a]);
        if (c == 0) {
            return true;
        }
        out->push_back(c);
    }
    return true;
}

} // namespace mt_events

#endif // MAPLETHIEF_RUNTIME_EVENTS_GUEST_READ_H
