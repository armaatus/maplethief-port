// Guest memory (ADR 0025, #42): the reservation, the image at its link-time addresses, the
// guest heap, guest stacks and their guards, the pointer helpers, and the bounds-check trap
// with the guest PC. Synthetic images only; memory_real_test.cpp maps the real slice.
#include "maplethief/memory.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "guest_trap.h"
#include "host_fault.h"

#if !MT_MEM_CHECKED
#error "these tests exercise the bounds-checked model (MT_MEM_CHECKED 1)"
#endif

namespace {

using mt_test::Access;
using mt_test::faults;

// --- a synthetic Mach-O with the armv7 slice's segment layout (ANALYSIS.md) --------------

void put32(std::vector<std::uint8_t> &f, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; i++) {
        f[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
    }
}

struct SegSpec {
    const char *name;
    std::uint32_t vmaddr, vmsize, fileoff, filesize, initprot;
};

constexpr std::uint32_t k_rx = MT_PROT_READ | MT_PROT_EXECUTE;
constexpr std::uint32_t k_rw = MT_PROT_READ | MT_PROT_WRITE;

// __TEXT holds the header (file offset 0), as in the real slice. Shortened file contents
// keep the fixture small; the vm layout is the real one.
const SegSpec k_layout[] = {
    {"__PAGEZERO", 0x0u, 0x1000u, 0u, 0u, 0u},
    {"__TEXT", 0x1000u, 0xF0000u, 0u, 0x3000u, k_rx},
    {"__DATA", 0xF1000u, 0x2A000u, 0x3000u, 0x1800u, k_rw},
    {"__LINKEDIT", 0x11B000u, 0xB000u, 0x4800u, 0x200u, MT_PROT_READ},
};

std::vector<std::uint8_t> macho(const SegSpec *segs, std::size_t n, std::size_t file_size) {
    std::vector<std::uint8_t> f(file_size);
    for (std::size_t i = 0; i < f.size(); i++) {
        f[i] = static_cast<std::uint8_t>(i * 7u + (i >> 8)); // recognisable bytes
    }
    put32(f, 0, 0xFEEDFACEu);
    put32(f, 4, 12u); // CPU_TYPE_ARM
    put32(f, 8, 9u);  // armv7
    put32(f, 12, 2u); // MH_EXECUTE
    put32(f, 16, static_cast<std::uint32_t>(n));
    put32(f, 20, static_cast<std::uint32_t>(56u * n));
    put32(f, 24, 0x10085u);
    for (std::size_t i = 0; i < n; i++) {
        const std::size_t at = 28u + 56u * i;
        put32(f, at, 1u); // LC_SEGMENT
        put32(f, at + 4, 56u);
        std::memset(&f[at + 8], 0, 16);
        std::memcpy(&f[at + 8], segs[i].name, std::strlen(segs[i].name));
        put32(f, at + 24, segs[i].vmaddr);
        put32(f, at + 28, segs[i].vmsize);
        put32(f, at + 32, segs[i].fileoff);
        put32(f, at + 36, segs[i].filesize);
        put32(f, at + 40, segs[i].initprot);
        put32(f, at + 44, segs[i].initprot);
        put32(f, at + 48, 0u);
        put32(f, at + 52, 0u);
    }
    return f;
}

std::vector<std::uint8_t> image() { return macho(k_layout, 4, 0x4A00u); }

// An address space with a cpu attached.
struct Space {
    mt_mem *mem;
    mt_cpu cpu{};
    explicit Space(std::uint32_t reservation = MT_MEM_RESERVE_AUTO) {
        mt_mem_config cfg = mt_mem_config_default();
        cfg.reservation = reservation;
        mem = mt_mem_create(&cfg);
        REQUIRE(mem != nullptr);
        mt_mem_attach(mem, &cpu);
    }
    ~Space() { mt_mem_destroy(mem); }
    Space(const Space &) = delete;
    Space &operator=(const Space &) = delete;
    std::uint8_t *host(std::uint32_t addr) const { return mt_mem_base(mem) + addr; }
};

std::string describe(mt_mem *mem, std::uint32_t addr, std::uint32_t *region = nullptr) {
    char buf[160];
    const std::uint32_t r = mt_mem_describe(mem, addr, buf, sizeof buf);
    if (region != nullptr) {
        *region = r;
    }
    return buf;
}

// Runs one guest access through the generated code's path; true if it trapped.
struct Access32 {
    mt_cpu *cpu;
    std::uint32_t addr;
    std::uint32_t pc;
    int kind; // 0: ld32, 1: st8, 2: ld64, 3: mt_g2h of 4 bytes, 4: mt_g2h_str
};
bool access_traps(const Access32 &a) {
    Access32 copy = a;
    return mt_test::stops(
        [](void *arg) {
            auto *x = static_cast<Access32 *>(arg);
            mt_cpu *cpu = x->cpu;
            switch (x->kind) {
            case 0:
                (void)MT_LD32(x->addr, x->pc);
                break;
            case 1:
                MT_ST8(x->addr, 1u, x->pc);
                break;
            case 2:
                (void)MT_LD64(x->addr, x->pc);
                break;
            case 3:
                (void)mt_g2h(cpu, x->addr, 4u);
                break;
            default:
                (void)mt_g2h_str(cpu, x->addr);
                break;
            }
        },
        &copy);
}

// xorshift32: a fixed sequence on every host.
struct Rng {
    std::uint32_t s;
    std::uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    std::uint32_t below(std::uint32_t n) { return next() % n; }
};

} // namespace

