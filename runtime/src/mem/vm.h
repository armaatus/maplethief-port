// Host virtual memory for the guest address space (ADR 0025): reserve, commit, protect.
// POSIX mmap/mprotect, Windows VirtualAlloc/VirtualProtect, or on the Nintendo Switch a libnx
// virtmem reservation committed with svcMapMemory (commit_map.h; protect is a no-op there);
// nothing else in runtime/src/mem/ is platform-specific. Every address and size is a multiple
// of page_size().
#ifndef MAPLETHIEF_RUNTIME_MEM_VM_H
#define MAPLETHIEF_RUNTIME_MEM_VM_H

#include <cstddef>
#include <cstdint>

namespace mt::vm {

enum class Prot { none, read, read_write };

// The host page size (the protection granularity).
std::size_t page_size() noexcept;

// Reserves `size` bytes of address space with no access and no commit charge; nullptr if
// the host refuses.
std::uint8_t *reserve(std::uint64_t size) noexcept;

// Releases a whole reservation.
void release(std::uint8_t *base, std::uint64_t size) noexcept;

// Makes reserved pages read-write. Pages committed for the first time read as zero.
bool commit(std::uint8_t *addr, std::size_t size) noexcept;

// Returns committed pages to reserved, no-access pages; their contents are discarded, so
// they read as zero once committed again.
bool decommit(std::uint8_t *addr, std::size_t size) noexcept;

// Changes the protection of committed pages.
bool protect(std::uint8_t *addr, std::size_t size, Prot prot) noexcept;

} // namespace mt::vm

#endif // MAPLETHIEF_RUNTIME_MEM_VM_H
