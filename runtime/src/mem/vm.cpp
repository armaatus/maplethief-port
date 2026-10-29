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