TEST_CASE("the address space is a 4 GiB reservation on 64-bit desktops, or the arena alone") {
    Space full(MT_MEM_RESERVE_AUTO);
    CHECK(full.cpu.mem == mt_mem_base(full.mem));
    CHECK(full.cpu.mem_size == MT_GUEST_ARENA_SIZE);
    CHECK(mt_mem_from_cpu(&full.cpu) == full.mem);
#if defined(__SWITCH__)
    // Horizon reserves the arena alone (ADR 0025's Horizon addendum): AUTO falls back to it.
    CHECK(mt_mem_is_full_reservation(full.mem) == 0);
#else
    if constexpr (sizeof(void *) == 8u) {
        CHECK(mt_mem_is_full_reservation(full.mem) == 1); // the desktop hosts we test on
    }
#endif
    Space arena(MT_MEM_RESERVE_ARENA); // the iOS model
    CHECK(mt_mem_is_full_reservation(arena.mem) == 0);
    CHECK(mt_mem_from_cpu(&arena.cpu) == arena.mem);
    CHECK(arena.cpu.mem != full.cpu.mem);

    mt_cpu stray{};
    std::uint8_t window[16];
    stray.mem = window;
    CHECK(mt_mem_from_cpu(&stray) == nullptr);

    const std::uint32_t page = mt_mem_host_page_size();
    CHECK((page & (page - 1u)) == 0u);
    CHECK(page >= 0x1000u);
    CHECK(MT_GUEST_RESERVE_GUARD % page == 0u);
}

#if !defined(__SWITCH__) // no 4 GiB reservation on Horizon
TEST_CASE("in the full reservation, every guest address beyond the arena faults in hardware") {
    Space s(MT_MEM_RESERVE_FULL);
    REQUIRE(mt_mem_is_full_reservation(s.mem) == 1);
    CHECK(faults(s.host(MT_GUEST_ARENA_SIZE), Access::read));
    CHECK(faults(s.host(0x80000000u), Access::read));
    CHECK(faults(s.host(0xFFFFFFFFu), Access::read));
    CHECK(faults(s.host(0xFFFFFFFFu) + 7, Access::read)); // the last byte of an 8-byte access
}
#else
TEST_CASE("on Horizon only the arena is reserved, and the full reservation is refused") {
    mt_mem_config cfg{MT_MEM_RESERVE_FULL};
    CHECK(mt_mem_create(&cfg) == nullptr);
    Space s(MT_MEM_RESERVE_ARENA);
    CHECK(faults(s.host(0u), Access::read)); // nothing is committed yet
    CHECK(faults(s.host(MT_GUEST_ARENA_SIZE - 4u), Access::read));
}
#endif

