// The state file (#202, ADR 0105): a fixed header, then tagged sections. Little-endian.
//
//   header (104 bytes)
//     0   magic "MTSTATE\x1A"
//     8   u32 format version (MT_SNAPSHOT_FORMAT)
//     12  u32 header size (104)
//     16  u8[32] the armv7 slice's SHA-256
//     48  char[32] the port's version (MT_VERSION), NUL-padded
//     80  u32 character slot (MT_SNAPSHOT_ANY_SLOT: none)
//     84  u32 flags (mt_snapshot_meta.flags)
//     88  u64 the guest's time, Unix microseconds
//     96  u32 number of sections
//     100 u32 CRC-32 of bytes 0..99
//   then each section
//     u8 name length (1..k_max_name), the name (ASCII)
//     u32 flags (bit 0: deflated, a zlib stream; else stored)
//     u32 length of the payload (inflated)
//     u32 length stored in the file
//     u32 CRC-32 of the stored bytes
//     the stored bytes
//
// A reader skips sections it does not know (a later format can add one without breaking older
// readers) and refuses the file when one it needs is missing. Everything a reader takes from the
// file is checked before it is used: the header's CRC, every stored length against the bytes
// left, every payload length against k_max_section, every section's CRC before it is inflated,
// and the inflated length.
#ifndef MAPLETHIEF_RUNTIME_SNAPSHOT_STATE_FILE_H
#define MAPLETHIEF_RUNTIME_SNAPSHOT_STATE_FILE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "maplethief/snapshot.h"

namespace mt_snap {

inline constexpr char k_magic[8] = {'M', 'T', 'S', 'T', 'A', 'T', 'E', 0x1A};
inline constexpr std::size_t k_header_size = 104;
inline constexpr std::size_t k_max_name = 32;
// A payload larger than this is refused (the whole guest arena is 512 MiB).
inline constexpr std::uint32_t k_max_section = 0x30000000u;
inline constexpr std::uint32_t k_section_deflated = 1u;

struct Section {
    std::string name;
    std::vector<std::uint8_t> bytes; // the payload (inflated)
};

// The file of `header` (format, slot, flags, guest time, binary hash and port version as given;
// the section count is set here) and `sections`, each deflated unless `store`. False only if
// out of memory or a section is too large.
bool write_state_file(const mt_snapshot_header &header, const std::vector<Section> &sections,
                      bool store, std::vector<std::uint8_t> *out, std::string *why);

// Reads and checks a state file. With `payloads`, every section is inflated into *sections
// (else only checked). False with the reason for anything a reader cannot trust.
bool read_state_file(const std::uint8_t *bytes, std::size_t size, const mt_snapshot_expect *expect,
                     mt_snapshot_header *header, std::vector<Section> *sections, std::string *why);

// The section named `name`, or nullptr.
const Section *find_section(const std::vector<Section> &sections, const char *name);

} // namespace mt_snap

#endif // MAPLETHIEF_RUNTIME_SNAPSHOT_STATE_FILE_H
