// A map's live object list, as CMMap::LoadObj 0xD259 builds it (docs/formats/obj.md): what the
// restored_content wrappers on that function share, Monster Carnival's NPC (#427) and the
// unreached maps' portal (#428). Guest thread only.
#ifndef MAPLETHIEF_HOOKS_MAP_OBJECTS_H
#define MAPLETHIEF_HOOKS_MAP_OBJECTS_H

#include <cstdint>

#include "maplethief/recomp/recomp.h"

namespace mt_map_objects_detail {

// The kind of object i of the CMMap at `map`, -1 past the list or where it is not a list the
// loader built.
std::int32_t kind_at(const mt_cpu *cpu, std::uint32_t map, std::uint32_t i) noexcept;
// The number of objects (CMMap +0x44), 0 where it is not a list the loader built.
std::uint32_t count(const mt_cpu *cpu, std::uint32_t map) noexcept;
// Builds the object and name arrays again one longer with the guest's new[] (0xF0E30, the
// loader's own allocator), the new object (cell, kind, quest marker -1, visible, facing 0,
// frame 0) and its name at index `at` (0 first, count() last), and frees the old arrays with
// delete[] (0xF0E28). Calls into the guest as `caller` (the hooked function). False when the list
// is not one the loader built or the guest's new[] failed; the list is then unchanged.
bool insert(mt_cpu *cpu, std::uint32_t map, std::uint32_t at, std::uint16_t cell,
            std::uint16_t kind, const char *name, std::uint32_t caller) noexcept;

} // namespace mt_map_objects_detail

#endif