TEST_CASE("the image maps at its link-time addresses") {
    Space s;
    const std::vector<std::uint8_t> file = image();
    mt_segment segs[MT_MAX_SEGMENTS];
    std::uint32_t n = 0;
    REQUIRE(mt_macho_segments(file.data(), file.size(), segs, MT_MAX_SEGMENTS, &n) == 0);
    REQUIRE(n == 4u);
    CHECK(std::string(segs[1].name) == "__TEXT");
    CHECK(segs[2].vmaddr == 0xF1000u);
    CHECK(segs[3].initprot == MT_PROT_READ);

    REQUIRE(mt_mem_map_macho(s.mem, file.data(), file.size()) == 0);
    std::uint32_t mapped = 0;
    const mt_segment *m = mt_mem_segments(s.mem, &mapped);
    REQUIRE(mapped == 3u); // __PAGEZERO is never mapped
    CHECK(std::string(m[0].name) == "__TEXT");
    CHECK(std::string(m[2].name) == "__LINKEDIT");

    mt_cpu *cpu = &s.cpu;
    CHECK(MT_LD32(0x1000u, 0u) == 0xFEEDFACEu); // the header, at __TEXT's vmaddr
    CHECK(MT_LD8(0x27E0u, 0u) == file[0x17E0]); // the entry point's bytes
    CHECK(MT_LD8(0x3FFFu, 0u) == file[0x2FFF]); // the last file byte of __TEXT
    CHECK(MT_LD32(0x4000u, 0u) == 0u);          // __TEXT's vmsize beyond its file bytes
    CHECK(MT_LD8(0xF1000u, 0u) == file[0x3000]);
    CHECK(MT_LD8(0xF27FFu, 0u) == file[0x47FF]);
    CHECK(MT_LD32(0xF2800u, 0u) == 0u); // __DATA's zero fill (__bss)
    CHECK(MT_LD8(0x11B000u, 0u) == file[0x4800]);
    CHECK(MT_LD8(0x125FFFu, 0u) == 0u); // the last byte of __LINKEDIT

    CHECK(describe(s.mem, 0x27E0u) == "__TEXT+0x17e0");
    CHECK(describe(s.mem, 0xF1010u) == "__DATA+0x10");
    std::uint32_t region = 0;
    CHECK(describe(s.mem, 0x200000u, &region) == "image area, no segment");
    CHECK(region == MT_REGION_UNMAPPED);
}

TEST_CASE("sealing makes __TEXT read-only and keeps __DATA writable") {
    Space s;
    const std::vector<std::uint8_t> file = image();
    REQUIRE(mt_mem_map_macho(s.mem, file.data(), file.size()) == 0);
    CHECK_FALSE(faults(s.host(0x20000u), Access::write)); // writable until sealed
    REQUIRE(mt_mem_seal_image(s.mem) == 0);
    const std::uint32_t page = mt_mem_host_page_size();
#if defined(__SWITCH__)
    // Horizon cannot reprotect memory mapped with svcMapMemory: sealing is a no-op there and
    // __TEXT stays writable (ADR 0025's Horizon addendum).
    CHECK_FALSE(faults(s.host(0x20000u), Access::write));
    CHECK_FALSE(faults(s.host(0x11C000u), Access::write));
#else
    CHECK(faults(s.host(0x20000u), Access::write));
    CHECK(faults(s.host(0xE0000u), Access::write));
    CHECK(faults(s.host(0x11C000u), Access::write)); // __LINKEDIT
#endif
    CHECK_FALSE(faults(s.host(0x20000u), Access::read));
    CHECK_FALSE(faults(s.host(0xF2000u), Access::write)); // __DATA
    if (page == 0x1000u) {
        CHECK(faults(s.host(0u), Access::read)); // page zero is never mapped
    } else {
        // A 16 KiB host page holds page zero and the start of __TEXT: readable, not writable.
        CHECK_FALSE(faults(s.host(0u), Access::read));
        CHECK(faults(s.host(0u), Access::write));
    }
    CHECK(faults(s.host(0x130000u), Access::read)); // the image area beyond the segments
    const mt_segment late{"__LATE", 0x200000u, 0x1000u, 0u, 0u, k_rw};
    CHECK(mt_mem_map_segment(s.mem, &late, nullptr, 0u) == -1); // no mapping after sealing
}

