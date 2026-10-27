// The guest heap (ADR 0025): malloc, calloc, realloc, free and memalign in guest memory, with
// host-side bookkeeping. The structure is described at mt::mem::Heap (mem_internal.h).
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "mem/mem_internal.h"

namespace mt::mem {
namespace {

constexpr std::uint32_t k_heap_span = MT_GUEST_HEAP_END - MT_GUEST_HEAP_BASE;
static_assert(k_heap_span % k_heap_commit_step == 0u, "the heap is whole commit steps");
static_assert(k_small_refill % k_small_max == 0u, "a refill is whole blocks of the largest class");

// The usable size for a request: rounded up to MT_HEAP_ALIGN, at least one unit. False if
// the request cannot fit in the heap at all.
bool usable_for(std::uint32_t size, std::uint32_t *out) noexcept {
    if (size > k_heap_span) {
        return false;
    }
    *out = size == 0u ? MT_HEAP_ALIGN : (size + (MT_HEAP_ALIGN - 1u)) & ~(MT_HEAP_ALIGN - 1u);
    return true;
}

std::uint32_t class_of(std::uint32_t usable) noexcept { return usable / MT_HEAP_ALIGN - 1u; }

void insert_range(Heap &h, std::uint32_t addr, std::uint32_t size) {
    h.free_by_addr.emplace(addr, size);
    h.free_by_size.emplace(size, addr);
}

void erase_range(Heap &h, std::uint32_t addr, std::uint32_t size) {
    h.free_by_addr.erase(addr);
    h.free_by_size.erase({size, addr});
}

// Extends the top by `size` bytes, committing pages as needed; returns the old top, or 0.
std::uint32_t grow(mt_mem *mem, std::uint32_t size) noexcept {
    Heap &h = mem->heap;
    if (size > MT_GUEST_HEAP_END - h.top) {
        return 0u;
    }
    const std::uint32_t new_top = h.top + size;
    if (new_top > h.committed) {
        // Up to the next commit step (the heap span is a whole number of steps).
        const std::uint32_t c = (new_top - MT_GUEST_HEAP_BASE + k_heap_commit_step - 1u) /
                                    k_heap_commit_step * k_heap_commit_step +
                                MT_GUEST_HEAP_BASE;
        if (!commit_range(mem, h.committed, c)) {
            return 0u;
        }
        h.committed = c;
    }
    const std::uint32_t addr = h.top;
    h.top = new_top;
    return addr;
}

// `size` bytes (a multiple of MT_HEAP_ALIGN) from the best-fitting free range, or from the
// top; 0 if the heap is full.
std::uint32_t take(mt_mem *mem, std::uint32_t size) {
    Heap &h = mem->heap;
    auto it = h.free_by_size.lower_bound({size, 0u});
    if (it == h.free_by_size.end()) {
        return grow(mem, size);
    }
    const std::uint32_t range = it->first;
    const std::uint32_t addr = it->second;
    erase_range(h, addr, range);
    if (range > size) {
        insert_range(h, addr + size, range - size);
    }
    return addr;
}

// Returns [addr, addr + size) to the free ranges, merged with its free neighbours; a range
// that ends at the top lowers the top instead.
void give_back(Heap &h, std::uint32_t addr, std::uint32_t size) {
    auto next = h.free_by_addr.find(addr + size);
    if (next != h.free_by_addr.end()) {
        const std::uint32_t n = next->second;
        erase_range(h, addr + size, n);
        size += n;
    }
    auto prev = h.free_by_addr.lower_bound(addr);
    if (prev != h.free_by_addr.begin()) {
        --prev;
        if (prev->first + prev->second == addr) {
            const std::uint32_t p = prev->first, n = prev->second;
            erase_range(h, p, n);
            addr = p;
            size += n;
        }
    }
    if (addr + size == h.top) {
        h.top = addr;
        return;
    }
    insert_range(h, addr, size);
}

void add_live(Heap &h, std::uint32_t addr, std::uint32_t usable) {
    h.live.emplace(addr, usable);
    h.live_bytes += usable;
}

std::uint32_t alloc_locked(mt_mem *mem, std::uint32_t usable) {
    Heap &h = mem->heap;
    std::uint32_t addr;
    if (usable <= k_small_max) {
        auto &list = h.small[class_of(usable)];
        if (list.empty()) {
            std::uint32_t n = std::max(1u, k_small_refill / usable);
            std::uint32_t chunk = take(mem, n * usable);
            if (chunk == 0u && n > 1u) {
                n = 1u;
                chunk = take(mem, usable);
            }
            if (chunk == 0u) {
                return 0u;
            }
            for (std::uint32_t i = n; i-- > 0u;) {
                list.push_back(chunk + i * usable); // the lowest address is handed out first
            }
        }
        addr = list.back();
        list.pop_back();
    } else {
        addr = take(mem, usable);
        if (addr == 0u) {
            return 0u;
        }
    }
    add_live(h, addr, usable);
    return addr;
}

void free_locked(Heap &h, std::map<std::uint32_t, std::uint32_t>::iterator block) {
    const std::uint32_t addr = block->first, usable = block->second;
    h.live.erase(block);
    h.live_bytes -= usable;
    if (usable <= k_small_max) {
        h.small[class_of(usable)].push_back(addr);
    } else {
        give_back(h, addr, usable);
    }
}

// Grows the large block at addr from `old` to `usable` bytes where it lies, if the bytes
// after it are a free range or the top. Returns whether it did.
bool grow_in_place(mt_mem *mem, std::uint32_t addr, std::uint32_t old, std::uint32_t usable) {
    Heap &h = mem->heap;
    const std::uint32_t end = addr + old, need = usable - old;
    auto next = h.free_by_addr.find(end);
    if (next != h.free_by_addr.end() && next->second >= need) {
        const std::uint32_t range = next->second;
        erase_range(h, end, range);
        if (range > need) {
            insert_range(h, end + need, range - need);
        }
        return true;
    }
    return end == h.top && grow(mem, need) == end;
}

} // namespace
} // namespace mt::mem

