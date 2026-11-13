// Synthetic zip files for the platform tests (save_tools_test.cpp, ipa_import_test.cpp): zips
// whose entries inflate to any size from a few kilobytes, to reach the zip reader's caps.
#ifndef MAPLETHIEF_TESTS_ZIP_FIXTURES_H
#define MAPLETHIEF_TESTS_ZIP_FIXTURES_H

#include <cstdint>
#include <string>
#include <vector>

#include "save_zip.h"

namespace zip_fixtures {

using Bytes = std::vector<std::uint8_t>;

// A zip of `names`, each `size` (at least 1) zero bytes deflated (fixed Huffman codes: one
// literal, then copies of 258 at distance 1): a few kilobytes that inflate to any size.
inline Bytes zip_of_zeros(const std::vector<std::string> &names, std::uint32_t size) {
    Bytes deflated;
    std::uint32_t acc = 0, bits = 0;
    const auto put = [&](std::uint32_t v, std::uint32_t n) { // LSB first
        for (std::uint32_t i = 0; i < n; i++) {
            acc |= ((v >> i) & 1u) << bits;
            if (++bits == 8) {
                deflated.push_back(static_cast<std::uint8_t>(acc));
                acc = bits = 0;
            }
        }
    };
    const auto code = [&](std::uint32_t c, std::uint32_t n) { // a Huffman code, MSB first
        for (std::uint32_t i = n; i-- > 0;) {
            put((c >> i) & 1u, 1);
        }
    };
    put(1, 1);     // the last block
    put(1, 2);     // fixed codes
    code(0x30, 8); // literal 0
    std::uint32_t left = size - 1;
    for (; left >= 258; left -= 258) {
        code(0xC5, 8); // length 258 (code 285)
        code(0, 5);    // distance 1
    }
    for (; left > 0; left--) {
        code(0x30, 8);
    }
    code(0, 7); // end of block
    if (bits != 0) {
        deflated.push_back(static_cast<std::uint8_t>(acc));
    }
    const std::uint32_t crc = maplethief::frontend::crc32(Bytes(size, 0));
    Bytes zip, central;
    const auto le = [](Bytes &b, std::uint32_t v, int n) {
        for (int i = 0; i < n; i++) {
            b.push_back(i < 4 ? static_cast<std::uint8_t>(v >> (8 * i)) : 0);
        }
    };
    for (const std::string &name : names) {
        const auto at = static_cast<std::uint32_t>(zip.size());
        le(zip, 0x04034b50u, 4);
        le(zip, 20, 2);
        le(zip, 0, 2);
        le(zip, 8, 2);
        le(zip, 0, 4); // time, date
        le(zip, crc, 4);
        le(zip, static_cast<std::uint32_t>(deflated.size()), 4);
        le(zip, size, 4);
        le(zip, static_cast<std::uint32_t>(name.size()), 2);
        le(zip, 0, 2);
        zip.insert(zip.end(), name.begin(), name.end());
        zip.insert(zip.end(), deflated.begin(), deflated.end());
        le(central, 0x02014b50u, 4);
        le(central, 20, 2);
        le(central, 20, 2);
        le(central, 0, 2);
        le(central, 8, 2);
        le(central, 0, 4);
        le(central, crc, 4);
        le(central, static_cast<std::uint32_t>(deflated.size()), 4);
        le(central, size, 4);
        le(central, static_cast<std::uint32_t>(name.size()), 2);
        le(central, 0, 2 + 2 + 2 + 2 + 4); // extra, comment, disk, internal, external
        le(central, at, 4);
        central.insert(central.end(), name.begin(), name.end());
    }
    const auto central_at = static_cast<std::uint32_t>(zip.size());
    zip.insert(zip.end(), central.begin(), central.end());
    le(zip, 0x06054b50u, 4);
    le(zip, 0, 4); // disks
    le(zip, static_cast<std::uint32_t>(names.size()), 2);
    le(zip, static_cast<std::uint32_t>(names.size()), 2);
    le(zip, static_cast<std::uint32_t>(central.size()), 4);
    le(zip, central_at, 4);
    le(zip, 0, 2);
    return zip;
}

} // namespace zip_fixtures

#endif // MAPLETHIEF_TESTS_ZIP_FIXTURES_H
