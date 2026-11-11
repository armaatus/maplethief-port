// Host virtual memory (ADR 0025): see vm.h.
#include "mem/vm.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__SWITCH__)
#include <malloc.h> // memalign
#include <switch.h>

#include <cstdio>
#include <cstdlib>
#include <mutex>

#include "mem/commit_map.h"
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace mt::vm {

namespace {

// Whether a 64-bit size is representable on this host (always, on the 64-bit hosts we ship).
constexpr bool fits(std::uint64_t size) noexcept {
    if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
        return size <= static_cast<std::uint64_t>(static_cast<std::size_t>(-1));
    } else {
        return true;
    }
}

} // namespace

#if defined(_WIN32)

std::size_t page_size() noexcept {
    static const std::size_t size = [] {
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        return static_cast<std::size_t>(info.dwPageSize);
    }();
    return size;
}

std::uint8_t *reserve(std::uint64_t size) noexcept {
    if (!fits(size)) {
        return nullptr;
    }
    void *p = VirtualAlloc(nullptr, static_cast<SIZE_T>(size), MEM_RESERVE, PAGE_NOACCESS);
    return static_cast<std::uint8_t *>(p);
}

void release(std::uint8_t *base, std::uint64_t) noexcept {
    if (base != nullptr) {
        VirtualFree(base, 0, MEM_RELEASE);
    }
}

