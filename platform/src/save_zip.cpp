// Zip files for the save export and import (#148, ADR 0078); see save_zip.h.
#include "save_zip.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <set>
#include <utility>

// The raw inflate of the pinned stb_image (ADR 0003), a private copy (static linkage) as in
// runtime/src/cg/codec.cpp, so that the two can be linked into one program.
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace maplethief::frontend {

namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;

constexpr u32 kLocalSig = 0x04034B50;
constexpr u32 kCentralSig = 0x02014B50;
constexpr u32 kEndSig = 0x06054B50;
constexpr std::size_t kLocalSize = 30;
constexpr std::size_t kCentralSize = 46;
constexpr std::size_t kEndSize = 22;
constexpr u16 kUtf8Names = 1u << 11;
constexpr u16 kVersion = 20; // 2.0: deflate, folders

std::array<u32, 256> make_crc_table() {
    std::array<u32, 256> t{};
    for (u32 i = 0; i < 256; i++) {
        u32 c = i;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        t[i] = c;
    }
    return t;
}
const std::array<u32, 256> kCrcTable = make_crc_table();

void put16(std::vector<u8> &out, u32 v) {
    out.push_back(static_cast<u8>(v));
    out.push_back(static_cast<u8>(v >> 8));
}
void put32(std::vector<u8> &out, u32 v) {
    put16(out, v & 0xFFFF);
    put16(out, v >> 16);
}
u32 get16(const u8 *p) { return static_cast<u32>(p[0]) | static_cast<u32>(p[1]) << 8; }
u32 get32(const u8 *p) { return get16(p) | get16(p + 2) << 16; }

bool fail(std::string *why, const std::string &what) {
    if (why != nullptr) {
        *why = what;
    }
    return false;
}

} // namespace

