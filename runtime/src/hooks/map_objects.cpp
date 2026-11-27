// A map's live object list (restored_content, #427 and #428); see map_objects.h.
//
// CMMap (docs/formats/obj.md): the object count +0x44, the Obj* array +0x48 with its count +0x4C
// and 6 at +0x50, the name array +0x54 with its count +0x58. Each Obj is 12 bytes: cell +0, kind
// +2, quest marker +4 (-1 none), visible +6, facing +8, frame +0xA.
#include "map_objects.h"

#include <cstring>

#include "maplethief/hle/abi.h"

namespace mt_map_objects_detail {

namespace {

using u32 = std::uint32_t;

constexpr u32 k_new_array = 0xF0E30u;    // operator new[]'s import stub
constexpr u32 k_delete_array = 0xF0E28u; // operator delete[]'s
constexpr u32 k_map_count = 0x44u;
constexpr u32 k_map_objs = 0x48u;
constexpr u32 k_map_objs_count = 0x4Cu;
constexpr u32 k_map_objs_step = 0x50u;
constexpr u32 k_map_names = 0x54u;
constexpr u32 k_map_names_count = 0x58u;
constexpr u32 k_array_step = 6u; // the loader's +0x50
constexpr u32 k_obj_size = 12u;
constexpr u32 k_obj_kind = 2u;
constexpr u32 k_obj_marker = 4u;
constexpr u32 k_obj_visible = 6u;
constexpr u32 k_max_count = 0xFFu; // the file's count is a byte

bool in_guest(const mt_cpu *cpu, u32 addr, u32 size) {
    return addr != 0u && static_cast<std::uint64_t>(addr) + size <= cpu->mem_size;
}

u32 read_word(const mt_cpu *cpu, u32 addr) {
    u32 v = 0;
    if (in_guest(cpu, addr, 4u)) {
        std::memcpy(&v, cpu->mem + addr, 4);
    }
    return v;
}

void write_word(mt_cpu *cpu, u32 addr, u32 v) { std::memcpy(cpu->mem + addr, &v, 4); }

void write_half(mt_cpu *cpu, u32 addr, std::uint16_t v) { std::memcpy(cpu->mem + addr, &v, 2); }

u32 call(mt_cpu *cpu, u32 fn, u32 arg, u32 caller) {
    return mt_call_guest(cpu, fn, &arg, 1u, caller);
}

// The list's count, or k_max_count + 1 when `map` holds no list the loader built.
u32 checked_count(const mt_cpu *cpu, u32 map) {
    if (!in_guest(cpu, map, k_map_names_count + 4u)) {
        return k_max_count + 1u;
    }
    const u32 n = read_word(cpu, map + k_map_count);
    if (n > k_max_count ||
        (n > 0u && (!in_guest(cpu, read_word(cpu, map + k_map_objs), 4u * n) ||
                    !in_guest(cpu, read_word(cpu, map + k_map_names), 4u * n)))) {
        return k_max_count + 1u;
    }
    return n;
}

} // namespace

std::uint32_t count(const mt_cpu *cpu, std::uint32_t map) noexcept {
    const u32 n = checked_count(cpu, map);
    return n > k_max_count ? 0u : n;
}

std::int32_t kind_at(const mt_cpu *cpu, std::uint32_t map, std::uint32_t i) noexcept {
    if (i >= count(cpu, map)) {
        return -1;
    }
    const u32 obj = read_word(cpu, read_word(cpu, map + k_map_objs) + 4u * i);
    std::uint16_t kind = 0;
    if (!in_guest(cpu, obj, k_obj_size)) {
        return -1;
    }
    std::memcpy(&kind, cpu->mem + obj + k_obj_kind, 2);
    return kind;
}

bool insert(mt_cpu *cpu, std::uint32_t map, std::uint32_t at, std::uint16_t cell,
            std::uint16_t kind, const char *name, std::uint32_t caller) noexcept {
    const u32 n = checked_count(cpu, map);
    if (n >= k_max_count || at > n) {
        return false; // not a list the loader built, or no room for one more
    }
    const u32 objs = read_word(cpu, map + k_map_objs);
    const u32 names = read_word(cpu, map + k_map_names);
    const u32 len = static_cast<u32>(std::strlen(name));
    const u32 new_objs = call(cpu, k_new_array, 4u * (n + 1u), caller);
    const u32 new_names = call(cpu, k_new_array, 4u * (n + 1u), caller);
    const u32 obj = call(cpu, k_new_array, k_obj_size, caller);
    const u32 text = call(cpu, k_new_array, len + 1u, caller);
    if (!in_guest(cpu, new_objs, 4u * (n + 1u)) || !in_guest(cpu, new_names, 4u * (n + 1u)) ||
        !in_guest(cpu, obj, k_obj_size) || !in_guest(cpu, text, len + 1u)) {
        return false;
    }
    // The objects before `at`, the new one, then the rest.
    std::memcpy(cpu->mem + new_objs, cpu->mem + objs, 4u * at);
    std::memcpy(cpu->mem + new_names, cpu->mem + names, 4u * at);
    std::memcpy(cpu->mem + new_objs + 4u * (at + 1u), cpu->mem + objs + 4u * at, 4u * (n - at));
    std::memcpy(cpu->mem + new_names + 4u * (at + 1u), cpu->mem + names + 4u * at, 4u * (n - at));
    std::memset(cpu->mem + obj, 0, k_obj_size); // facing 0, frame 0
    write_half(cpu, obj, cell);
    write_half(cpu, obj + k_obj_kind, kind);
    write_half(cpu, obj + k_obj_marker, 0xFFFFu);
    write_half(cpu, obj + k_obj_visible, 1u);
    std::memcpy(cpu->mem + text, name, len + 1u);
    write_word(cpu, new_objs + 4u * at, obj);
    write_word(cpu, new_names + 4u * at, text);
    write_word(cpu, map + k_map_count, n + 1u);
    write_word(cpu, map + k_map_objs, new_objs);
    write_word(cpu, map + k_map_objs_count, n + 1u);
    write_word(cpu, map + k_map_objs_step, k_array_step);
    write_word(cpu, map + k_map_names, new_names);
    write_word(cpu, map + k_map_names_count, n + 1u);
    if (n > 0u) {
        call(cpu, k_delete_array, objs, caller);
        call(cpu, k_delete_array, names, caller);
    }
    return true;
}

} // namespace mt_map_objects_detail