TEST_CASE("malformed Mach-O files and segments are rejected") {
    std::vector<std::uint8_t> file = image();
    mt_segment segs[MT_MAX_SEGMENTS];
    std::uint32_t n = 0;
    CHECK(mt_macho_segments(file.data(), 20u, segs, MT_MAX_SEGMENTS, &n) == -1);
    CHECK(mt_macho_segments(file.data(), file.size(), segs, 3u, &n) == -1); // too many
    {
        auto bad = file;
        put32(bad, 0, 0xFEEDFACFu); // 64-bit
        CHECK(mt_macho_segments(bad.data(), bad.size(), segs, MT_MAX_SEGMENTS, &n) == -1);
    }
    {
        auto bad = file;
        put32(bad, 20, 0x100000u); // sizeofcmds past the end
        CHECK(mt_macho_segments(bad.data(), bad.size(), segs, MT_MAX_SEGMENTS, &n) == -1);
    }
    {
        auto bad = file;
        put32(bad, 28 + 56 + 4, 6u); // a command size that is not a multiple of 4
        CHECK(mt_macho_segments(bad.data(), bad.size(), segs, MT_MAX_SEGMENTS, &n) == -1);
    }
    {
        auto bad = file;
        put32(bad, 28 + 56 + 36, 0x100000u); // __TEXT's filesize past the end of the file
        CHECK(mt_macho_segments(bad.data(), bad.size(), segs, MT_MAX_SEGMENTS, &n) == -1);
    }
    {
        auto bad = file;
        put32(bad, 28 + 2 * 56 + 36, 0x30000u); // __DATA filesize > vmsize
        CHECK(mt_macho_segments(bad.data(), bad.size(), segs, MT_MAX_SEGMENTS, &n) == -1);
    }
    {
        auto bad = file;
        put32(bad, 28 + 56 + 28, 0xFFFFFFFFu); // __TEXT reaching beyond 4 GiB
        CHECK(mt_macho_segments(bad.data(), bad.size(), segs, MT_MAX_SEGMENTS, &n) == -1);
    }
    Space s;
    const mt_segment outside{"__HIGH", 0x00FFF000u, 0x2000u, 0u, 0u, k_rw};
    CHECK(mt_mem_map_segment(s.mem, &outside, nullptr, 0u) == -1);
    const mt_segment low{"__LOW", 0x800u, 0x1000u, 0u, 0u, k_rw};
    CHECK(mt_mem_map_segment(s.mem, &low, nullptr, 0u) == -1);
    const mt_segment a{"__A", 0x2000u, 0x2000u, 0u, 0x10u, k_rw};
    CHECK(mt_mem_map_segment(s.mem, &a, file.data(), file.size()) == 0);
    const mt_segment b{"__B", 0x3000u, 0x1000u, 0u, 0u, k_rw};
    CHECK(mt_mem_map_segment(s.mem, &b, nullptr, 0u) == -1); // overlaps __A
    const mt_segment c{"__C", 0x8000u, 0x1000u, 0x10000u, 0x10u, k_rw};
    CHECK(mt_mem_map_segment(s.mem, &c, file.data(), file.size()) == -1); // file range
}

TEST_CASE("heap blocks are 16-byte aligned, distinct, and follow Darwin's malloc rules") {
    Space s;
    mt_mem *m = s.mem;
    const std::uint32_t a = mt_heap_malloc(m, 1u);
    const std::uint32_t b = mt_heap_malloc(m, 1u);
    const std::uint32_t z = mt_heap_malloc(m, 0u); // a unique minimum-sized block
    CHECK(a >= MT_GUEST_HEAP_BASE);
    CHECK(a < MT_GUEST_HEAP_END);
    CHECK(a % MT_HEAP_ALIGN == 0u);
    CHECK(b != a);
    CHECK(z != 0u);
    CHECK(z != a);
    CHECK(z != b);
    CHECK(mt_heap_usable_size(m, a) == 16u);
    CHECK(mt_heap_usable_size(m, a + 4u) == 0u); // not the start of a block
    CHECK(mt_heap_usable_size(m, mt_heap_malloc(m, 17u)) == 32u);
    CHECK(mt_heap_free(m, a) == 0);
    CHECK(mt_heap_free(m, a) == -1);                    // double free
    CHECK(mt_heap_free(m, 0x1234u) == -1);              // not a block
    CHECK(mt_heap_free(m, b + 8u) == -1);               // inside a block
    CHECK(mt_heap_free(m, 0u) == 0);                    // free(NULL)
    CHECK(mt_heap_malloc(m, 16u) == a);                 // the freed block is reused first
    CHECK(mt_heap_malloc(m, 0x1F000000u) == 0u);        // larger than the heap
    CHECK(mt_heap_calloc(m, 0x10000u, 0x10001u) == 0u); // count * size overflows

    // Large blocks: best fit, and freed neighbours merge.
    const std::uint32_t l1 = mt_heap_malloc(m, 4096u);
    const std::uint32_t l2 = mt_heap_malloc(m, 4096u);
    const std::uint32_t l3 = mt_heap_malloc(m, 4096u);
    CHECK(l2 == l1 + 4096u);
    CHECK(mt_heap_free(m, l1) == 0);
    CHECK(mt_heap_free(m, l2) == 0);
    CHECK(mt_heap_malloc(m, 8192u) == l1); // the merged range
    (void)l3;

    mt_heap_stats st{};
    mt_heap_get_stats(m, &st);
    CHECK(st.top > MT_GUEST_HEAP_BASE);
    CHECK(st.committed >= st.top);
    CHECK(st.committed % 0x100000u == 0u);
}

