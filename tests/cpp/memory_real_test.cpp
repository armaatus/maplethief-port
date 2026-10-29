// The real armv7 slice in guest memory (ADR 0025, #42): every segment maps at its link-time
// address with the file's bytes, __TEXT is read-only after sealing, and the heap and a stack
// work next to the image. Needs work/slices/MapleThief.armv7 (scripts/bootstrap); exits 77,
// which CTest reports as skipped, without it. Prints what it mapped as evidence.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "host_fault.h"
#include "maplethief/memory.h"

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        g_failures++;
    }
}

// The armv7 layout (docs/ANALYSIS.md).
struct Expected {
    const char *name;
    std::uint32_t vmaddr, vmsize;
};
const Expected k_expected[] = {
    {"__TEXT", 0x1000u, 0xF0000u},
    {"__DATA", 0xF1000u, 0x2A000u},
    {"__LINKEDIT", 0x11B000u, 0xB000u},
};

} // namespace

int main() {
    std::ifstream in(MT_ARMV7_SLICE, std::ios::binary);
    if (!in) {
        std::printf("skipped: %s is missing (run scripts/bootstrap)\n", MT_ARMV7_SLICE);
        return 77;
    }
    const std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(in)),
                                         std::istreambuf_iterator<char>());
    std::printf("slice: %s (%zu bytes)\n", MT_ARMV7_SLICE, file.size());

    mt_mem *mem = mt_mem_create(nullptr);
    if (mem == nullptr) {
        std::printf("FAIL cannot create the address space\n");
        return 1;
    }
    mt_cpu state{};
    mt_mem_attach(mem, &state);
    mt_cpu *cpu = &state; // the name the access macros expect
    check(mt_mem_map_macho(mem, file.data(), file.size()) == 0, "mt_mem_map_macho");

    mt_segment segs[MT_MAX_SEGMENTS];
    std::uint32_t n = 0;
    check(mt_macho_segments(file.data(), file.size(), segs, MT_MAX_SEGMENTS, &n) == 0,
          "mt_macho_segments");
    std::uint32_t mapped = 0;
    const mt_segment *m = mt_mem_segments(mem, &mapped);
    check(mapped == 3u, "3 segments mapped (__PAGEZERO skipped)");
    for (std::uint32_t i = 0; i < mapped && i < 3u; i++) {
        const Expected &e = k_expected[i];
        std::printf("     %-10s 0x%08x-0x%08x file 0x%x+0x%x prot %u\n", m[i].name, m[i].vmaddr,
                    m[i].vmaddr + m[i].vmsize, m[i].fileoff, m[i].filesize, m[i].initprot);
        check(std::strcmp(m[i].name, e.name) == 0 && m[i].vmaddr == e.vmaddr &&
                  m[i].vmsize == e.vmsize,
              (std::string(e.name) + " at its link-time address and size").c_str());
        const std::uint8_t *guest = mt_mem_base(mem) + m[i].vmaddr;
        check(std::memcmp(guest, file.data() + m[i].fileoff, m[i].filesize) == 0,
              (std::string(e.name) + ": every file byte at vmaddr + offset").c_str());
        bool zero = true;
        for (std::uint32_t at = m[i].filesize; at < m[i].vmsize; at++) {
            zero = zero && guest[at] == 0u;
        }
        check(zero, (std::string(e.name) + ": the rest of vmsize is zero").c_str());
    }

    check(MT_LD32(0x1000u, 0u) == 0xFEEDFACEu, "the Mach-O header at 0x1000");
    // LC_UNIXTHREAD's entry point 0x27E0 (ARM start): the bytes at file offset 0x17E0.
    check(MT_LD32(0x27E0u, 0u) == (static_cast<std::uint32_t>(file[0x17E0]) |
                                   static_cast<std::uint32_t>(file[0x17E1]) << 8 |
                                   static_cast<std::uint32_t>(file[0x17E2]) << 16 |
                                   static_cast<std::uint32_t>(file[0x17E3]) << 24),
          "the entry point's instruction at 0x27E0");

    check(mt_mem_seal_image(mem) == 0, "mt_mem_seal_image");
    check(mt_test::faults(mt_mem_base(mem) + 0x27E0u, mt_test::Access::write),
          "__TEXT is read-only after sealing");
    check(!mt_test::faults(mt_mem_base(mem) + 0xF4000u, mt_test::Access::write),
          "__DATA stays writable");

    const std::uint32_t block = mt_heap_malloc(mem, 1000u);
    mt_stack stack{};
    check(block >= MT_GUEST_HEAP_BASE && block % MT_HEAP_ALIGN == 0u, "heap next to the image");
    check(mt_stack_create(mem, MT_STACK_MAIN_SIZE, &stack) == 0, "main stack next to the image");
    char where[128];
    mt_mem_describe(mem, 0x27E0u, where, sizeof where);
    std::printf("     describe(0x27e0) = %s\n", where);
    check(std::strcmp(where, "__TEXT+0x17e0") == 0, "describe names the segment");

    mt_mem_destroy(mem);
    std::printf("%s\n", g_failures == 0 ? "PASS" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
