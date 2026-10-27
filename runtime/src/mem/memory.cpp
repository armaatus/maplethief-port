// The guest address space (ADR 0025): reservation, image mapping, guest stacks, diagnostics
// and the memory fault handler of generated code. See maplethief/memory.h; the heap is
// heap.cpp, the host calls vm.cpp.
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <new>

#include "mem/mem_internal.h"
#include "mem/vm.h"

static_assert(MT_GUEST_HEAP_BASE == MT_GUEST_IMAGE_END &&
                  MT_GUEST_STACK_AREA == MT_GUEST_HEAP_END &&
                  MT_GUEST_ARENA_SIZE == MT_GUEST_STACK_AREA_END,
              "the layout regions are contiguous");
static_assert(MT_STACK_GUARD % MT_STACK_GRANULE == 0u &&
                  MT_STACK_MAIN_SIZE % MT_STACK_GRANULE == 0u,
              "stacks are whole granules");

namespace mt::mem {
namespace {

constexpr std::uint64_t k_full_size = (std::uint64_t{1} << 32) + MT_GUEST_RESERVE_GUARD;
constexpr std::uint32_t k_lc_segment = 0x1u;
constexpr std::uint32_t k_mh_magic = 0xFEEDFACEu;
constexpr std::uint32_t k_cpu_arm = 12u;

// The live address spaces, for mt_mem_from_cpu.
constexpr int k_max_spaces = 16;
std::atomic<mt_mem *> g_spaces[k_max_spaces];
std::mutex g_spaces_mu;

thread_local mt_mem_fault_info t_fault;
thread_local bool t_has_fault = false;

std::uint32_t floor_page(const mt_mem *mem, std::uint32_t addr) { return addr & ~(mem->page - 1u); }

std::uint64_t ceil_page(const mt_mem *mem, std::uint64_t addr) {
    return (addr + mem->page - 1u) & ~static_cast<std::uint64_t>(mem->page - 1u);
}

std::uint32_t rd32(const std::uint8_t *p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}

int fail(const char *what) {
    log_line("guest memory: %s", what);
    return -1;
}

const mt_segment *segment_at(const mt_mem *mem, std::uint32_t addr) {
    for (const mt_segment &s : mem->segments) {
        if (addr >= s.vmaddr && addr - s.vmaddr < s.vmsize) {
            return &s;
        }
    }
    return nullptr;
}

// Writes a description of a heap address (the heap lock is only tried, never waited for).
std::uint32_t describe_heap(mt_mem *mem, std::uint32_t addr, char *buf, std::size_t n) {
    Heap &h = mem->heap;
    std::unique_lock<std::mutex> lock(h.mu, std::try_to_lock);
    if (!lock.owns_lock()) {
        std::snprintf(buf, n, "heap (busy, no detail)");
        return MT_REGION_HEAP;
    }
    if (addr >= h.top) {
        std::snprintf(buf, n, "heap area above the heap top 0x%08x", h.top);
        return MT_REGION_HEAP_UNUSED;
    }
    auto block = h.live.upper_bound(addr);
    if (block != h.live.begin()) {
        --block;
        if (addr - block->first < block->second) {
            std::snprintf(buf, n, "heap block 0x%08x+0x%x (%u bytes)", block->first,
                          addr - block->first, block->second);
            return MT_REGION_HEAP;
        }
    }
    std::snprintf(buf, n, "heap, not in a live block");
    return MT_REGION_HEAP;
}

std::uint32_t describe_stack(mt_mem *mem, std::uint32_t addr, char *buf, std::size_t n) {
    std::unique_lock<std::mutex> lock(mem->stacks_mu, std::try_to_lock);
    if (!lock.owns_lock()) {
        std::snprintf(buf, n, "stack area (busy, no detail)");
        return MT_REGION_STACK;
    }
    for (const Stack &s : mem->stacks) {
        if (addr >= s.guard && addr < s.top) {
            const bool guard = addr < s.base;
            std::snprintf(buf, n, "%sguest stack 0x%08x-0x%08x", guard ? "guard below " : "",
                          s.base, s.top);
            return guard ? MT_REGION_STACK_GUARD : MT_REGION_STACK;
        }
    }
    std::snprintf(buf, n, "stack area, no stack");
    return MT_REGION_STACK_UNUSED;
}

} // namespace

bool commit_range(mt_mem *mem, std::uint32_t lo, std::uint32_t hi) noexcept {
    if (hi <= lo) {
        return true;
    }
    if (!vm::commit(mem->base + lo, hi - lo)) {
        log_line("guest memory: cannot commit 0x%08x-0x%08x", lo, hi);
        return false;
    }
    return true;
}

void log_line(const char *fmt, ...) noexcept {
    std::va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

} // namespace mt::mem

using namespace mt::mem;

// --- the address space ------------------------------------------------------------------

extern "C" mt_mem_config mt_mem_config_default(void) noexcept {
    return mt_mem_config{MT_MEM_RESERVE_AUTO};
}

extern "C" std::uint32_t mt_mem_host_page_size(void) noexcept {
    return static_cast<std::uint32_t>(mt::vm::page_size());
}

extern "C" mt_mem *mt_mem_create(const mt_mem_config *cfg) noexcept {
    const mt_mem_config c = cfg != nullptr ? *cfg : mt_mem_config_default();
    const std::size_t page = mt::vm::page_size();
    if (page == 0u || (page & (page - 1u)) != 0u || page > MT_GUEST_RESERVE_GUARD) {
        log_line("guest memory: unsupported host page size %zu", page);
        return nullptr;
    }
    std::uint8_t *base = nullptr;
    std::uint64_t reserved = 0;
    if (c.reservation != MT_MEM_RESERVE_ARENA && sizeof(void *) >= 8u) {
        base = mt::vm::reserve(k_full_size);
        reserved = k_full_size;
    }
    if (base == nullptr && c.reservation == MT_MEM_RESERVE_FULL) {
        log_line("guest memory: cannot reserve 4 GiB of address space, which code compiled "
                 "with MT_MEM_CHECKED 0 needs (ADR 0025)");
        return nullptr;
    }
    if (base == nullptr) {
        base = mt::vm::reserve(MT_GUEST_ARENA_SIZE);
        reserved = MT_GUEST_ARENA_SIZE;
    }
    if (base == nullptr) {
        log_line("guest memory: cannot reserve the %u MiB guest arena", MT_GUEST_ARENA_SIZE >> 20);
        return nullptr;
    }
    auto *mem = new (std::nothrow) mt_mem;
    if (mem == nullptr) {
        mt::vm::release(base, reserved);
        return nullptr;
    }
    mem->base = base;
    mem->reserved = reserved;
    mem->full = reserved == k_full_size;
    mem->page = static_cast<std::uint32_t>(page);
    {
        std::lock_guard<std::mutex> lock(g_spaces_mu);
        for (auto &slot : g_spaces) {
            if (slot.load(std::memory_order_relaxed) == nullptr) {
                slot.store(mem, std::memory_order_release);
                return mem;
            }
        }
    }
    log_line("guest memory: more than %d live address spaces", k_max_spaces);
    mt::vm::release(base, reserved);
    delete mem;
    return nullptr;
}

extern "C" void mt_mem_destroy(mt_mem *mem) noexcept {
    if (mem == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_spaces_mu);
        for (auto &slot : g_spaces) {
            if (slot.load(std::memory_order_relaxed) == mem) {
                slot.store(nullptr, std::memory_order_release);
            }
        }
    }
    mt::vm::release(mem->base, mem->reserved);
    delete mem;
}