TEST_CASE("calloc zeroes reused memory; realloc keeps the contents") {
    Space s;
    mt_mem *m = s.mem;
    const std::uint32_t a = mt_heap_malloc(m, 64u);
    std::memset(s.host(a), 0xAB, 64);
    CHECK(mt_heap_free(m, a) == 0);
    const std::uint32_t c = mt_heap_calloc(m, 4u, 16u);
    CHECK(c == a); // reused, and still zeroed
    CHECK(std::all_of(s.host(c), s.host(c) + 64, [](std::uint8_t v) { return v == 0u; }));

    // Small to large: moved, contents kept, old block freed.
    for (std::uint32_t i = 0; i < 64u; i++) {
        s.host(c)[i] = static_cast<std::uint8_t>(i);
    }
    const std::uint32_t big = mt_heap_realloc(m, c, 5000u);
    REQUIRE(big != 0u);
    CHECK(mt_heap_usable_size(m, c) == 0u);
    for (std::uint32_t i = 0; i < 64u; i++) {
        CHECK(s.host(big)[i] == i);
    }
    // Large grows in place at the top, and shrinks in place.
    const std::uint32_t grown = mt_heap_realloc(m, big, 20000u);
    CHECK(grown == big);
    CHECK(mt_heap_usable_size(m, grown) == 20000u);
    CHECK(mt_heap_realloc(m, grown, 3000u) == grown);
    CHECK(mt_heap_usable_size(m, grown) == 3008u);
    CHECK(s.host(grown)[63] == 63u);
    // realloc(p, 0): a new minimum-sized block, p freed.
    const std::uint32_t tiny = mt_heap_realloc(m, grown, 0u);
    CHECK(tiny != 0u);
    CHECK(tiny != grown);
    CHECK(mt_heap_usable_size(m, tiny) == 16u);
    CHECK(mt_heap_usable_size(m, grown) == 0u);
    // realloc of something that is not a block changes nothing.
    CHECK(mt_heap_realloc(m, 0x01234560u, 8u) == 0u);
    // realloc(0, n) is malloc.
    CHECK(mt_heap_usable_size(m, mt_heap_realloc(m, 0u, 40u)) == 48u);
}

TEST_CASE("memalign and the import allocator give aligned, zeroed blocks") {
    Space s;
    mt_mem *m = s.mem;
    (void)mt_heap_malloc(m, 24u); // move the top off any large alignment
    for (std::uint32_t align = 32u; align <= 0x10000u; align <<= 1) {
        const std::uint32_t p = mt_heap_memalign(m, align, 100u);
        REQUIRE(p != 0u);
        CHECK(p % align == 0u);
        CHECK(mt_heap_usable_size(m, p) == 112u);
    }
    CHECK(mt_heap_memalign(m, 24u, 8u) == 0u); // not a power of two
    CHECK(mt_heap_memalign(m, 0u, 8u) == 0u);
    CHECK(mt_heap_memalign(m, 4u, 8u) % MT_HEAP_ALIGN == 0u);

    const std::uint32_t d = mt_heap_malloc(m, 32u);
    std::memset(s.host(d), 0xFF, 32);
    CHECK(mt_heap_free(m, d) == 0);
    const std::uint32_t z = mt_mem_guest_alloc(m, 32u, 4u); // an mt_guest_alloc_fn
    CHECK(z % MT_HEAP_ALIGN == 0u);
    CHECK(std::all_of(s.host(z), s.host(z) + 32, [](std::uint8_t v) { return v == 0u; }));
    CHECK(mt_mem_guest_alloc(m, 16u, 4096u) % 4096u == 0u);
}

