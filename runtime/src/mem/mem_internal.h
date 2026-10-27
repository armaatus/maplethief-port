// The guest address space object behind maplethief/memory.h (ADR 0025). Shared by
// memory.cpp (reservation, image, stacks, diagnostics, faults) and heap.cpp (the guest heap).
#ifndef MAPLETHIEF_RUNTIME_MEM_MEM_INTERNAL_H
#define MAPLETHIEF_RUNTIME_MEM_MEM_INTERNAL_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

#include "maplethief/memory.h"

namespace mt::mem {

// Small blocks: usable sizes 16, 32, ... k_small_max, one free list per size class.
constexpr std::uint32_t k_small_max = 1024u;
constexpr std::uint32_t k_small_classes = k_small_max / MT_HEAP_ALIGN;
// A size class that runs dry takes this many bytes of new blocks at once.
constexpr std::uint32_t k_small_refill = 16u * 1024u;
// The heap commits pages in steps of this size (a multiple of every host page size).
constexpr std::uint32_t k_heap_commit_step = 1024u * 1024u;

// The guest heap. Bookkeeping is on the host; guest memory holds only the blocks.
//
// Blocks of up to k_small_max bytes come from per-size-class free lists, refilled by carving
// k_small_refill bytes into blocks of one class. Larger blocks (and refills) come from the
// free ranges, best fit with the lowest address on ties, or else from the top of the heap,
// which grows and commits pages as needed. Freed large blocks merge with free neighbours,
// and a free range that reaches the top lowers it. Small blocks stay in their class.
struct Heap {
    std::mutex mu;
    std::uint32_t top = MT_GUEST_HEAP_BASE;       // end of the space handed out so far
    std::uint32_t committed = MT_GUEST_HEAP_BASE; // end of the committed pages
    std::map<std::uint32_t, std::uint32_t> live;  // block address -> usable size
    std::uint64_t live_bytes = 0;
    std::map<std::uint32_t, std::uint32_t> free_by_addr;            // free range -> size
    std::set<std::pair<std::uint32_t, std::uint32_t>> free_by_size; // (size, address)
    std::vector<std::uint32_t> small[k_small_classes]; // free small blocks, popped from back
};

struct Stack {
    std::uint32_t guard;
    std::uint32_t base;
    std::uint32_t top;
};

} // namespace mt::mem

struct mt_mem {
    std::uint8_t *base = nullptr;     // host address of guest 0
    std::uint64_t reserved = 0;       // bytes reserved at base
    bool full = false;                // 4 GiB + guard (else the arena alone)
    std::uint32_t page = 0;           // host page size
    std::vector<mt_segment> segments; // mapped image segments
    bool sealed = false;
    mt::mem::Heap heap;
    std::mutex stacks_mu;
    std::vector<mt::mem::Stack> stacks; // sorted by address
};

namespace mt::mem {

// Commits the guest range [lo, hi) (page-aligned) read-write; false on failure.
bool commit_range(mt_mem *mem, std::uint32_t lo, std::uint32_t hi) noexcept;

#if defined(__GNUC__) || defined(__clang__)
#define MT_MEM_PRINTF __attribute__((format(printf, 1, 2)))
#else
#define MT_MEM_PRINTF
#endif

// Logs one printf-formatted line to stderr.
MT_MEM_PRINTF void log_line(const char *fmt, ...) noexcept;

} // namespace mt::mem

#endif // MAPLETHIEF_RUNTIME_MEM_MEM_INTERNAL_H