extern "C" std::uint8_t *mt_mem_base(const mt_mem *mem) noexcept { return mem->base; }

extern "C" int mt_mem_is_full_reservation(const mt_mem *mem) noexcept { return mem->full ? 1 : 0; }

extern "C" void mt_mem_attach(mt_mem *mem, mt_cpu *cpu) noexcept {
    cpu->mem = mem->base;
    cpu->mem_size = MT_GUEST_ARENA_SIZE;
}

extern "C" mt_mem *mt_mem_from_cpu(const mt_cpu *cpu) noexcept {
    for (auto &slot : g_spaces) {
        mt_mem *mem = slot.load(std::memory_order_acquire);
        if (mem != nullptr && mem->base == cpu->mem) {
            return mem;
        }
    }
    return nullptr;
}

// --- the image --------------------------------------------------------------------------

extern "C" int mt_macho_segments(const std::uint8_t *file, std::size_t size, mt_segment *out,
                                 std::uint32_t max, std::uint32_t *count) noexcept {
    *count = 0;
    constexpr std::size_t header = 28u;
    if (file == nullptr || size < header) {
        return fail("Mach-O: shorter than its header");
    }
    if (rd32(file) != k_mh_magic || rd32(file + 4) != k_cpu_arm) {
        return fail("Mach-O: not a thin 32-bit little-endian ARM Mach-O");
    }
    const std::uint32_t ncmds = rd32(file + 16), sizeofcmds = rd32(file + 20);
    if (sizeofcmds > size - header) {
        return fail("Mach-O: load commands run past the end of the file");
    }
    std::size_t off = header;
    const std::size_t end = header + sizeofcmds;
    for (std::uint32_t i = 0; i < ncmds; i++) {
        if (end - off < 8u) {
            return fail("Mach-O: more load commands than sizeofcmds holds");
        }
        const std::uint32_t cmd = rd32(file + off), cmdsize = rd32(file + off + 4);
        if (cmdsize < 8u || cmdsize % 4u != 0u || cmdsize > end - off) {
            return fail("Mach-O: bad load command size");
        }
        if (cmd == k_lc_segment) {
            if (cmdsize < 56u) {
                return fail("Mach-O: LC_SEGMENT shorter than 56 bytes");
            }
            if (*count >= max) {
                return fail("Mach-O: too many segments");
            }
            const std::uint8_t *p = file + off;
            mt_segment s{};
            std::memcpy(s.name, p + 8, 16);
            s.name[16] = '\0';
            s.vmaddr = rd32(p + 24);
            s.vmsize = rd32(p + 28);
            s.fileoff = rd32(p + 32);
            s.filesize = rd32(p + 36);
            s.initprot = rd32(p + 44);
            if (s.filesize > s.vmsize) {
                return fail("Mach-O: segment filesize larger than vmsize");
            }
            if (static_cast<std::uint64_t>(s.fileoff) + s.filesize > size) {
                return fail("Mach-O: segment file range runs past the end of the file");
            }
            if (static_cast<std::uint64_t>(s.vmaddr) + s.vmsize > (std::uint64_t{1} << 32)) {
                return fail("Mach-O: segment beyond 4 GiB");
            }
            out[(*count)++] = s;
        }
        off += cmdsize;
    }
    return 0;
}