namespace {

// Runs a fixed random sequence of heap calls against a shadow model and returns a hash of
// every address it was given. Checks alignment, bounds, overlap and contents throughout.
std::uint64_t heap_stress(Space &s, std::uint32_t ops) {
    struct Block {
        std::uint32_t size;
        std::uint8_t fill;
    };
    std::map<std::uint32_t, Block> live;
    Rng rng{0x2A3C5u};
    std::uint64_t hash = 1469598103934665603ull;
    mt_mem *m = s.mem;
    auto pick_size = [&rng]() -> std::uint32_t {
        const std::uint32_t r = rng.below(100u);
        if (r < 70u) {
            return rng.below(1025u); // small classes, 0 included
        }
        if (r < 95u) {
            return 1025u + rng.below(64u * 1024u);
        }
        return 64u * 1024u + rng.below(512u * 1024u);
    };
    auto check_and_fill = [&](std::uint32_t addr, std::uint32_t size, std::uint8_t fill) {
        REQUIRE(addr != 0u);
        REQUIRE(addr % MT_HEAP_ALIGN == 0u);
        REQUIRE(addr >= MT_GUEST_HEAP_BASE);
        REQUIRE(static_cast<std::uint64_t>(addr) + size <= MT_GUEST_HEAP_END);
        REQUIRE(mt_heap_usable_size(m, addr) >= size);
        auto next = live.lower_bound(addr);
        if (next != live.end()) {
            REQUIRE(addr + std::max(size, 1u) <= next->first);
        }
        if (next != live.begin()) {
            auto prev = std::prev(next);
            REQUIRE(prev->first + std::max(prev->second.size, 1u) <= addr);
        }
        std::memset(s.host(addr), fill, size);
        live[addr] = Block{size, fill};
        hash = (hash ^ addr) * 1099511628211ull;
    };
    auto intact = [&](std::uint32_t addr, std::uint32_t n, std::uint8_t fill) {
        const std::uint8_t *p = s.host(addr);
        const std::uint32_t probes[] = {0u, n / 2u, n > 0u ? n - 1u : 0u};
        for (std::uint32_t at : probes) {
            for (std::uint32_t i = at; i < n && i < at + 32u; i++) {
                if (p[i] != fill) {
                    return false;
                }
            }
        }
        return true;
    };
    for (std::uint32_t op = 0; op < ops; op++) {
        const std::uint32_t r = rng.below(100u);
        const auto fill = static_cast<std::uint8_t>(op | 1u);
        if (live.empty() || (live.size() < 1500u && r < 45u)) {
            const std::uint32_t size = pick_size();
            const std::uint32_t kind = rng.below(10u);
            std::uint32_t addr;
            if (kind == 0u) {
                addr = mt_heap_calloc(m, 1u, size);
                REQUIRE(addr != 0u);
                REQUIRE(intact(addr, size, 0u));
            } else if (kind == 1u) {
                const std::uint32_t align = 32u << rng.below(8u);
                addr = mt_heap_memalign(m, align, size);
                REQUIRE(addr % align == 0u);
            } else {
                addr = mt_heap_malloc(m, size);
            }
            check_and_fill(addr, size, fill);
            continue;
        }
        // Pick a live block by position.
        auto it = live.begin();
        std::advance(it, static_cast<long>(rng.below(static_cast<std::uint32_t>(live.size()))));
        const std::uint32_t addr = it->first;
        const Block b = it->second;
        REQUIRE(intact(addr, b.size, b.fill));
        if (r < 75u) {
            REQUIRE(mt_heap_free(m, addr) == 0);
            live.erase(it);
        } else {
            const std::uint32_t size = pick_size();
            live.erase(it);
            const std::uint32_t moved = mt_heap_realloc(m, addr, size);
            REQUIRE(intact(moved, std::min(size, b.size), b.fill));
            check_and_fill(moved, size, fill);
        }
    }
    for (const auto &[addr, b] : live) {
        REQUIRE(intact(addr, b.size, b.fill));
        REQUIRE(mt_heap_free(m, addr) == 0);
    }
    mt_heap_stats st{};
    mt_heap_get_stats(m, &st);
    CHECK(st.live_blocks == 0u);
    CHECK(st.live_bytes == 0u);
    return hash;
}

} // namespace

TEST_CASE("heap stress: random calls against a shadow model, same addresses every run") {
    std::uint64_t first, second;
    {
        Space s;
        first = heap_stress(s, 40000u);
    }
    {
        Space s(MT_MEM_RESERVE_ARENA); // the iOS model gives the same addresses
        second = heap_stress(s, 40000u);
    }
    CHECK(first == second);
}