bool commit(std::uint8_t *addr, std::size_t size) noexcept {
    return VirtualAlloc(addr, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

bool decommit(std::uint8_t *addr, std::size_t size) noexcept {
    return VirtualFree(addr, size, MEM_DECOMMIT) != 0;
}

bool protect(std::uint8_t *addr, std::size_t size, Prot prot) noexcept {
    DWORD flags = PAGE_NOACCESS;
    if (prot == Prot::read) {
        flags = PAGE_READONLY;
    } else if (prot == Prot::read_write) {
        flags = PAGE_READWRITE;
    }
    DWORD old = 0;
    return VirtualProtect(addr, size, flags, &old) != 0;
}

#elif defined(__SWITCH__)

// Horizon (ADR 0025's Horizon addendum, #236). newlib has no mmap, and memory mapped with
// svcMapMemory cannot be reprotected, so:
//   reserve   a libnx virtmem reservation in the stack region, the one region svcMapMemory maps
//             into; nothing is mapped there yet, so every page of it faults
//   commit    commit_map.h: page-aligned blocks of the newlib heap (which the loader handed all
//             of the process's memory, in applet mode as in title takeover) mapped into the
//             reservation with svcMapMemory, then zeroed
//   decommit  svcUnmapMemory, and the heap block goes back to malloc
//   protect   an honest no-op: sealed __TEXT stays writable. Uncommitted pages (page zero, the
//             stack guards, unused heap) stay unmapped, so they still fault.
// Only the arena is ever reserved: the 4 GiB reservation of the unchecked model (ADR 0139:
// MAPLETHIEF_MEM_MODEL=reserved is a configure error here) is refused.

namespace {

constexpr std::size_t k_page = 0x1000;
constexpr std::uint64_t k_max_reservation = std::uint64_t{1} << 30;
// The largest heap block one commit takes: a commit of 64 MiB needs 8 of them, not one
// contiguous 64 MiB of a heap the rest of the program also allocates from.
constexpr std::size_t k_max_block = std::size_t{8} << 20;

void *heap_alloc(void *, std::size_t size) { return memalign(k_page, size); }
void heap_free(void *, void *block) { std::free(block); }
bool map_heap(void *, std::uintptr_t dst, std::uintptr_t src, std::size_t size) {
    return R_SUCCEEDED(
        svcMapMemory(reinterpret_cast<void *>(dst), reinterpret_cast<void *>(src), size));
}
bool unmap_heap(void *, std::uintptr_t dst, std::uintptr_t src, std::size_t size) {
    return R_SUCCEEDED(
        svcUnmapMemory(reinterpret_cast<void *>(dst), reinterpret_cast<void *>(src), size));
}

struct Reservation {
    std::uint8_t *base = nullptr;
    std::uint64_t size = 0;
    VirtmemReservation *rv = nullptr;
};

struct Horizon {
    std::mutex mu;
    CommitMap commits{CommitOps{nullptr, heap_alloc, heap_free, map_heap, unmap_heap}, k_page,
                      k_max_block};
    Reservation reservations[16];
    bool said_protect = false;
};

Horizon &horizon() {
    static Horizon h;
    return h;
}

} // namespace

std::size_t page_size() noexcept { return k_page; }

std::uint8_t *reserve(std::uint64_t size) noexcept {
    if (!fits(size) || size == 0u || size > k_max_reservation) {
        return nullptr;
    }
    Horizon &h = horizon();
    std::lock_guard<std::mutex> lock(h.mu);
    Reservation *slot = nullptr;
    for (Reservation &r : h.reservations) {
        if (r.base == nullptr) {
            slot = &r;
            break;
        }
    }
    if (slot == nullptr) {
        return nullptr;
    }
    const auto n = static_cast<std::size_t>(size);
    virtmemLock();
    void *p = virtmemFindStack(n, k_page * 16u); // 64 KiB of unmapped space on either side
    VirtmemReservation *rv = p != nullptr ? virtmemAddReservation(p, n) : nullptr;
    virtmemUnlock();
    if (rv == nullptr) {
        return nullptr;
    }
    *slot = Reservation{static_cast<std::uint8_t *>(p), size, rv};
    return slot->base;
}

void release(std::uint8_t *base, std::uint64_t size) noexcept {
    if (base == nullptr) {
        return;
    }
    Horizon &h = horizon();
    std::lock_guard<std::mutex> lock(h.mu);
    h.commits.decommit(reinterpret_cast<std::uintptr_t>(base), static_cast<std::size_t>(size));
    for (Reservation &r : h.reservations) {
        if (r.base == base) {
            virtmemLock();
            virtmemRemoveReservation(r.rv);
            virtmemUnlock();
            r = Reservation{};
        }
    }
}

bool commit(std::uint8_t *addr, std::size_t size) noexcept {
    Horizon &h = horizon();
    std::lock_guard<std::mutex> lock(h.mu);
    return h.commits.commit(reinterpret_cast<std::uintptr_t>(addr), size);
}

bool decommit(std::uint8_t *addr, std::size_t size) noexcept {
    Horizon &h = horizon();
    std::lock_guard<std::mutex> lock(h.mu);
    return h.commits.decommit(reinterpret_cast<std::uintptr_t>(addr), size);
}

bool protect(std::uint8_t *, std::size_t, Prot prot) noexcept {
    Horizon &h = horizon();
    std::lock_guard<std::mutex> lock(h.mu);
    if (prot != Prot::read_write && !h.said_protect) {
        h.said_protect = true;
        std::fprintf(stderr, "guest memory: Horizon cannot reprotect mapped memory; sealed "
                             "__TEXT stays writable (ADR 0025)\n");
    }
    return true;
}

#else

namespace {

#if defined(MAP_NORESERVE)
constexpr int k_reserve_flags = MAP_PRIVATE | MAP_ANON | MAP_NORESERVE;
#else
constexpr int k_reserve_flags = MAP_PRIVATE | MAP_ANON;
#endif

} // namespace

std::size_t page_size() noexcept {
    static const std::size_t size = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    return size;
}

std::uint8_t *reserve(std::uint64_t size) noexcept {
    if (!fits(size)) {
        return nullptr;
    }
    void *p = mmap(nullptr, static_cast<std::size_t>(size), PROT_NONE, k_reserve_flags, -1, 0);
    return p == MAP_FAILED ? nullptr : static_cast<std::uint8_t *>(p);
}

void release(std::uint8_t *base, std::uint64_t size) noexcept {
    if (base != nullptr) {
        munmap(base, static_cast<std::size_t>(size));
    }
}

bool commit(std::uint8_t *addr, std::size_t size) noexcept {
    return mprotect(addr, size, PROT_READ | PROT_WRITE) == 0;
}

bool decommit(std::uint8_t *addr, std::size_t size) noexcept {
    // A fresh anonymous mapping in place: no access, and zero pages when committed again.
    void *p = mmap(addr, size, PROT_NONE, k_reserve_flags | MAP_FIXED, -1, 0);
    return p != MAP_FAILED;
}

bool protect(std::uint8_t *addr, std::size_t size, Prot prot) noexcept {
    int flags = PROT_NONE;
    if (prot == Prot::read) {
        flags = PROT_READ;
    } else if (prot == Prot::read_write) {
        flags = PROT_READ | PROT_WRITE;
    }
    return mprotect(addr, size, flags) == 0;
}

#endif

} // namespace mt::vm