extern "C" int mt_mem_map_segment(mt_mem *mem, const mt_segment *seg, const std::uint8_t *file,
                                  std::size_t size) noexcept {
    if (seg->vmsize == 0u || (seg->vmaddr == 0u && seg->initprot == 0u)) {
        return 0; // __PAGEZERO, or nothing to map
    }
    if (mem->sealed) {
        return fail("cannot map a segment after mt_mem_seal_image");
    }
    const std::uint64_t vmend = static_cast<std::uint64_t>(seg->vmaddr) + seg->vmsize;
    if (seg->vmaddr < MT_GUEST_PAGE_ZERO_END || vmend > MT_GUEST_IMAGE_END) {
        log_line("guest memory: segment %.16s 0x%08x-0x%08llx is outside the image area", seg->name,
                 seg->vmaddr, static_cast<unsigned long long>(vmend));
        return -1;
    }
    if (seg->filesize > seg->vmsize ||
        static_cast<std::uint64_t>(seg->fileoff) + seg->filesize > size ||
        (seg->filesize > 0u && file == nullptr)) {
        return fail("segment file range is not inside the file");
    }
    for (const mt_segment &s : mem->segments) {
        if (seg->vmaddr < static_cast<std::uint64_t>(s.vmaddr) + s.vmsize && s.vmaddr < vmend) {
            log_line("guest memory: segment %.16s overlaps %s", seg->name, s.name);
            return -1;
        }
    }
    const std::uint32_t lo = floor_page(mem, seg->vmaddr);
    const auto hi = static_cast<std::uint32_t>(ceil_page(mem, vmend));
    if (!commit_range(mem, lo, hi)) {
        return -1;
    }
    if (seg->filesize > 0u) {
        std::memcpy(mem->base + seg->vmaddr, file + seg->fileoff, seg->filesize);
    }
    mt_segment copy = *seg;
    copy.name[16] = '\0';
    mem->segments.push_back(copy);
    return 0;
}