TEST_CASE("the heap reports exhaustion as 0 and keeps working") {
    Space s;
    mt_mem *m = s.mem;
    std::vector<std::uint32_t> blocks;
    for (;;) {
        const std::uint32_t p = mt_heap_malloc(m, 64u * 1024u * 1024u);
        if (p == 0u) {
            break;
        }
        blocks.push_back(p);
    }
#if defined(__SWITCH__)
    // In applet mode the console's memory runs out before the 480 MiB heap does: committing
    // fails, and the heap reports that as exhaustion too.
    CHECK(blocks.size() >= 1u);
    CHECK(blocks.size() <= (MT_GUEST_HEAP_END - MT_GUEST_HEAP_BASE) / (64u * 1024u * 1024u));
#else
    CHECK(blocks.size() == (MT_GUEST_HEAP_END - MT_GUEST_HEAP_BASE) / (64u * 1024u * 1024u));
#endif
    for (std::uint32_t p : blocks) {
        CHECK(mt_heap_free(m, p) == 0);
    }
    mt_heap_stats st{};
    mt_heap_get_stats(m, &st);
    CHECK(st.top == MT_GUEST_HEAP_BASE); // everything merged back into the top
    CHECK(mt_heap_malloc(m, 100u) != 0u);
}

TEST_CASE("guest stacks: sp at a 16-byte aligned top, a no-access guard below each") {
    Space s;
    mt_mem *m = s.mem;
    mt_stack main_stack{}, t1{}, t2{};
    REQUIRE(mt_stack_create(m, MT_STACK_MAIN_SIZE, &main_stack) == 0);
    REQUIRE(mt_stack_create(m, 0u, &t1) == 0); // the default thread size
    REQUIRE(mt_stack_create(m, 100u, &t2) == 0);
    CHECK(main_stack.guard == MT_GUEST_STACK_AREA);
    CHECK(main_stack.base == main_stack.guard + MT_STACK_GUARD);
    CHECK(main_stack.top - main_stack.base == MT_STACK_MAIN_SIZE);
    CHECK(t1.top - t1.base == MT_STACK_THREAD_SIZE);
    CHECK(t2.top - t2.base == MT_STACK_GRANULE);
    CHECK(t1.guard == main_stack.top);
    CHECK(main_stack.top % 16u == 0u);

    // The stack works from generated code; the guard faults in hardware.
    s.cpu.r[MT_SP] = main_stack.top;
    mt_cpu *cpu = &s.cpu;
    MT_ST32(main_stack.top - 4u, 0x11223344u, 0u);
    MT_ST32(main_stack.base, 0x55667788u, 0u);
    CHECK(MT_LD32(main_stack.top - 4u, 0u) == 0x11223344u);
    CHECK(faults(s.host(main_stack.base - 1u), Access::write));
    CHECK(faults(s.host(main_stack.guard), Access::read));
    CHECK(faults(s.host(t1.base - 4u), Access::write));

    std::uint32_t region = 0;
    CHECK(describe(m, main_stack.base - 4u, &region) ==
          "guard below guest stack 0x1f010000-0x1f110000");
    CHECK(region == MT_REGION_STACK_GUARD);
    describe(m, t1.top - 4u, &region);
    CHECK(region == MT_REGION_STACK);

    // A freed stack's slot is reused (first fit) and reads as zero again.
    MT_ST32(t1.top - 4u, 0xFFFFFFFFu, 0u);
    CHECK(mt_stack_destroy(m, &t1) == 0);
    CHECK(mt_stack_destroy(m, &t1) == -1);
    CHECK(faults(s.host(t1.top - 4u), Access::read));
    mt_stack again{};
    REQUIRE(mt_stack_create(m, MT_STACK_THREAD_SIZE, &again) == 0);
    CHECK(again.guard == t1.guard);
    CHECK(MT_LD32(again.top - 4u, 0u) == 0u);

    // The area fills up and says so.
    CHECK(mt_stack_create(m, MT_GUEST_STACK_AREA_END - MT_GUEST_STACK_AREA, &t1) == -1);
    int created = 0;
    mt_stack extra{};
    while (mt_stack_create(m, MT_STACK_THREAD_SIZE, &extra) == 0) {
        created++;
    }
    CHECK(created == 25); // (16 MiB - 1.75 MiB used) / (512 KiB + 64 KiB guard)
}