using namespace mt::mem;

extern "C" std::uint32_t mt_heap_malloc(mt_mem *mem, std::uint32_t size) noexcept {
    std::uint32_t usable;
    if (!usable_for(size, &usable)) {
        return 0u;
    }
    std::lock_guard<std::mutex> lock(mem->heap.mu);
    return alloc_locked(mem, usable);
}

extern "C" std::uint32_t mt_heap_calloc(mt_mem *mem, std::uint32_t count,
                                        std::uint32_t size) noexcept {
    const std::uint64_t total = static_cast<std::uint64_t>(count) * size;
    if (total > UINT32_MAX) {
        return 0u;
    }
    const std::uint32_t addr = mt_heap_malloc(mem, static_cast<std::uint32_t>(total));
    if (addr != 0u) {
        std::memset(mem->base + addr, 0, static_cast<std::size_t>(total));
    }
    return addr;
}

extern "C" std::uint32_t mt_heap_realloc(mt_mem *mem, std::uint32_t addr,
                                         std::uint32_t size) noexcept {
    if (addr == 0u) {
        return mt_heap_malloc(mem, size);
    }
    std::uint32_t usable;
    if (!usable_for(size, &usable)) {
        return 0u;
    }
    Heap &h = mem->heap;
    std::lock_guard<std::mutex> lock(h.mu);
    auto block = h.live.find(addr);
    if (block == h.live.end()) {
        return 0u;
    }
    const std::uint32_t old = block->second;
    if (size == 0u) {
        // Darwin: a new minimum-sized block, and the old one is freed.
        const std::uint32_t fresh = alloc_locked(mem, usable);
        if (fresh != 0u) {
            free_locked(h, h.live.find(addr));
        }
        return fresh;
    }
    if (usable <= old) {
        if (usable > k_small_max) {
            // Shrink a large block in place; the tail goes back to the free ranges.
            block->second = usable;
            h.live_bytes -= old - usable;
            if (old > usable) {
                give_back(h, addr + usable, old - usable);
            }
        }
        return addr;
    }
    if (old > k_small_max && grow_in_place(mem, addr, old, usable)) {
        block->second = usable;
        h.live_bytes += usable - old;
        return addr;
    }
    const std::uint32_t fresh = alloc_locked(mem, usable);
    if (fresh == 0u) {
        return 0u;
    }
    std::memcpy(mem->base + fresh, mem->base + addr, old);
    free_locked(h, h.live.find(addr));
    return fresh;
}

extern "C" int mt_heap_free(mt_mem *mem, std::uint32_t addr) noexcept {
    if (addr == 0u) {
        return 0;
    }
    Heap &h = mem->heap;
    std::lock_guard<std::mutex> lock(h.mu);
    auto block = h.live.find(addr);
    if (block == h.live.end()) {
        return -1;
    }
    free_locked(h, block);
    return 0;
}

extern "C" std::uint32_t mt_heap_memalign(mt_mem *mem, std::uint32_t align,
                                          std::uint32_t size) noexcept {
    if (align == 0u || (align & (align - 1u)) != 0u) {
        return 0u;
    }
    if (align <= MT_HEAP_ALIGN) {
        return mt_heap_malloc(mem, size);
    }
    std::uint32_t usable;
    if (!usable_for(size, &usable) || align > k_heap_span ||
        usable > k_heap_span - (align - MT_HEAP_ALIGN)) {
        return 0u;
    }
    Heap &h = mem->heap;
    std::lock_guard<std::mutex> lock(h.mu);
    const std::uint32_t total = usable + align - MT_HEAP_ALIGN;
    const std::uint32_t chunk = take(mem, total);
    if (chunk == 0u) {
        return 0u;
    }
    const std::uint32_t start = (chunk + align - 1u) & ~(align - 1u);
    const std::uint32_t end = start + usable, chunk_end = chunk + total;
    // The tail first: if the chunk came from the top, giving it back lowers the top.
    if (chunk_end > end) {
        give_back(h, end, chunk_end - end);
    }
    if (start > chunk) {
        give_back(h, chunk, start - chunk);
    }
    add_live(h, start, usable);
    return start;
}

extern "C" std::uint32_t mt_heap_usable_size(mt_mem *mem, std::uint32_t addr) noexcept {
    Heap &h = mem->heap;
    std::lock_guard<std::mutex> lock(h.mu);
    auto block = h.live.find(addr);
    return block == h.live.end() ? 0u : block->second;
}

extern "C" void mt_heap_get_stats(mt_mem *mem, mt_heap_stats *out) noexcept {
    Heap &h = mem->heap;
    std::lock_guard<std::mutex> lock(h.mu);
    out->live_blocks = static_cast<std::uint32_t>(h.live.size());
    out->live_bytes = static_cast<std::uint32_t>(h.live_bytes);
    out->top = h.top;
    out->committed = h.committed;
}

extern "C" std::uint32_t mt_mem_guest_alloc(void *user, std::uint32_t size,
                                            std::uint32_t align) noexcept {
    auto *mem = static_cast<mt_mem *>(user);
    const std::uint32_t addr =
        mt_heap_memalign(mem, align < MT_HEAP_ALIGN ? MT_HEAP_ALIGN : align, size);
    if (addr != 0u) {
        std::memset(mem->base + addr, 0, size);
    }
    return addr;
}

extern "C" int mt_mem_guest_free(void *user, std::uint32_t addr) noexcept {
    return mt_heap_free(static_cast<mt_mem *>(user), addr);
}
