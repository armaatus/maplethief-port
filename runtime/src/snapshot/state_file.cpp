// The state file's writer and reader (#202, ADR 0105); the layout is in state_file.h.
#include "snapshot/state_file.h"

#include <cstring>
#include <string>

#include "cg/zlib.h"
#include "snapshot/snapshot_internal.h"

namespace mt_snap {

namespace {

std::string hex(const std::uint8_t *p, std::size_t n) {
    static const char k_digits[] = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < n; i++) {
        s += k_digits[p[i] >> 4];
        s += k_digits[p[i] & 15u];
    }
    return s;
}

} // namespace

bool write_state_file(const mt_snapshot_header &header, const std::vector<Section> &sections,
                      bool store, std::vector<std::uint8_t> *out, std::string *why) {
    Writer w;
    w.raw(k_magic, sizeof k_magic);
    w.u32(header.format);
    w.u32(static_cast<u32>(k_header_size));
    w.raw(header.binary_sha256, 32);
    char version[32] = {};
    std::memcpy(version, header.port_version,
                strnlen(header.port_version, sizeof header.port_version - 1));
    w.raw(version, sizeof version);
    w.u32(header.slot);
    w.u32(header.flags);
    w.u64(header.guest_unix_us);
    w.u32(static_cast<u32>(sections.size()));
    w.u32(mt_crc32(0, w.bytes().data(), w.bytes().size()));
    std::vector<std::uint8_t> packed;
    for (const Section &s : sections) {
        if (s.name.empty() || s.name.size() > k_max_name || s.bytes.size() > k_max_section) {
            *why = "section " + s.name + " cannot be written (" + std::to_string(s.bytes.size()) +
                   " bytes)";
            return false;
        }
        const std::vector<std::uint8_t> *stored = &s.bytes;
        u32 flags = 0;
        if (!store && !s.bytes.empty()) {
            if (!mt_cg::zlib_deflate(s.bytes.data(), s.bytes.size(), packed)) {
                *why = "section " + s.name + " could not be deflated";
                return false;
            }
            if (packed.size() < s.bytes.size()) {
                stored = &packed;
                flags = k_section_deflated;
            }
        }
        w.u8v(static_cast<std::uint8_t>(s.name.size()));
        w.raw(s.name.data(), s.name.size());
        w.u32(flags);
        w.u32(static_cast<u32>(s.bytes.size()));
        w.u32(static_cast<u32>(stored->size()));
        w.u32(mt_crc32(0, stored->data(), stored->size()));
        w.raw(stored->data(), stored->size());
    }
    *out = std::move(w.bytes());
    return true;
}

bool read_state_file(const std::uint8_t *bytes, std::size_t size, const mt_snapshot_expect *expect,
                     mt_snapshot_header *header, std::vector<Section> *sections, std::string *why) {
    if (bytes == nullptr || size < sizeof k_magic ||
        std::memcmp(bytes, k_magic, sizeof k_magic) != 0) {
        *why = "not a save state (wrong magic)";
        return false;
    }
    Reader r(bytes, size, "state file");
    r.raw(sizeof k_magic);
    mt_snapshot_header h{};
    u32 header_size = 0, crc = 0;
    if (!r.u32(&h.format)) {
        *why = "truncated header";
        return false;
    }
    if (h.format != MT_SNAPSHOT_FORMAT) {
        *why = "format version " + std::to_string(h.format) + ", this port reads " +
               std::to_string(MT_SNAPSHOT_FORMAT);
        return false;
    }
    if (size < k_header_size || !r.u32(&header_size) || header_size != k_header_size) {
        *why = "truncated or malformed header";
        return false;
    }
    r.raw(h.binary_sha256, 32);
    r.raw(h.port_version, 32);
    h.port_version[31] = 0;
    r.u32(&h.slot);
    r.u32(&h.flags);
    r.u64(&h.guest_unix_us);
    r.u32(&h.sections);
    r.u32(&crc);
    if (crc != mt_crc32(0, bytes, k_header_size - 4)) {
        *why = "the header's CRC does not match";
        return false;
    }
    if (expect != nullptr && expect->binary_sha256 != nullptr &&
        std::memcmp(expect->binary_sha256, h.binary_sha256, 32) != 0) {
        *why = "a state of another game binary (" + hex(h.binary_sha256, 4) + "..., this is " +
               hex(expect->binary_sha256, 4) + "...)";
        return false;
    }
    if (expect != nullptr && expect->slot != MT_SNAPSHOT_ANY_SLOT && h.slot != expect->slot) {
        *why = "a state of character slot " + std::to_string(h.slot) + ", not " +
               std::to_string(expect->slot);
        return false;
    }
    if ((h.flags & MT_SNAPSHOT_PARTIAL) != 0 && sections != nullptr) {
        *why = "a partial state (host states left out): it cannot be restored";
        return false;
    }
    std::vector<Section> out;
    for (u32 i = 0; i < h.sections; i++) {
        std::uint8_t name_len = 0;
        u32 flags = 0, raw_len = 0, stored_len = 0, sum = 0;
        std::string name;
        const std::uint8_t *name_p = nullptr;
        if (!r.u8v(&name_len) || name_len == 0 || name_len > k_max_name ||
            (name_p = r.raw(name_len)) == nullptr) {
            *why = "section " + std::to_string(i) + ": truncated or malformed name";
            return false;
        }
        name.assign(reinterpret_cast<const char *>(name_p), name_len);
        for (const char c : name) {
            if (c < 0x21 || c > 0x7E) {
                *why = "section " + std::to_string(i) + ": malformed name";
                return false;
            }
        }
        if (!r.u32(&flags) || !r.u32(&raw_len) || !r.u32(&stored_len) || !r.u32(&sum)) {
            *why = "section " + name + ": truncated";
            return false;
        }
        if (raw_len > k_max_section) {
            *why = "section " + name + ": oversized (" + std::to_string(raw_len) + " bytes)";
            return false;
        }
        if ((flags & ~k_section_deflated) != 0 ||
            ((flags & k_section_deflated) == 0 && stored_len != raw_len)) {
            *why = "section " + name + ": malformed lengths or flags";
            return false;
        }
        if (stored_len > r.left()) {
            *why = "section " + name + ": " + std::to_string(stored_len) +
                   " bytes run past the end of the file";
            return false;
        }
        const std::uint8_t *stored = r.raw(stored_len);
        if (mt_crc32(0, stored, stored_len) != sum) {
            *why = "section " + name + ": bad CRC";
            return false;
        }
        if (sections == nullptr) {
            continue;
        }
        Section s;
        s.name = std::move(name);
        if ((flags & k_section_deflated) != 0) {
            if (!mt_cg::zlib_inflate(stored, stored_len, raw_len, s.bytes)) {
                *why = "section " + s.name + ": does not inflate to its length";
                return false;
            }
        } else {
            s.bytes.assign(stored, stored + stored_len);
        }
        for (const Section &e : out) {
            if (e.name == s.name) {
                *why = "section " + s.name + " appears twice";
                return false;
            }
        }
        out.push_back(std::move(s));
    }
    if (r.left() != 0) {
        *why = std::to_string(r.left()) + " bytes after the last section";
        return false;
    }
    *header = h;
    if (sections != nullptr) {
        *sections = std::move(out);
    }
    return true;
}

const Section *find_section(const std::vector<Section> &sections, const char *name) {
    for (const Section &s : sections) {
        if (s.name == name) {
            return &s;
        }
    }
    return nullptr;
}

} // namespace mt_snap
