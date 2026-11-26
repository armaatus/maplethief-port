// The run coverage's watchers (#295, ADR 0160): the script natives called and the maps entered,
// from the entries of two guest functions (mt_coverage_watch, maplethief/diag.h; the
// MT_TRACE_ENTRY build calls them on every entry). They only read.
//
// Lua::callNative(L, index, base) 0x9B37D switches on the native's symbol index (r1), 0-64; the
// names are g_scriptNativeNames 0x117950, 65 char* in the image (docs/re/script-vm.md). The map
// loader 0x86699 is (this, area, map, newArea) (docs/formats/mapprj.md): every map change,
// including the first map of a game.
#include "maplethief/events.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "maplethief/diag.h"
#include "maplethief/memory.h"

namespace {

using u32 = std::uint32_t;

constexpr u32 k_native_names = 0x117950u;
constexpr u32 k_native_count = 65u;

// Whether [addr, addr + n) lies in one of the image's segments (a synthetic guest has none).
bool in_image(const mt_cpu *cpu, u32 addr, u32 n) {
    const std::uint64_t end = static_cast<std::uint64_t>(addr) + n;
    mt_mem *mem = mt_mem_from_cpu(cpu);
    if (addr == 0 || end > cpu->mem_size || mem == nullptr) {
        return false;
    }
    u32 count = 0;
    const mt_segment *segs = mt_mem_segments(mem, &count);
    for (u32 i = 0; segs != nullptr && i < count; i++) {
        if (addr >= segs[i].vmaddr &&
            end <= static_cast<std::uint64_t>(segs[i].vmaddr) + segs[i].vmsize) {
            return true;
        }
    }
    return false;
}

// The name of native `index` from the game's table, or "".
void native_name(const mt_cpu *cpu, u32 index, char *out, std::size_t size) {
    out[0] = '\0';
    u32 ptr = 0;
    if (index >= k_native_count || !in_image(cpu, k_native_names + 4u * index, 4u)) {
        return;
    }
    std::memcpy(&ptr, cpu->mem + k_native_names + 4u * index, 4);
    std::size_t n = 0;
    while (n + 1u < size && in_image(cpu, ptr + static_cast<u32>(n), 1u)) {
        const char c = static_cast<char>(cpu->mem[ptr + n]);
        if (c == '\0' || c == ' ' || c == '\n') {
            break;
        }
        out[n++] = c;
    }
    out[n] = '\0';
}

void on_call_native(mt_cpu *cpu, u32) {
    const u32 index = cpu->r[1];
    if (mt_hit_count(MT_HIT_NATIVE, index) != 0) {
        return;
    }
    char name[64];
    native_name(cpu, index, name, sizeof name);
    char key[96];
    std::snprintf(key, sizeof key, "%u %s", index, name[0] != '\0' ? name : "?");
    mt_hit_add(MT_HIT_NATIVE, index, key);
}

void on_load_map(mt_cpu *cpu, u32) {
    const auto area = static_cast<std::int32_t>(cpu->r[1]);
    const auto map = static_cast<std::int32_t>(cpu->r[2]);
    const std::uint64_t id = (static_cast<std::uint64_t>(cpu->r[1]) << 32) | cpu->r[2];
    if (mt_hit_count(MT_HIT_MAP, id) != 0) {
        return;
    }
    char key[32];
    std::snprintf(key, sizeof key, "%d.%d", area, map);
    mt_hit_add(MT_HIT_MAP, id, key);
}

} // namespace

extern "C" void mt_run_hits_install(void) noexcept {
    mt_coverage_watch(MT_RUN_HITS_FN_CALL_NATIVE, on_call_native);
    mt_coverage_watch(MT_RUN_HITS_FN_LOAD_MAP, on_load_map);
}