extern "C" int mt_mem_map_macho(mt_mem *mem, const std::uint8_t *file, std::size_t size) noexcept {
    mt_segment segs[MT_MAX_SEGMENTS];
    std::uint32_t n = 0;
    if (mt_macho_segments(file, size, segs, MT_MAX_SEGMENTS, &n) != 0) {
        return -1;
    }
    for (std::uint32_t i = 0; i < n; i++) {
        if (mt_mem_map_segment(mem, &segs[i], file, size) != 0) {
            return -1;
        }
    }
    return 0;
}

extern "C" int mt_mem_seal_image(mt_mem *mem) noexcept {
    // The protection of each committed host page: the union of the segments in it.
    std::map<std::uint32_t, std::uint32_t> pages;
    for (const mt_segment &s : mem->segments) {
        const std::uint64_t end = ceil_page(mem, static_cast<std::uint64_t>(s.vmaddr) + s.vmsize);
        for (std::uint64_t p = floor_page(mem, s.vmaddr); p < end; p += mem->page) {
            pages[static_cast<std::uint32_t>(p)] |= s.initprot;
        }
    }
    int rc = 0;
    auto it = pages.begin();
    while (it != pages.end()) {
        // A run of contiguous pages with the same protection.
        const std::uint32_t lo = it->first, prot = it->second;
        std::uint32_t hi = lo + mem->page;
        for (++it; it != pages.end() && it->first == hi && it->second == prot; ++it) {
            hi += mem->page;
        }
        mt::vm::Prot p = mt::vm::Prot::none;
        if ((prot & MT_PROT_WRITE) != 0u) {
            p = mt::vm::Prot::read_write;
        } else if ((prot & (MT_PROT_READ | MT_PROT_EXECUTE)) != 0u) {
            p = mt::vm::Prot::read;
        }
        if (!mt::vm::protect(mem->base + lo, hi - lo, p)) {
            log_line("guest memory: cannot protect 0x%08x-0x%08x", lo, hi);
            rc = -1;
        }
    }
    mem->sealed = true;
    return rc;
}

extern "C" const mt_segment *mt_mem_segments(const mt_mem *mem, std::uint32_t *count) noexcept {
    *count = static_cast<std::uint32_t>(mem->segments.size());
    return mem->segments.data();
}

// --- guest stacks -----------------------------------------------------------------------

extern "C" int mt_stack_create(mt_mem *mem, std::uint32_t size, mt_stack *out) noexcept {
    constexpr std::uint32_t area = MT_GUEST_STACK_AREA_END - MT_GUEST_STACK_AREA;
    if (size == 0u) {
        size = MT_STACK_THREAD_SIZE;
    }
    if (size > area - MT_STACK_GUARD) {
        return -1;
    }
    size = (size + MT_STACK_GRANULE - 1u) & ~(MT_STACK_GRANULE - 1u);
    const std::uint32_t need = MT_STACK_GUARD + size;
    std::lock_guard<std::mutex> lock(mem->stacks_mu);
    // First fit between the existing stacks (sorted by address).
    std::uint32_t at = MT_GUEST_STACK_AREA;
    auto pos = mem->stacks.begin();
    for (; pos != mem->stacks.end(); ++pos) {
        if (pos->guard - at >= need) {
            break;
        }
        at = pos->top;
    }
    if (pos == mem->stacks.end() && MT_GUEST_STACK_AREA_END - at < need) {
        return -1;
    }
    const Stack s{at, at + MT_STACK_GUARD, at + need};
    if (!commit_range(mem, s.base, s.top)) {
        return -1;
    }
    mem->stacks.insert(pos, s);
    *out = mt_stack{s.guard, s.base, s.top};
    return 0;
}

