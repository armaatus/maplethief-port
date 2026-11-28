// Synthetic zip files for the platform tests (save_tools_test.cpp, ipa_import_test.cpp): zips
// whose entries inflate to any size from a few kilobytes, to reach the zip reader's caps, and
// deflated copies of a set of files (a repacked IPA, #350).
#ifndef MAPLETHIEF_TESTS_ZIP_FIXTURES_H
#define MAPLETHIEF_TESTS_ZIP_FIXTURES_H

#include <cstdint>
#include <string>
#include <vector>

#include "save_zip.h"

namespace zip_fixtures {

using Bytes = std::vector<std::uint8_t>;

// Deflate's bit writer: values LSB first, Huffman codes MSB first.
struct DeflateBits {
    Bytes out;
    std::uint32_t acc = 0, bits = 0;
    void put(std::uint32_t v, std::uint32_t n) {
        for (std::uint32_t i = 0; i < n; i++) {
            acc |= ((v >> i) & 1u) << bits;
            if (++bits == 8) {
                out.push_back(static_cast<std::uint8_t>(acc));
                acc = bits = 0;
            }
        }
    }
    void code(std::uint32_t c, std::uint32_t n) {
        for (std::uint32_t i = n; i-- > 0;) {
            put((c >> i) & 1u, 1);
        }
    }
    // A literal byte in the fixed codes.
    void literal(std::uint8_t b) {
        if (b < 144) {
            code(0x30u + b, 8);
        } else {
            code(0x190u + (b - 144u), 9);
        }
    }
    Bytes finish() {
        code(0, 7); // end of block
        if (bits != 0) {
            out.push_back(static_cast<std::uint8_t>(acc));
        }
        return out;
    }
};

// One deflated entry: its name, deflated data, and the CRC-32 and size of what it inflates to.
struct DeflatedEntry {
    std::string name;
    Bytes deflated;
    std::uint32_t crc = 0;
    std::uint32_t size = 0;
};

// A zip of `entries`, each deflated (method 8), in order.
inline Bytes zip_of_deflated(const std::vector<DeflatedEntry> &entries) {
    Bytes zip, central;
    const auto le = [](Bytes &b, std::uint32_t v, int n) {
        for (int i = 0; i < n; i++) {
            b.push_back(i < 4 ? static_cast<std::uint8_t>(v >> (8 * i)) : 0);
        }
    };
    for (const DeflatedEntry &e : entries) {
        const auto at = static_cast<std::uint32_t>(zip.size());
        le(zip, 0x04034b50u, 4);
        le(zip, 20, 2);
        le(zip, 0, 2);
        le(zip, 8, 2);
        le(zip, 0, 4); // time, date
        le(zip, e.crc, 4);
        le(zip, static_cast<std::uint32_t>(e.deflated.size()), 4);
        le(zip, e.size, 4);
        le(zip, static_cast<std::uint32_t>(e.name.size()), 2);
        le(zip, 0, 2);
        zip.insert(zip.end(), e.name.begin(), e.name.end());
        zip.insert(zip.end(), e.deflated.begin(), e.deflated.end());
        le(central, 0x02014b50u, 4);
        le(central, 20, 2);
        le(central, 20, 2);
        le(central, 0, 2);
        le(central, 8, 2);
        le(central, 0, 4);
        le(central, e.crc, 4);
        le(central, static_cast<std::uint32_t>(e.deflated.size()), 4);
        le(central, e.size, 4);
        le(central, static_cast<std::uint32_t>(e.name.size()), 2);
        le(central, 0, 2 + 2 + 2 + 2 + 4); // extra, comment, disk, internal, external
        le(central, at, 4);
        central.insert(central.end(), e.name.begin(), e.name.end());
    }
    const auto central_at = static_cast<std::uint32_t>(zip.size());
    zip.insert(zip.end(), central.begin(), central.end());
    le(zip, 0x06054b50u, 4);
    le(zip, 0, 4); // disks
    le(zip, static_cast<std::uint32_t>(entries.size()), 2);
    le(zip, static_cast<std::uint32_t>(entries.size()), 2);
    le(zip, static_cast<std::uint32_t>(central.size()), 4);
    le(zip, central_at, 4);
    le(zip, 0, 2);
    return zip;
}

// A zip of `names`, each `size` (at least 1) zero bytes deflated (fixed Huffman codes: one
// literal, then copies of 258 at distance 1): a few kilobytes that inflate to any size.
inline Bytes zip_of_zeros(const std::vector<std::string> &names, std::uint32_t size) {
    DeflateBits d;
    d.put(1, 1); // the last block
    d.put(1, 2); // fixed codes
    d.literal(0);
    std::uint32_t left = size - 1;
    for (; left >= 258; left -= 258) {
        d.code(0xC5, 8); // length 258 (code 285)
        d.code(0, 5);    // distance 1
    }
    for (; left > 0; left--) {
        d.literal(0);
    }
    const Bytes deflated = d.finish();
    const std::uint32_t crc = maplethief::frontend::crc32(Bytes(size, 0));
    std::vector<DeflatedEntry> entries;
    for (const std::string &name : names) {
        entries.push_back({name, deflated, crc, size});
    }
    return zip_of_deflated(entries);
}

// A zip of `entries`, each deflated as fixed-Huffman literals (another compression than
// zip_store's, as a repacking tool writes): the same files in other bytes.
inline Bytes zip_deflate(const std::vector<maplethief::frontend::ZipEntry> &entries) {
    std::vector<DeflatedEntry> out;
    for (const auto &e : entries) {
        DeflateBits d;
        d.put(1, 1); // the last block
        d.put(1, 2); // fixed codes
        for (const std::uint8_t b : e.bytes) {
            d.literal(b);
        }
        out.push_back({e.name, d.finish(), maplethief::frontend::crc32(e.bytes),
                       static_cast<std::uint32_t>(e.bytes.size())});
    }
    return zip_of_deflated(out);
}

} // namespace zip_fixtures

#endif // MAPLETHIEF_TESTS_ZIP_FIXTURES_H