std::uint32_t crc32(std::span<const std::uint8_t> bytes) {
    u32 c = 0xFFFFFFFFu;
    for (const u8 b : bytes) {
        c = kCrcTable[(c ^ b) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

DosTime dos_time(int year, int month, int day, int hour, int minute, int second) {
    DosTime t;
    if (year < 1980 || year > 2107) {
        return t;
    }
    t.date = static_cast<u16>((year - 1980) << 9 | month << 5 | day);
    t.time = static_cast<u16>(hour << 11 | minute << 5 | second / 2);
    return t;
}

std::vector<std::uint8_t> zip_store(std::span<const ZipEntry> entries, DosTime when) {
    std::vector<u8> out;
    std::vector<u8> central;
    for (const ZipEntry &e : entries) {
        const u32 crc = crc32(e.bytes);
        const u32 size = static_cast<u32>(e.bytes.size());
        const u32 offset = static_cast<u32>(out.size());
        const u32 name_size = static_cast<u32>(e.name.size());
        put32(out, kLocalSig);
        put16(out, kVersion);
        put16(out, kUtf8Names);
        put16(out, 0); // stored
        put16(out, when.time);
        put16(out, when.date);
        put32(out, crc);
        put32(out, size);
        put32(out, size);
        put16(out, name_size);
        put16(out, 0); // extra
        out.insert(out.end(), e.name.begin(), e.name.end());
        out.insert(out.end(), e.bytes.begin(), e.bytes.end());

        put32(central, kCentralSig);
        put16(central, 3u << 8 | kVersion); // made by: Unix, so the attributes below count
        put16(central, kVersion);
        put16(central, kUtf8Names);
        put16(central, 0);
        put16(central, when.time);
        put16(central, when.date);
        put32(central, crc);
        put32(central, size);
        put32(central, size);
        put16(central, name_size);
        put16(central, 0);              // extra
        put16(central, 0);              // comment
        put16(central, 0);              // disk
        put16(central, 0);              // internal attributes
        put32(central, 0100644u << 16); // a regular file, rw-r--r--
        put32(central, offset);
        central.insert(central.end(), e.name.begin(), e.name.end());
    }
    const u32 central_at = static_cast<u32>(out.size());
    out.insert(out.end(), central.begin(), central.end());
    put32(out, kEndSig);
    put16(out, 0);
    put16(out, 0);
    put16(out, static_cast<u32>(entries.size()));
    put16(out, static_cast<u32>(entries.size()));
    put32(out, static_cast<u32>(central.size()));
    put32(out, central_at);
    put16(out, 0); // comment
    return out;
}

bool zip_list(std::span<const std::uint8_t> zip, std::vector<ZipItem> *items, std::string *why) {
    items->clear();
    const u8 *z = zip.data();
    const std::size_t n = zip.size();
    if (n < kEndSize || n > 0xFFFFFFFFu) {
        return fail(why, "it is not a zip file (" + std::to_string(n) + " bytes)");
    }
    // The end record: the last one whose comment reaches the end of the file exactly.
    std::size_t end = n;
    for (std::size_t at = n - kEndSize + 1; at-- > 0 && n - at <= kEndSize + 0xFFFF;) {
        if (get32(z + at) == kEndSig && at + kEndSize + get16(z + at + 20) == n) {
            end = at;
            break;
        }
    }
    if (end == n) {
        return fail(why, "it is not a zip file (no end of central directory)");
    }
    const u8 *e = z + end;
    const u32 count = get16(e + 10);
    const u32 central_size = get32(e + 12);
    const u32 central_at = get32(e + 16);
    if (get16(e + 4) != 0 || get16(e + 6) != 0 || get16(e + 8) != count) {
        return fail(why, "it is a zip split over several files");
    }
    if (count == 0xFFFF || central_size == 0xFFFFFFFFu || central_at == 0xFFFFFFFFu) {
        return fail(why, "it is a zip64 file, which this reader does not take");
    }
    if (count > kMaxZipEntries) {
        return fail(why, "it has " + std::to_string(count) + " entries, more than " +
                             std::to_string(kMaxZipEntries));
    }
    if (static_cast<std::size_t>(central_at) + central_size > end) {
        return fail(why, "its central directory lies outside the file");
    }
    const std::size_t central_end = static_cast<std::size_t>(central_at) + central_size;
    items->reserve(count);
    std::size_t at = central_at;
    for (u32 i = 0; i < count; i++) {
        if (at + kCentralSize > central_end || get32(z + at) != kCentralSig) {
            items->clear();
            return fail(why, "entry " + std::to_string(i) + " of the central directory is damaged");
        }
        const u8 *c = z + at;
        const u32 name_size = get16(c + 28);
        const u32 extra_size = get16(c + 30);
        const u32 comment_size = get16(c + 32);
        const std::size_t next = at + kCentralSize + name_size + extra_size + comment_size;
        if (next > central_end) {
            items->clear();
            return fail(why, "entry " + std::to_string(i) + " of the central directory is damaged");
        }
        ZipItem item;
        item.name.assign(reinterpret_cast<const char *>(c + kCentralSize), name_size);
        item.made_by = static_cast<u16>(get16(c + 4));
        item.flags = static_cast<u16>(get16(c + 8));
        item.method = static_cast<u16>(get16(c + 10));
        item.crc = get32(c + 16);
        item.packed = get32(c + 20);
        item.size = get32(c + 24);
        item.external = get32(c + 38);
        item.local_at = get32(c + 42);
        items->push_back(std::move(item));
        at = next;
    }
    return true;
}

bool zip_extract(std::span<const std::uint8_t> zip, const ZipItem &item,
                 std::size_t max_entry_bytes, std::vector<std::uint8_t> *bytes, std::string *why) {
    bytes->clear();
    const u8 *z = zip.data();
    const std::size_t n = zip.size();
    const std::string &name = item.name;
    if (item.encrypted()) {
        return fail(why, name + " is encrypted");
    }
    if (item.method != 0 && item.method != 8) {
        return fail(why, name + " is compressed with method " + std::to_string(item.method) +
                             " (only stored and deflated entries can be read)");
    }
    if (item.size > max_entry_bytes) {
        return fail(why, name + " has " + std::to_string(item.size) + " bytes, more than " +
                             std::to_string(max_entry_bytes));
    }
    if (item.method == 0 && item.packed != item.size) {
        return fail(why, name + " is stored with two different sizes");
    }
    if (static_cast<std::size_t>(item.local_at) + kLocalSize > n ||
        get32(z + item.local_at) != kLocalSig) {
        return fail(why, name + "'s local header is damaged");
    }
    const std::size_t data_at = static_cast<std::size_t>(item.local_at) + kLocalSize +
                                get16(z + item.local_at + 26) + get16(z + item.local_at + 28);
    if (data_at > n || item.packed > n - data_at) {
        return fail(why, name + "'s data runs past the end of the file");
    }
    if (item.method == 0) {
        bytes->assign(z + data_at, z + data_at + item.size);
    } else if (item.size > 0 || item.packed > 0) {
        // At least 1 byte for an empty file: stb wants somewhere to point.
        bytes->resize(std::max<std::size_t>(item.size, 1));
        const int got = stbi_zlib_decode_noheader_buffer(
            reinterpret_cast<char *>(bytes->data()), static_cast<int>(bytes->size()),
            reinterpret_cast<const char *>(z + data_at), static_cast<int>(item.packed));
        if (got != static_cast<int>(item.size)) {
            bytes->clear();
            return fail(why, name + "'s compressed data is damaged");
        }
        bytes->resize(item.size);
    }
    if (crc32(*bytes) != item.crc) {
        bytes->clear();
        return fail(why, name + " is damaged (its CRC-32 does not match)");
    }
    return true;
}

bool zip_read(std::span<const std::uint8_t> zip, std::size_t max_entry_bytes,
              std::vector<ZipEntry> *entries, std::string *why) {
    return zip_read(
        zip, max_entry_bytes, SIZE_MAX, [](const std::string &) { return true; }, entries, why);
}

bool zip_read(std::span<const std::uint8_t> zip, std::size_t max_entry_bytes,
              std::size_t max_total_bytes, const std::function<bool(const std::string &)> &keep,
              std::vector<ZipEntry> *entries, std::string *why) {
    entries->clear();
    std::vector<ZipItem> items;
    if (!zip_list(zip, &items, why)) {
        return false;
    }
    // Every name first; only the files kept are inflated, within the total.
    std::set<std::string> names;
    std::vector<const ZipItem *> kept;
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < items.size(); i++) {
        ZipItem &item = items[i];
        for (char &ch : item.name) {
            ch = ch == '\\' ? '/' : ch;
        }
        if (item.name.empty() || item.name.find('\0') != std::string::npos) {
            return fail(why, "entry " + std::to_string(i) + " has no name");
        }
        if (item.is_dir()) {
            continue; // a folder
        }
        if (!names.insert(item.name).second) {
            return fail(why, item.name + " is in it twice");
        }
        if (!keep(item.name)) {
            continue;
        }
        total += item.size;
        if (total > max_total_bytes) {
            return fail(why, "its files come to more than " + std::to_string(max_total_bytes) +
                                 " bytes in all");
        }
        kept.push_back(&item);
    }
    for (const ZipItem *item : kept) {
        ZipEntry out{item->name, {}};
        if (!zip_extract(zip, *item, max_entry_bytes, &out.bytes, why)) {
            entries->clear();
            return false;
        }
        entries->push_back(std::move(out));
    }
    return true;
}

} // namespace maplethief::frontend