extern "C" int mt_stack_destroy(mt_mem *mem, const mt_stack *stack) noexcept {
    std::lock_guard<std::mutex> lock(mem->stacks_mu);
    for (auto it = mem->stacks.begin(); it != mem->stacks.end(); ++it) {
        if (it->guard == stack->guard && it->base == stack->base && it->top == stack->top) {
            const bool ok = mt::vm::decommit(mem->base + it->base, it->top - it->base);
            mem->stacks.erase(it);
            return ok ? 0 : -1;
        }
    }
    return -1;
}

// --- diagnostics ------------------------------------------------------------------------

extern "C" std::uint32_t mt_mem_describe(mt_mem *mem, std::uint32_t addr, char *buf,
                                         std::size_t buf_size) noexcept {
    char scratch[1];
    if (buf == nullptr || buf_size == 0u) {
        buf = scratch;
        buf_size = sizeof scratch;
    }
    if (addr >= MT_GUEST_ARENA_SIZE) {
        std::snprintf(buf, buf_size, "outside guest memory (it ends at 0x%08x)",
                      MT_GUEST_ARENA_SIZE);
        return MT_REGION_OUTSIDE;
    }
    if (addr < MT_GUEST_PAGE_ZERO_END) {
        std::snprintf(buf, buf_size, "page zero (a null pointer)");
        return MT_REGION_PAGE_ZERO;
    }
    if (addr < MT_GUEST_IMAGE_END) {
        const mt_segment *s = mem != nullptr ? segment_at(mem, addr) : nullptr;
        if (s == nullptr) {
            std::snprintf(buf, buf_size, "image area, no segment");
            return MT_REGION_UNMAPPED;
        }
        std::snprintf(buf, buf_size, "%s+0x%x", s->name, addr - s->vmaddr);
        return MT_REGION_IMAGE;
    }
    if (addr < MT_GUEST_HEAP_END) {
        if (mem == nullptr) {
            std::snprintf(buf, buf_size, "heap");
            return MT_REGION_HEAP;
        }
        return describe_heap(mem, addr, buf, buf_size);
    }
    if (mem == nullptr) {
        std::snprintf(buf, buf_size, "stack area");
        return MT_REGION_STACK;
    }
    return describe_stack(mem, addr, buf, buf_size);
}

extern "C" int mt_mem_last_fault(mt_mem_fault_info *out) noexcept {
    if (!t_has_fault) {
        return -1;
    }
    *out = t_fault;
    return 0;
}

extern "C" void mt_mem_fault(mt_cpu *cpu, std::uint32_t addr, std::uint32_t size,
                             std::uint32_t pc) noexcept {
    char where[160];
    std::uint32_t region;
    if (static_cast<std::uint64_t>(addr) + size > cpu->mem_size) {
        std::snprintf(where, sizeof where, "outside guest memory (it ends at 0x%08llx)",
                      static_cast<unsigned long long>(cpu->mem_size));
        region = MT_REGION_OUTSIDE;
    } else {
        region = mt_mem_describe(mt_mem_from_cpu(cpu), addr, where, sizeof where);
    }
    t_fault = mt_mem_fault_info{addr, size, pc, region};
    t_has_fault = true;
    log_line("guest memory fault: %u-byte access to 0x%08x at pc 0x%08x: %s", size, addr, pc,
             where);
    cpu->r[MT_PC] = pc;
    mt_trap(cpu, MT_TRAP_MEMORY, pc, addr);
}

extern "C" void mt_mem_bad_host_pointer(const mt_cpu *cpu, const void *host) noexcept {
    log_line("runtime bug: host pointer %p is not in guest memory (%p, 0x%llx bytes)", host,
             static_cast<const void *>(cpu->mem), static_cast<unsigned long long>(cpu->mem_size));
    std::abort();
}