TEST_CASE("a checked access outside guest memory traps with the guest PC") {
    Space s;
    mt_mem_fault_info f{};

    // A 4-byte load straddling the end of guest memory, by the instruction at 0x2A3C4.
    CHECK(access_traps({&s.cpu, MT_GUEST_ARENA_SIZE - 2u, 0x0002A3C4u, 0}));
    CHECK(mt_test::g_trap.kind == MT_TRAP_MEMORY);
    CHECK(mt_test::g_trap.pc == 0x0002A3C4u);
    CHECK(mt_test::g_trap.detail == MT_GUEST_ARENA_SIZE - 2u);
    REQUIRE(mt_mem_last_fault(&f) == 0);
    CHECK(f.addr == MT_GUEST_ARENA_SIZE - 2u);
    CHECK(f.size == 4u);
    CHECK(f.pc == 0x0002A3C4u);
    CHECK(f.region == MT_REGION_OUTSIDE);
    CHECK(s.cpu.r[MT_PC] == 0x0002A3C4u);

    // A byte store at the top of the 32-bit space; an 8-byte load just past the arena.
    CHECK(access_traps({&s.cpu, 0xFFFFFFFFu, 0x00010001u, 1}));
    CHECK(mt_test::g_trap.pc == 0x00010001u);
    CHECK(mt_test::g_trap.detail == 0xFFFFFFFFu);
    CHECK(access_traps({&s.cpu, MT_GUEST_ARENA_SIZE - 4u, 0x00020002u, 2}));
    REQUIRE(mt_mem_last_fault(&f) == 0);
    CHECK(f.size == 8u);
    CHECK(f.pc == 0x00020002u);

    // In range: no trap.
    const std::uint32_t block = mt_heap_malloc(s.mem, 16u);
    CHECK_FALSE(access_traps({&s.cpu, block, 0x3000u, 0}));
    CHECK_FALSE(access_traps({&s.cpu, MT_GUEST_ARENA_SIZE - 4u, 0x3000u, 3}));
}

TEST_CASE("HLE pointer helpers check guest pointers and report the guest caller") {
    Space s;
    mt_cpu *cpu = &s.cpu;
    const std::uint32_t str = mt_heap_malloc(s.mem, 16u);
    std::memcpy(s.host(str), "MapleThief", 11);
    CHECK(std::string(mt_g2h_str(cpu, str)) == "MapleThief");
    CHECK(mt_g2h(cpu, str, 16u) == s.host(str));
    CHECK(mt_h2g(cpu, s.host(str) + 3) == str + 3u);
    CHECK(mt_h2g(cpu, nullptr) == 0u);

    // A null pointer passed to an HLE function: the PC is the guest caller (lr).
    cpu->r[MT_LR] = 0x0000D7C5u;
    CHECK(access_traps({cpu, 0x10u, 0u, 3}));
    CHECK(mt_test::g_trap.kind == MT_TRAP_MEMORY);
    CHECK(mt_test::g_trap.pc == 0x0000D7C5u);
    mt_mem_fault_info f{};
    REQUIRE(mt_mem_last_fault(&f) == 0);
    CHECK(f.region == MT_REGION_PAGE_ZERO);
    CHECK(access_traps({cpu, MT_GUEST_ARENA_SIZE - 2u, 0u, 3}));
    CHECK(access_traps({cpu, 0u, 0u, 4}));
}

TEST_CASE("describe names every region of the layout") {
    Space s;
    mt_mem *m = s.mem;
    std::uint32_t region = 0;
    CHECK(describe(m, 0x8u, &region) == "page zero (a null pointer)");
    CHECK(region == MT_REGION_PAGE_ZERO);
    const std::uint32_t block = mt_heap_malloc(m, 32u);
    char expect[64];
    std::snprintf(expect, sizeof expect, "heap block 0x%08x+0x8 (32 bytes)", block);
    CHECK(describe(m, block + 8u, &region) == expect);
    CHECK(region == MT_REGION_HEAP);
    describe(m, MT_GUEST_HEAP_END - 16u, &region);
    CHECK(region == MT_REGION_HEAP_UNUSED);
    describe(m, MT_GUEST_STACK_AREA + 0x100u, &region);
    CHECK(region == MT_REGION_STACK_UNUSED);
    CHECK(describe(m, 0x30000000u, &region) == "outside guest memory (it ends at 0x20000000)");
    CHECK(region == MT_REGION_OUTSIDE);
    CHECK(mt_mem_describe(nullptr, 0x27E0u, nullptr, 0u) == MT_REGION_UNMAPPED);
}
